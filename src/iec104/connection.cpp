/**
 * @file connection.cpp
 * @brief IEC 60870-5-104 Connection Manager Implementation
 */

#include "connection.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cstring>

// Thread-safe station ID storage for callbacks
static thread_local std::string g_currentStationId;

namespace tls104 {


// Helper function to extract timestamp from CP56Time2a (7-byte time)
// Returns milliseconds since epoch
static uint64_t extractCP56Timestamp(CP56Time2a ts) {
    if (ts == nullptr) {
        return 0;
    }
    // CP56Time2a_toMsTimestamp converts to milliseconds since epoch
    return CP56Time2a_toMsTimestamp(ts);
}

// Helper function to extract timestamp from CP24Time2a (3-byte time)
// CP24Time2a contains: milliseconds (0-1), minutes (2), so no hour info
// We use current time as base and adjust for minutes
static uint64_t extractCP24Timestamp(CP24Time2a ts) {
    if (ts == nullptr) {
        return 0;
    }

    int ms = CP24Time2a_getMillisecond(ts);
    int minute = CP24Time2a_getMinute(ts);

    // Get current time as base
    auto now = std::chrono::system_clock::now();
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    // Get current time components
    std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm* tmNow = std::localtime(&nowTime);

    // Calculate minute difference and adjust
    int currentMinute = tmNow->tm_min;
    int minuteDiff = minute - currentMinute;

    // Handle wraparound (e.g., 59 -> 0)
    if (minuteDiff < -30) {
        minuteDiff += 60;
    } else if (minuteDiff > 30) {
        minuteDiff -= 60;
    }

    // Adjust timestamp by minute difference
    return nowMs + (minuteDiff * 60 * 1000) + ms;
}

IEC104ConnectionManager::IEC104ConnectionManager() {
    // TLS is not supported in this build
    g_tlsConfig = nullptr;
}

IEC104ConnectionManager::~IEC104ConnectionManager() {
    // Disconnect all stations
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& pair : connections_) {
        auto& info = pair.second;
        if (info && info->connection) {
            CS104_Connection_destroy(info->connection);
        }
    }
    connections_.clear();

#if defined(LIB60870_HAS_TLS_SUPPORT)
    if (g_tlsConfig) {
        TLSConfiguration_destroy(g_tlsConfig);
    }
#endif
}

bool IEC104ConnectionManager::addStation(const StationConfig& config) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    if (connections_.find(config.id) != connections_.end()) {
        std::cerr << "[IEC104] Station already exists: " << config.id << std::endl;
        return false;
    }

    auto info = std::make_unique<IEC104ConnectionInfo>();
    info->stationId = config.id;
    info->host = config.host;
    info->port = config.port;
    info->useTLS = config.useTLS;
    info->caFile = config.caFile;
    info->certFile = config.certFile;
    info->keyFile = config.keyFile;
    info->commonAddress = config.commonAddress > 0 ? config.commonAddress : 1;
    info->tlsVersion = config.tlsVersion.empty() ? "1.2" : config.tlsVersion;
    info->status = ConnectionStatus::CONNECTING;
    info->shouldReconnect = true;
    info->holder = this;  // Store manager pointer

    IEC104ConnectionInfo* infoPtr = info.get();
    connections_[config.id] = std::move(info);

    // Connect in a separate thread
    std::thread t([this, infoPtr]() {
        connectThreadFunc(infoPtr->stationId, infoPtr);
    });
    t.detach();

    std::cout << "[IEC104] Adding station: " << config.id << std::endl;
    return true;
}

bool IEC104ConnectionManager::removeStation(const std::string& stationId) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end()) {
        std::cerr << "[IEC104] Station not found: " << stationId << std::endl;
        return false;
    }

    auto& info = it->second;
    if (info) {
        info->shouldReconnect = false;

        if (info->connection) {
            CS104_Connection_destroy(info->connection);
            info->connection = nullptr;
        }

        if (info->tlsConfig) {
            TLSConfiguration_destroy(info->tlsConfig);
            info->tlsConfig = nullptr;
        }
    }

    connections_.erase(it);
    std::cout << "[IEC104] Removed station: " << stationId << std::endl;
    return true;
}

bool IEC104ConnectionManager::disconnectStation(const std::string& stationId) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end()) {
        std::cerr << "[IEC104] Station not found: " << stationId << std::endl;
        return false;
    }

    auto& info = it->second;
    if (info) {
        info->shouldReconnect = false;

        if (info->connection) {
            CS104_Connection_destroy(info->connection);
            info->connection = nullptr;
        }

        if (info->tlsConfig) {
            TLSConfiguration_destroy(info->tlsConfig);
            info->tlsConfig = nullptr;
        }

        info->status = ConnectionStatus::DISCONNECTED;
    }

    std::cout << "[IEC104] Disconnected station: " << stationId << std::endl;

    if (connectionCallback_) {
        connectionCallback_(stationId, ConnectionStatus::DISCONNECTED, "Manually disconnected");
    }

    return true;
}

bool IEC104ConnectionManager::connectStation(const std::string& stationId) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end()) {
        std::cerr << "[IEC104] Station not found: " << stationId << std::endl;
        return false;
    }

    auto& info = it->second;
    if (!info) return false;

    // Already connected
    if (info->connection && info->status == ConnectionStatus::CONNECTED) {
        std::cout << "[IEC104] Station already connected: " << stationId << std::endl;
        return true;
    }

    info->shouldReconnect = true;
    info->status = ConnectionStatus::CONNECTING;

    if (connectionCallback_) {
        connectionCallback_(stationId, ConnectionStatus::CONNECTING, "Connecting");
    }

    IEC104ConnectionInfo* infoPtr = info.get();
    std::thread t([this, stationId, infoPtr]() {
        connectThreadFunc(stationId, infoPtr);
    });
    t.detach();

    std::cout << "[IEC104] Connecting station: " << stationId << std::endl;
    return true;
}

void IEC104ConnectionManager::connectThreadFunc(const std::string& stationId, IEC104ConnectionInfo* info) {
    std::cout << "[IEC104] Connecting to " << stationId << "..." << std::endl;

    info->connection = createConnection(*info);
    if (!info->connection) {
        std::cerr << "[IEC104] Failed to create connection: " << stationId << std::endl;
        info->status = ConnectionStatus::CONN_ERROR;
        if (connectionCallback_) {
            connectionCallback_(stationId, ConnectionStatus::CONN_ERROR, "Failed to create connection");
        }
        return;
    }

    // Set handlers - pass info pointer to get station_id in callbacks
    CS104_Connection_setASDUReceivedHandler(info->connection, asduReceivedHandler, info);
    CS104_Connection_setConnectionHandler(info->connection, connectionHandler, info);
    CS104_Connection_setRawMessageHandler(info->connection, rawMessageHandler, info);

    // Connect
    if (!CS104_Connection_connect(info->connection)) {
        std::cerr << "[IEC104] Connection failed: " << stationId << std::endl;
        info->status = ConnectionStatus::CONN_ERROR;
        CS104_Connection_destroy(info->connection);
        info->connection = nullptr;

        if (connectionCallback_) {
            connectionCallback_(stationId, ConnectionStatus::CONN_ERROR, "Connection failed");
        }

        // Start reconnect thread
        if (info->shouldReconnect) {
            if (!info->reconnectMutex) info->reconnectMutex = std::make_unique<std::mutex>();
            std::lock_guard<std::mutex> lock(*info->reconnectMutex);
            if (!info->reconnectThreadRunning) {
                info->reconnectThreadRunning = true;
                std::thread t([this, stationId, info]() {
                    reconnectThreadFunc(stationId, info);
                    if (info->reconnectMutex) {
                        std::lock_guard<std::mutex> lock(*info->reconnectMutex);
                        info->reconnectThreadRunning = false;
                    }
                });
                t.detach();
            }
        }
        return;
    }

    // Send STARTDT ACT
    CS104_Connection_sendStartDT(info->connection);
    info->status = ConnectionStatus::CONNECTED;

    std::cout << "[IEC104] Connected: " << stationId << std::endl;

    if (connectionCallback_) {
        connectionCallback_(stationId, ConnectionStatus::CONNECTED, "Connected");
    }
}

void IEC104ConnectionManager::reconnectThreadFunc(const std::string& stationId, IEC104ConnectionInfo* info) {
    int retryCount = 0;
    const int maxRetries = 10;
    const int baseDelayMs = 1000;

    while (info->shouldReconnect && retryCount < maxRetries) {
        // Exponential backoff
        int delayMs = baseDelayMs * (1 << std::min(retryCount, 5));
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));

        if (!info->shouldReconnect) break;

        std::cout << "[IEC104] Reconnecting to " << stationId << " (attempt " << (retryCount + 1) << ")..." << std::endl;

        // Destroy old connection
        if (info->connection) {
            CS104_Connection_destroy(info->connection);
            info->connection = nullptr;
        }

        // Create new connection
        info->connection = createConnection(*info);
        if (!info->connection) {
            retryCount++;
            continue;
        }

        // Set handlers - pass info pointer to get station_id in callbacks
        CS104_Connection_setASDUReceivedHandler(info->connection, asduReceivedHandler, info);
        CS104_Connection_setConnectionHandler(info->connection, connectionHandler, info);

        // Try to connect
        if (CS104_Connection_connect(info->connection)) {
            CS104_Connection_sendStartDT(info->connection);
            info->status = ConnectionStatus::CONNECTED;

            std::cout << "[IEC104] Reconnected: " << stationId << std::endl;

            if (connectionCallback_) {
                connectionCallback_(stationId, ConnectionStatus::CONNECTED, "Reconnected");
            }
            return;
        }

        retryCount++;
    }

    std::cerr << "[IEC104] Max retries reached: " << stationId << std::endl;
    info->status = ConnectionStatus::CONN_ERROR;

    if (connectionCallback_) {
        connectionCallback_(stationId, ConnectionStatus::CONN_ERROR, "Max retries reached");
    }
}

CS104_Connection IEC104ConnectionManager::createConnection(IEC104ConnectionInfo& info) {
    CS104_Connection conn = nullptr;

    if (info.useTLS) {
        // Create TLS connection
        std::cout << "[IEC104] Creating TLS connection to " << info.host << ":" << info.port << std::endl;

        // Destroy any previous TLS config before creating a new one
        if (info.tlsConfig) {
            TLSConfiguration_destroy(info.tlsConfig);
            info.tlsConfig = nullptr;
        }

        // Create TLS configuration (stored in info so it can be properly destroyed later)
        info.tlsConfig = TLSConfiguration_create();
        TLSConfiguration_setClientMode(info.tlsConfig);

        // Set TLS version (IEC 62351 requires TLS 1.2+)
        if (info.tlsVersion == "1.3") {
            TLSConfiguration_setMinTlsVersion(info.tlsConfig, TLS_VERSION_TLS_1_3);
            std::cout << "[IEC104] TLS version set to 1.3" << std::endl;
        } else {
            TLSConfiguration_setMinTlsVersion(info.tlsConfig, TLS_VERSION_TLS_1_2);
            std::cout << "[IEC104] TLS version set to 1.2" << std::endl;
        }

        // Set TLS event handler for detailed debugging
        auto tlsEventHandler = [](void* param, TLSEventLevel level, int eventCode, const char* message, TLSConnection con) {
            const char* levelStr = (level == TLS_SEC_EVT_INFO) ? "INFO" :
                                   (level == TLS_SEC_EVT_WARNING) ? "WARNING" : "ERROR";
            std::cout << "[IEC104 TLS] " << levelStr << " eventCode=" << eventCode << " msg=" << (message ? message : "null") << std::endl;
        };
        TLSConfiguration_setEventHandler(info.tlsConfig, tlsEventHandler, nullptr);

        // Set CA certificate for verifying server
        if (!info.caFile.empty()) {
            if (TLSConfiguration_addCACertificateFromFile(info.tlsConfig, info.caFile.c_str())) {
                std::cout << "[IEC104] Loaded CA certificate: " << info.caFile << std::endl;
            } else {
                std::cerr << "[IEC104] Failed to load CA certificate: " << info.caFile << std::endl;
            }
        }

        // Set client certificate for mutual authentication
        if (!info.certFile.empty()) {
            if (TLSConfiguration_setOwnCertificateFromFile(info.tlsConfig, info.certFile.c_str())) {
                std::cout << "[IEC104] Loaded client certificate: " << info.certFile << std::endl;
            } else {
                std::cerr << "[IEC104] Failed to load client certificate: " << info.certFile << std::endl;
            }
        }

        // Set client private key
        if (!info.keyFile.empty()) {
            if (TLSConfiguration_setOwnKeyFromFile(info.tlsConfig, info.keyFile.c_str(), nullptr)) {
                std::cout << "[IEC104] Loaded client key: " << info.keyFile << std::endl;
            } else {
                std::cerr << "[IEC104] Failed to load client key: " << info.keyFile << std::endl;
            }
        }

        // Create secure connection
        conn = CS104_Connection_createSecure(info.host.c_str(), info.port, info.tlsConfig);

        if (conn) {
            CS104_Connection_setConnectTimeout(conn, 10);
            std::cout << "[IEC104] TLS connection created successfully" << std::endl;
        } else {
            std::cerr << "[IEC104] Failed to create TLS connection" << std::endl;
            TLSConfiguration_destroy(info.tlsConfig);
            info.tlsConfig = nullptr;
        }
    } else {
        // Create plain TCP connection
        conn = CS104_Connection_create(info.host.c_str(), info.port);

        if (conn) {
            // Set connection timeout (10 seconds)
            CS104_Connection_setConnectTimeout(conn, 10);
        }
    }

    return conn;
}

bool IEC104ConnectionManager::sendInterrogation(const std::string& stationId, int ca) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end() || !it->second || !it->second->connection) {
        std::cerr << "[IEC104] Station not connected: " << stationId << std::endl;
        return false;
    }

    bool result = CS104_Connection_sendInterrogationCommand(
        it->second->connection,
        CS101_COT_ACTIVATION,
        ca,
        IEC60870_QOI_STATION
    );

    std::cout << "[IEC104] Send interrogation to " << stationId
              << " (CA=" << ca << "): " << (result ? "OK" : "FAILED") << std::endl;
    return result;
}

bool IEC104ConnectionManager::sendClockSync(const std::string& stationId, int ca) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end() || !it->second || !it->second->connection) {
        std::cerr << "[IEC104] Station not connected: " << stationId << std::endl;
        return false;
    }

    // Get current time in milliseconds since epoch
    auto now = std::chrono::system_clock::now();
    uint64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    CP56Time2a time = CP56Time2a_createFromMsTimestamp(nullptr, ms);

    bool result = CS104_Connection_sendClockSyncCommand(it->second->connection, ca, time);

    std::cout << "[IEC104] Send clock sync to " << stationId
              << " (CA=" << ca << "): " << (result ? "OK" : "FAILED") << std::endl;
    return result;
}

bool IEC104ConnectionManager::sendCounterRead(const std::string& stationId, int ca) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end() || !it->second || !it->second->connection) {
        return false;
    }

    bool result = CS104_Connection_sendCounterInterrogationCommand(
        it->second->connection,
        CS101_COT_ACTIVATION,
        ca,
        IEC60870_QCC_RQT_GENERAL | IEC60870_QCC_FRZ_FREEZE_WITHOUT_RESET
    );

    std::cout << "[IEC104] Send counter read to " << stationId << std::endl;
    return result;
}

bool IEC104ConnectionManager::sendControl(const std::string& stationId, const ControlCommand& cmd) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end() || !it->second || !it->second->connection) {
        std::cerr << "[IEC104] Station not connected: " << stationId << std::endl;
        return false;
    }

    bool result = false;

    if (cmd.type == "single") {
        // C_SC_NA_1 - Single command (Type 45)
        auto sc = SingleCommand_create(nullptr, cmd.ioa, cmd.value != 0, cmd.select, false);
        result = CS104_Connection_sendProcessCommandEx(it->second->connection, CS101_COT_ACTIVATION, cmd.ca, (InformationObject)sc);
        SingleCommand_destroy(sc);
    } else if (cmd.type == "double") {
        // C_DC_NA_1 - Double command (Type 46)
        auto dc = DoubleCommand_create(nullptr, cmd.ioa, cmd.value, cmd.select, false);
        result = CS104_Connection_sendProcessCommandEx(it->second->connection, CS101_COT_ACTIVATION, cmd.ca, (InformationObject)dc);
        DoubleCommand_destroy(dc);
    } else if (cmd.type == "step") {
        // C_RC_NA_1 - Step command (Type 47)
        // value: 1 = lower (退), 2 = higher (进)
        StepCommandValue stepValue = (cmd.value == 2) ? IEC60870_STEP_HIGHER : IEC60870_STEP_LOWER;
        auto sc = StepCommand_create(nullptr, cmd.ioa, stepValue, cmd.select, false);
        result = CS104_Connection_sendProcessCommandEx(it->second->connection, CS101_COT_ACTIVATION, cmd.ca, (InformationObject)sc);
        StepCommand_destroy(sc);
    } else if (cmd.type == "normalized") {
        // C_SE_NA_1 - Normalized setpoint command (Type 48)
        // value is normalized (-1.0 ~ 1.0), stored as int16_t
        float normalizedValue = (float)cmd.value / 32767.0f;
        auto sp = SetpointCommandNormalized_create(nullptr, cmd.ioa, normalizedValue, cmd.select, false);
        result = CS104_Connection_sendProcessCommandEx(it->second->connection, CS101_COT_ACTIVATION, cmd.ca, (InformationObject)sp);
        SetpointCommandNormalized_destroy(sp);
    } else if (cmd.type == "scaled") {
        // C_SE_NB_1 - Scaled setpoint command (Type 49)
        auto sp = SetpointCommandScaled_create(nullptr, cmd.ioa, cmd.value, cmd.select, false);
        result = CS104_Connection_sendProcessCommandEx(it->second->connection, CS101_COT_ACTIVATION, cmd.ca, (InformationObject)sp);
        SetpointCommandScaled_destroy(sp);
    } else if (cmd.type == "float") {
        // C_SE_NC_1 - Float setpoint command (Type 50)
        float floatValue = static_cast<float>(cmd.value);
        auto sp = SetpointCommandShort_create(nullptr, cmd.ioa, floatValue, cmd.select, false);
        result = CS104_Connection_sendProcessCommandEx(it->second->connection, CS101_COT_ACTIVATION, cmd.ca, (InformationObject)sp);
        SetpointCommandShort_destroy(sp);
    } else if (cmd.type == "bitstring") {
        // C_BO_NA_1 - Bitstring command (Type 51)
        // Note: Bitstring32Command doesn't have select/qu parameters
        auto bs = Bitstring32Command_create(nullptr, cmd.ioa, cmd.value);
        result = CS104_Connection_sendProcessCommandEx(it->second->connection, CS101_COT_ACTIVATION, cmd.ca, (InformationObject)bs);
        Bitstring32Command_destroy(bs);
    }

    std::cout << "[IEC104] Send control to " << stationId
              << " IOA=" << cmd.ioa << " type=" << cmd.type << ": " << (result ? "OK" : "FAILED") << std::endl;
    return result;
}

void IEC104ConnectionManager::setConnectionCallback(ConnectionCallback cb) {
    connectionCallback_ = cb;
}

void IEC104ConnectionManager::setDataCallback(DataCallback cb) {
    dataCallback_ = cb;
}

void IEC104ConnectionManager::setASDUDataCallback(ASDUDataCallback cb) {
    asduDataCallback_ = cb;
}

void IEC104ConnectionManager::setPacketCallback(PacketCallback cb) {
    packetCallback_ = cb;
}

void IEC104ConnectionManager::setControlResultCallback(ControlResultCallback cb) {
    controlResultCallback_ = cb;
}

// IPCBridgeCallback implementation
void IEC104ConnectionManager::onAddStation(const StationConfig& config) {
    addStation(config);
}

void IEC104ConnectionManager::onRemoveStation(const std::string& stationId) {
    removeStation(stationId);
}

void IEC104ConnectionManager::onDisconnectStation(const std::string& stationId) {
    disconnectStation(stationId);
}

void IEC104ConnectionManager::onConnectStation(const std::string& stationId) {
    connectStation(stationId);
}

void IEC104ConnectionManager::onSendInterrogation(const std::string& stationId, int ca) {
    sendInterrogation(stationId, ca);
}

void IEC104ConnectionManager::onSendClockSync(const std::string& stationId, int ca) {
    sendClockSync(stationId, ca);
}

void IEC104ConnectionManager::onSendCounterRead(const std::string& stationId, int ca) {
    sendCounterRead(stationId, ca);
}

void IEC104ConnectionManager::onSendControl(const std::string& stationId, const ControlCommand& cmd) {
    sendControl(stationId, cmd);
}

// Static callbacks
bool IEC104ConnectionManager::asduReceivedHandler(void* param, int size, CS101_ASDU asdu) {
    auto* info = static_cast<IEC104ConnectionInfo*>(param);
    if (!info) return false;

    // Get station ID from info struct
    std::string stationId = info->stationId;
    auto* self = static_cast<IEC104ConnectionManager*>(info->holder);

    if (self) {
        self->parseASDU(stationId, asdu);
    }

    return true;
}

void IEC104ConnectionManager::connectionHandler(void* param, CS104_Connection connection, CS104_ConnectionEvent event) {
    auto* info = static_cast<IEC104ConnectionInfo*>(param);
    if (!info) return;

    std::string stationId = info->stationId;
    auto* self = static_cast<IEC104ConnectionManager*>(info->holder);
    if (!self) return;

    switch (event) {
        case CS104_CONNECTION_OPENED:
            std::cout << "[IEC104] Connection opened: " << stationId << std::endl;
            if (self->connectionCallback_) {
                self->connectionCallback_(stationId, ConnectionStatus::CONNECTED, "Connected");
            }
            break;

        case CS104_CONNECTION_CLOSED:
            std::cout << "[IEC104] Connection closed: " << stationId << std::endl;
            if (self->connectionCallback_) {
                self->connectionCallback_(stationId, ConnectionStatus::DISCONNECTED, "Disconnected");
            }
            break;

        case CS104_CONNECTION_FAILED:
            std::cout << "[IEC104] Connection failed: " << stationId << std::endl;
            if (self->connectionCallback_) {
                self->connectionCallback_(stationId, ConnectionStatus::CONN_ERROR, "Connection failed");
            }
            break;

        default:
            break;
    }
}

void IEC104ConnectionManager::rawMessageHandler(void* param, uint8_t* msg, int msgSize, bool sent) {
    auto* info = static_cast<IEC104ConnectionInfo*>(param);
    if (!info) return;

    std::string stationId = info->stationId;
    std::string direction = sent ? "TX" : "RX";

    // Print hex dump
    std::cout << "[IEC104] " << direction << " " << stationId << " (" << msgSize << " bytes): ";
    for (int i = 0; i < msgSize; i++) {
        printf("%02X ", msg[i]);
    }
    std::cout << std::endl;

    // Forward to packet callback
    auto* self = static_cast<IEC104ConnectionManager*>(info->holder);
    if (self && self->packetCallback_) {
        self->packetCallback_(stationId, sent, std::vector<uint8_t>(msg, msg + msgSize));
    }
}

void IEC104ConnectionManager::parseASDU(const std::string& stationId, CS101_ASDU asdu) {
    int typeId = CS101_ASDU_getTypeID(asdu);
    int numElements = CS101_ASDU_getNumberOfElements(asdu);
    int cot = CS101_ASDU_getCOT(asdu);

    // Check for control command response (COT = 7 ACTCON or 10 ACTTERM)
    if ((cot == CS101_COT_ACTIVATION_CON || cot == CS101_COT_ACTIVATION_TERMINATION) && numElements > 0) {
        auto io = CS101_ASDU_getElement(asdu, 0);
        uint32_t ioa = InformationObject_getObjectAddress(io);

        bool isPositive = CS101_ASDU_isNegative(asdu) == false;
        std::string message;

        if (cot == CS101_COT_ACTIVATION_CON) {
            message = isPositive ? "ACT_CON (positive)" : "ACT_CON (negative)";
        } else {
            message = "ACT_TERM";
        }

        std::cout << "[IEC104] Control response: IOA=" << ioa
                  << " COT=" << cot << " " << message << std::endl;

        // Determine command type from response (simplified - could be improved)
        std::string cmdType = "unknown";
        if (typeId == M_SP_NA_1 || typeId == M_SP_TA_1 || typeId == M_SP_TB_1) {
            cmdType = "single";
        } else if (typeId == M_DP_NA_1 || typeId == M_DP_TA_1 || typeId == M_DP_TB_1) {
            cmdType = "double";
        }

        // Call callback if registered
        if (controlResultCallback_) {
            controlResultCallback_(stationId, ioa, cmdType, isPositive, message);
        }

        // Don't process as regular data - this is a control response
        InformationObject_destroy(io);
        return;
    }

    std::vector<DigitalPointData> digital;
    std::vector<TelemetryPointData> telemetry;
    std::vector<StepPositionData> step;
    std::vector<BitstringData> bitstring;
    std::vector<CounterPointData> counter;
    std::vector<ProtectionEventData> protection;

    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    switch (typeId) {
        // ==================== Single Point Information ====================
        case M_SP_NA_1: { // Single point information (no time)
            for (int i = 0; i < numElements; i++) {
                auto sp = (SinglePointInformation)CS101_ASDU_getElement(asdu, i);
                DigitalPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)sp);
                p.type = "M_SP_NA_1";
                p.value = SinglePointInformation_getValue(sp);
                p.quality = SinglePointInformation_getQuality(sp);
                p.timestamp = timestamp;
                digital.push_back(p);
                SinglePointInformation_destroy(sp);
            }
            break;
        }

        case M_SP_TA_1: { // Single point information with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto sp = (SinglePointWithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                DigitalPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)sp);
                p.type = "M_SP_TA_1";
                p.value = SinglePointInformation_getValue((SinglePointInformation)sp);
                p.quality = SinglePointInformation_getQuality((SinglePointInformation)sp);
                p.timestamp = extractCP24Timestamp(SinglePointWithCP24Time2a_getTimestamp(sp));
                digital.push_back(p);
                SinglePointWithCP24Time2a_destroy(sp);
            }
            break;
        }

        case M_SP_TB_1: { // Single point information with CP56Time2a (Type 30)
            for (int i = 0; i < numElements; i++) {
                auto sp = (SinglePointWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                DigitalPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)sp);
                p.type = "M_SP_TB_1";
                p.value = SinglePointInformation_getValue((SinglePointInformation)sp);
                p.quality = SinglePointInformation_getQuality((SinglePointInformation)sp);
                p.timestamp = extractCP56Timestamp(SinglePointWithCP56Time2a_getTimestamp(sp));
                digital.push_back(p);
                SinglePointWithCP56Time2a_destroy(sp);
            }
            break;
        }

        // ==================== Double Point Information ====================
        case M_DP_NA_1: { // Double point information (no time)
            for (int i = 0; i < numElements; i++) {
                auto dp = (DoublePointInformation)CS101_ASDU_getElement(asdu, i);
                DigitalPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)dp);
                p.type = "M_DP_NA_1";
                p.value = DoublePointInformation_getValue(dp);
                p.quality = 0;
                p.timestamp = timestamp;
                digital.push_back(p);
                DoublePointInformation_destroy(dp);
            }
            break;
        }

        case M_DP_TA_1: { // Double point information with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto dp = (DoublePointWithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                DigitalPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)dp);
                p.type = "M_DP_TA_1";
                p.value = DoublePointInformation_getValue((DoublePointInformation)dp);
                p.quality = 0;
                p.timestamp = extractCP24Timestamp(DoublePointWithCP24Time2a_getTimestamp(dp));
                digital.push_back(p);
                DoublePointWithCP24Time2a_destroy(dp);
            }
            break;
        }

        case M_DP_TB_1: { // Double point information with CP56Time2a (Type 31)
            for (int i = 0; i < numElements; i++) {
                auto dp = (DoublePointWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                DigitalPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)dp);
                p.type = "M_DP_TB_1";
                p.value = DoublePointInformation_getValue((DoublePointInformation)dp);
                p.quality = 0;
                p.timestamp = extractCP56Timestamp(DoublePointWithCP56Time2a_getTimestamp(dp));
                digital.push_back(p);
                DoublePointWithCP56Time2a_destroy(dp);
            }
            break;
        }

        // ==================== Step Position Information ====================
        case M_ST_NA_1: { // Step position information (no time)
            for (int i = 0; i < numElements; i++) {
                auto sp = (StepPositionInformation)CS101_ASDU_getElement(asdu, i);
                StepPositionData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)sp);
                p.type = "M_ST_NA_1";
                p.value = StepPositionInformation_getValue(sp);
                p.transient = StepPositionInformation_isTransient(sp);
                p.quality = 0;
                p.timestamp = timestamp;
                step.push_back(p);
                StepPositionInformation_destroy(sp);
            }
            break;
        }

        case M_ST_TA_1: { // Step position information with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto sp = (StepPositionWithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                StepPositionData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)sp);
                p.type = "M_ST_TA_1";
                p.value = StepPositionInformation_getValue((StepPositionInformation)sp);
                p.transient = StepPositionInformation_isTransient((StepPositionInformation)sp);
                p.quality = 0;
                p.timestamp = extractCP24Timestamp(StepPositionWithCP24Time2a_getTimestamp(sp));
                step.push_back(p);
                StepPositionWithCP24Time2a_destroy(sp);
            }
            break;
        }

        case M_ST_TB_1: { // Step position information with CP56Time2a (Type 32)
            for (int i = 0; i < numElements; i++) {
                auto sp = (StepPositionWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                StepPositionData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)sp);
                p.type = "M_ST_TB_1";
                p.value = StepPositionInformation_getValue((StepPositionInformation)sp);
                p.transient = StepPositionInformation_isTransient((StepPositionInformation)sp);
                p.quality = 0;
                p.timestamp = extractCP56Timestamp(StepPositionWithCP56Time2a_getTimestamp(sp));
                step.push_back(p);
                StepPositionWithCP56Time2a_destroy(sp);
            }
            break;
        }

        // ==================== Bitstring 32-bit ====================
        case M_BO_NA_1: { // Bitstring 32-bit (no time)
            for (int i = 0; i < numElements; i++) {
                auto bs = (BitString32)CS101_ASDU_getElement(asdu, i);
                BitstringData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)bs);
                p.type = "M_BO_NA_1";
                p.value = BitString32_getValue(bs);
                p.quality = 0;
                p.timestamp = timestamp;
                bitstring.push_back(p);
                BitString32_destroy(bs);
            }
            break;
        }

        case M_BO_TA_1: { // Bitstring 32-bit with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto bs = (Bitstring32WithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                BitstringData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)bs);
                p.type = "M_BO_TA_1";
                p.value = BitString32_getValue((BitString32)bs);
                p.quality = 0;
                p.timestamp = extractCP24Timestamp(Bitstring32WithCP24Time2a_getTimestamp(bs));
                bitstring.push_back(p);
                Bitstring32WithCP24Time2a_destroy(bs);
            }
            break;
        }

        case M_BO_TB_1: { // Bitstring 32-bit with CP56Time2a (Type 33)
            for (int i = 0; i < numElements; i++) {
                auto bs = (Bitstring32WithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                BitstringData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)bs);
                p.type = "M_BO_TB_1";
                p.value = BitString32_getValue((BitString32)bs);
                p.quality = 0;
                p.timestamp = extractCP56Timestamp(Bitstring32WithCP56Time2a_getTimestamp(bs));
                bitstring.push_back(p);
                Bitstring32WithCP56Time2a_destroy(bs);
            }
            break;
        }

        // ==================== Measured Value - Normalized ====================
        case M_ME_NA_1: { // Measured value, normalized (no time)
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueNormalized)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_NA_1";
                p.value = (double)MeasuredValueNormalized_getValue(mv);
                p.quality = MeasuredValueNormalized_getQuality(mv);
                p.timestamp = timestamp;
                telemetry.push_back(p);
                MeasuredValueNormalized_destroy(mv);
            }
            break;
        }

        case M_ME_TA_1: { // Measured value, normalized with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueNormalizedWithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_TA_1";
                p.value = (double)MeasuredValueNormalized_getValue((MeasuredValueNormalized)mv);
                p.quality = MeasuredValueNormalized_getQuality((MeasuredValueNormalized)mv);
                p.timestamp = extractCP24Timestamp(MeasuredValueNormalizedWithCP24Time2a_getTimestamp(mv));
                telemetry.push_back(p);
                MeasuredValueNormalizedWithCP24Time2a_destroy(mv);
            }
            break;
        }

        case M_ME_TD_1: { // Measured value, normalized with CP56Time2a (Type 34)
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueNormalizedWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_TD_1";
                p.value = (double)MeasuredValueNormalized_getValue((MeasuredValueNormalized)mv);
                p.quality = MeasuredValueNormalized_getQuality((MeasuredValueNormalized)mv);
                p.timestamp = extractCP56Timestamp(MeasuredValueNormalizedWithCP56Time2a_getTimestamp(mv));
                telemetry.push_back(p);
                MeasuredValueNormalizedWithCP56Time2a_destroy(mv);
            }
            break;
        }

        case M_ME_ND_1: { // Measured value, normalized without quality (Type 21)
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueNormalizedWithoutQuality)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_ND_1";
                p.value = (double)MeasuredValueNormalizedWithoutQuality_getValue(mv);
                p.quality = 0;
                p.timestamp = timestamp;
                telemetry.push_back(p);
                MeasuredValueNormalizedWithoutQuality_destroy(mv);
            }
            break;
        }

        // ==================== Measured Value - Scaled ====================
        case M_ME_NB_1: { // Measured value, scaled (no time)
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueScaled)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_NB_1";
                p.value = (double)MeasuredValueScaled_getValue(mv);
                p.quality = MeasuredValueScaled_getQuality(mv);
                p.timestamp = timestamp;
                telemetry.push_back(p);
                MeasuredValueScaled_destroy(mv);
            }
            break;
        }

        case M_ME_TB_1: { // Measured value, scaled with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueScaledWithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_TB_1";
                p.value = (double)MeasuredValueScaled_getValue((MeasuredValueScaled)mv);
                p.quality = MeasuredValueScaled_getQuality((MeasuredValueScaled)mv);
                p.timestamp = extractCP24Timestamp(MeasuredValueScaledWithCP24Time2a_getTimestamp(mv));
                telemetry.push_back(p);
                MeasuredValueScaledWithCP24Time2a_destroy(mv);
            }
            break;
        }

        case M_ME_TE_1: { // Measured value, scaled with CP56Time2a (Type 35)
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueScaledWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_TE_1";
                p.value = (double)MeasuredValueScaled_getValue((MeasuredValueScaled)mv);
                p.quality = MeasuredValueScaled_getQuality((MeasuredValueScaled)mv);
                p.timestamp = extractCP56Timestamp(MeasuredValueScaledWithCP56Time2a_getTimestamp(mv));
                telemetry.push_back(p);
                MeasuredValueScaledWithCP56Time2a_destroy(mv);
            }
            break;
        }

        // ==================== Measured Value - Short Float ====================
        case M_ME_NC_1: { // Measured value, short float (no time)
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueShort)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_NC_1";
                p.value = MeasuredValueShort_getValue(mv);
                p.quality = MeasuredValueShort_getQuality(mv);
                p.timestamp = timestamp;
                telemetry.push_back(p);
                MeasuredValueShort_destroy(mv);
            }
            break;
        }

        case M_ME_TC_1: { // Measured value, short float with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueShortWithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_TC_1";
                p.value = MeasuredValueShort_getValue((MeasuredValueShort)mv);
                p.quality = MeasuredValueShort_getQuality((MeasuredValueShort)mv);
                p.timestamp = extractCP24Timestamp(MeasuredValueShortWithCP24Time2a_getTimestamp(mv));
                telemetry.push_back(p);
                MeasuredValueShortWithCP24Time2a_destroy(mv);
            }
            break;
        }

        case M_ME_TF_1: { // Measured value, short float with CP56Time2a (Type 36)
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueShortWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_TF_1";
                p.value = MeasuredValueShort_getValue((MeasuredValueShort)mv);
                p.quality = MeasuredValueShort_getQuality((MeasuredValueShort)mv);
                p.timestamp = extractCP56Timestamp(MeasuredValueShortWithCP56Time2a_getTimestamp(mv));
                telemetry.push_back(p);
                MeasuredValueShortWithCP56Time2a_destroy(mv);
            }
            break;
        }

        // ==================== Integrated Totals (Counter) ====================
        case M_IT_NA_1: { // Integrated totals (no time)
            for (int i = 0; i < numElements; i++) {
                auto it = (IntegratedTotals)CS101_ASDU_getElement(asdu, i);
                CounterPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)it);
                p.type = "M_IT_NA_1";
                auto bcr = IntegratedTotals_getBCR(it);
                p.value = BinaryCounterReading_getValue(bcr);
                p.quality = 0;
                p.timestamp = timestamp;
                p.carry = BinaryCounterReading_hasCarry(bcr);
                p.sequenceNumber = BinaryCounterReading_getSequenceNumber(bcr);
                counter.push_back(p);
                IntegratedTotals_destroy(it);
            }
            break;
        }

        case M_IT_TA_1: { // Integrated totals with CP24Time2a
            for (int i = 0; i < numElements; i++) {
                auto it = (IntegratedTotalsWithCP24Time2a)CS101_ASDU_getElement(asdu, i);
                CounterPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)it);
                p.type = "M_IT_TA_1";
                auto bcr = IntegratedTotals_getBCR((IntegratedTotals)it);
                p.value = BinaryCounterReading_getValue(bcr);
                p.quality = 0;
                p.timestamp = extractCP24Timestamp(IntegratedTotalsWithCP24Time2a_getTimestamp(it));
                p.carry = BinaryCounterReading_hasCarry(bcr);
                p.sequenceNumber = BinaryCounterReading_getSequenceNumber(bcr);
                counter.push_back(p);
                IntegratedTotalsWithCP24Time2a_destroy(it);
            }
            break;
        }

        case M_IT_TB_1: { // Integrated totals with CP56Time2a (Type 37)
            for (int i = 0; i < numElements; i++) {
                auto it = (IntegratedTotalsWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                CounterPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)it);
                p.type = "M_IT_TB_1";
                auto bcr = IntegratedTotals_getBCR((IntegratedTotals)it);
                p.value = BinaryCounterReading_getValue(bcr);
                p.quality = 0;
                p.timestamp = extractCP56Timestamp(IntegratedTotalsWithCP56Time2a_getTimestamp(it));
                p.carry = BinaryCounterReading_hasCarry(bcr);
                p.sequenceNumber = BinaryCounterReading_getSequenceNumber(bcr);
                counter.push_back(p);
                IntegratedTotalsWithCP56Time2a_destroy(it);
            }
            break;
        }

        case S_IT_TC_1: { // Integrated totals for security with CP56Time2a (Type 41)
            for (int i = 0; i < numElements; i++) {
                auto it = (IntegratedTotalsForSecurityStatistics)CS101_ASDU_getElement(asdu, i);
                CounterPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)it);
                p.type = "S_IT_TC_1";
                auto bcr = IntegratedTotalsForSecurityStatistics_getBCR(it);
                p.value = BinaryCounterReading_getValue(bcr);
                p.quality = 0;
                p.timestamp = extractCP56Timestamp(IntegratedTotalsForSecurityStatistics_getTimestamp(it));
                p.carry = BinaryCounterReading_hasCarry(bcr);
                p.sequenceNumber = BinaryCounterReading_getSequenceNumber(bcr);
                counter.push_back(p);
                IntegratedTotalsForSecurityStatistics_destroy(it);
            }
            break;
        }

        // ==================== Protection Equipment Events ====================
        case M_EP_TA_1: { // Event of protection equipment
            for (int i = 0; i < numElements; i++) {
                auto ee = (EventOfProtectionEquipment)CS101_ASDU_getElement(asdu, i);
                ProtectionEventData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)ee);
                p.type = "M_EP_TA_1";
                p.eventType = (int)SingleEvent_getEventState(EventOfProtectionEquipment_getEvent(ee));
                p.timestamp = timestamp;
                protection.push_back(p);
                EventOfProtectionEquipment_destroy(ee);
            }
            break;
        }

        case M_EP_TB_1: { // Packed start events of protection equipment
            for (int i = 0; i < numElements; i++) {
                auto pe = (PackedStartEventsOfProtectionEquipment)CS101_ASDU_getElement(asdu, i);
                ProtectionEventData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)pe);
                p.type = "M_EP_TB_1";
                p.eventType = 0;  // Packed events don't have single event code
                p.timestamp = timestamp;
                protection.push_back(p);
                PackedStartEventsOfProtectionEquipment_destroy(pe);
            }
            break;
        }

        case M_EP_TC_1: { // Packed output circuit info
            for (int i = 0; i < numElements; i++) {
                auto oci = (PackedOutputCircuitInfo)CS101_ASDU_getElement(asdu, i);
                ProtectionEventData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)oci);
                p.type = "M_EP_TC_1";
                p.eventType = 0;
                p.timestamp = timestamp;
                protection.push_back(p);
                PackedOutputCircuitInfo_destroy(oci);
            }
            break;
        }

        case M_EP_TD_1: { // Event of protection equipment with CP56Time2a (Type 38)
            for (int i = 0; i < numElements; i++) {
                auto ee = (EventOfProtectionEquipmentWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                ProtectionEventData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)ee);
                p.type = "M_EP_TD_1";
                p.eventType = (int)SingleEvent_getEventState(EventOfProtectionEquipment_getEvent((EventOfProtectionEquipment)ee));
                p.timestamp = extractCP56Timestamp(EventOfProtectionEquipmentWithCP56Time2a_getTimestamp(ee));
                protection.push_back(p);
                EventOfProtectionEquipmentWithCP56Time2a_destroy(ee);
            }
            break;
        }

        case M_EP_TE_1: { // Packed start events with CP56Time2a (Type 39)
            for (int i = 0; i < numElements; i++) {
                auto pe = (PackedStartEventsOfProtectionEquipmentWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                ProtectionEventData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)pe);
                p.type = "M_EP_TE_1";
                p.eventType = 0;
                p.timestamp = extractCP56Timestamp(PackedStartEventsOfProtectionEquipmentWithCP56Time2a_getTimestamp(pe));
                protection.push_back(p);
                PackedStartEventsOfProtectionEquipmentWithCP56Time2a_destroy(pe);
            }
            break;
        }

        case M_EP_TF_1: { // Packed output circuit info with CP56Time2a (Type 40)
            for (int i = 0; i < numElements; i++) {
                auto oci = (PackedOutputCircuitInfoWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                ProtectionEventData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)oci);
                p.type = "M_EP_TF_1";
                p.eventType = 0;
                p.timestamp = extractCP56Timestamp(PackedOutputCircuitInfoWithCP56Time2a_getTimestamp(oci));
                protection.push_back(p);
                PackedOutputCircuitInfoWithCP56Time2a_destroy(oci);
            }
            break;
        }

        // ==================== Packed Single Point with SCD ====================
        case M_PS_NA_1: { // Packed single point with SCD
            for (int i = 0; i < numElements; i++) {
                auto ps = (PackedSinglePointWithSCD)CS101_ASDU_getElement(asdu, i);
                // Convert SCD to individual digital points
                auto scd = PackedSinglePointWithSCD_getSCD(ps);
                int qds = PackedSinglePointWithSCD_getQuality(ps);
                int ioaBase = InformationObject_getObjectAddress((InformationObject)ps);

                // SCD contains up to 16 single points
                for (int j = 0; j < 16; j++) {
                    DigitalPointData p;
                    p.ioa = ioaBase + j;
                    p.type = "M_PS_NA_1";
                    p.value = StatusAndStatusChangeDetection_getST(scd, j) ? 1 : 0;
                    p.quality = qds;
                    p.timestamp = timestamp;
                    digital.push_back(p);
                }
                PackedSinglePointWithSCD_destroy(ps);
            }
            break;
        }

        // ==================== End of Initialization ====================
        case M_EI_NA_1: { // End of initialization
            std::cout << "[IEC104] End of initialization received from station: " << stationId << std::endl;
            break;
        }

        // ==================== Control Direction Commands ====================
        case C_IC_NA_1: { // Interrogation command (Type 100)
            for (int i = 0; i < numElements; i++) {
                auto ic = (InterrogationCommand)CS101_ASDU_getElement(asdu, i);
                int qoi = InterrogationCommand_getQOI(ic);
                int cot = CS101_ASDU_getCOT(asdu);

                if (cot == CS101_COT_ACTIVATION_CON) {
                    std::cout << "[IEC104] Interrogation ACT_CON received, QOI=" << (int)qoi << std::endl;
                } else if (cot == CS101_COT_ACTIVATION_TERMINATION) {
                    std::cout << "[IEC104] Interrogation ACT_TERM received, QOI=" << (int)qoi << std::endl;
                } else {
                    std::cout << "[IEC104] Interrogation command received, QOI=" << (int)qoi << ", CoT=" << cot << std::endl;
                }

                InterrogationCommand_destroy(ic);
            }
            break;
        }

        default:
            // Handle control command responses (C_ prefix types)
            if (typeId >= 45 && typeId <= 67) {
                std::cout << "[IEC104] Control command response (Type " << typeId << ")" << std::endl;
            } else {
                std::cout << "[IEC104] Unknown ASDU type: " << typeId << std::endl;
            }
            break;
    }

    // Send to legacy callback (backward compatibility)
    if (dataCallback_ && (!digital.empty() || !telemetry.empty())) {
        dataCallback_(stationId, digital, telemetry);
    }

    // Send to extended callback (new)
    if (asduDataCallback_) {
        asduDataCallback_(stationId, digital, telemetry, step, bitstring, counter, protection);
    }
}

} // namespace tls104
