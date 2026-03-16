/**
 * @file connection.cpp
 * @brief IEC 60870-5-104 Connection Manager Implementation
 */

#include "connection.h"
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>

// Thread-safe station ID storage for callbacks
static thread_local std::string g_currentStationId;

namespace tls104 {

IEC104ConnectionManager::IEC104ConnectionManager() {
    // Initialize TLS
#if defined(LIB60870_HAS_TLS_SUPPORT)
    g_tlsConfig = TLSConfiguration_create();
    if (g_tlsConfig) {
        TLSConfiguration_setClientMode(g_tlsConfig);
        TLSConfiguration_setChainValidation(g_tlsConfig, true);
        TLSConfiguration_setAllowOnlyKnownCertificates(g_tlsConfig, false);
        std::cout << "[IEC104] TLS initialized successfully" << std::endl;
    }
#endif
}

IEC104ConnectionManager::~IEC104ConnectionManager() {
    // Disconnect all stations
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (auto& pair : connections_) {
        auto& info = pair.second;
        if (info.connection) {
            CS104_Connection_destroy(info.connection);
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

    IEC104ConnectionInfo info;
    info.stationId = config.id;
    info.host = config.host;
    info.port = config.port;
    info.useTLS = config.useTLS;
    info.caFile = config.caFile;
    info.certFile = config.certFile;
    info.keyFile = config.keyFile;
    info.commonAddress = config.commonAddress > 0 ? config.commonAddress : 1;
    info.status = ConnectionStatus::CONNECTING;
    info.shouldReconnect = true;

    // Connect in a separate thread
    std::thread t([this, &info]() {
        connectThreadFunc(info.stationId, &info);
    });
    t.detach();

    connections_[config.id] = info;

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
    info.shouldReconnect = false;

    if (info.connection) {
        CS104_Connection_destroy(info.connection);
        info.connection = nullptr;
    }

    connections_.erase(it);
    std::cout << "[IEC104] Removed station: " << stationId << std::endl;
    return true;
}

void IEC104ConnectionManager::connectThreadFunc(const std::string& stationId, IEC104ConnectionInfo* info) {
    std::cout << "[IEC104] Connecting to " << stationId << "..." << std::endl;

    info->connection = createConnection(*info);
    if (!info->connection) {
        std::cerr << "[IEC104] Failed to create connection: " << stationId << std::endl;
        info->status = ConnectionStatus::ERROR;
        if (connectionCallback_) {
            connectionCallback_(stationId, ConnectionStatus::ERROR, "Failed to create connection");
        }
        return;
    }

    // Set handlers
    g_currentStationId = stationId;
    CS104_Connection_setASDUReceivedHandler(info->connection, asduReceivedHandler, this);
    CS104_Connection_setConnectionHandler(info->connection, connectionHandler, this);

    // Connect
    if (!CS104_Connection_connect(info->connection)) {
        std::cerr << "[IEC104] Connection failed: " << stationId << std::endl;
        info->status = ConnectionStatus::ERROR;
        CS104_Connection_destroy(info->connection);
        info->connection = nullptr;

        if (connectionCallback_) {
            connectionCallback_(stationId, ConnectionStatus::ERROR, "Connection failed");
        }

        // Start reconnect thread
        if (info->shouldReconnect) {
            std::lock_guard<std::mutex> lock(info->reconnectMutex);
            if (!info->reconnectThreadRunning) {
                info->reconnectThreadRunning = true;
                std::thread t([this, stationId, info]() {
                    reconnectThreadFunc(stationId, info);
                    std::lock_guard<std::mutex> lock(info->reconnectMutex);
                    info->reconnectThreadRunning = false;
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

        // Set handlers
        CS104_Connection_setASDUReceivedHandler(info->connection, asduReceivedHandler, this);
        CS104_Connection_setConnectionHandler(info->connection, connectionHandler, this);

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
    info->status = ConnectionStatus::ERROR;

    if (connectionCallback_) {
        connectionCallback_(stationId, ConnectionStatus::ERROR, "Max retries reached");
    }
}

CS104_Connection IEC104ConnectionManager::createConnection(const IEC104ConnectionInfo& info) {
    CS104_Connection conn = nullptr;

#if defined(LIB60870_HAS_TLS_SUPPORT)
    if (info.useTLS && g_tlsConfig) {
        conn = CS104_Connection_createSecure(
            info.host.c_str(),
            info.port,
            g_tlsConfig,
            info.certFile.c_str(),
            info.keyFile.c_str(),
            info.caFile.c_str()
        );
    } else {
        conn = CS104_Connection_create(info.host.c_str(), info.port);
    }
#else
    conn = CS104_Connection_create(info.host.c_str(), info.port);
#endif

    return conn;
}

bool IEC104ConnectionManager::sendInterrogation(const std::string& stationId, int ca) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end() || !it->second.connection) {
        std::cerr << "[IEC104] Station not connected: " << stationId << std::endl;
        return false;
    }

    bool result = CS104_Connection_sendInterrogationCommand(
        it->second.connection,
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
    if (it == connections_.end() || !it->second.connection) {
        std::cerr << "[IEC104] Station not connected: " << stationId << std::endl;
        return false;
    }

    // Get current time
    CP56Time2a time = CP56Time2a_createFromTimestamp(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())
    );

    bool result = CS104_Connection_sendClockSyncCommand(it->second.connection, ca, time);
    CP56Time2a_destroy(time);

    std::cout << "[IEC104] Send clock sync to " << stationId
              << " (CA=" << ca << "): " << (result ? "OK" : "FAILED") << std::endl;
    return result;
}

bool IEC104ConnectionManager::sendCounterRead(const std::string& stationId, int ca) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);

    auto it = connections_.find(stationId);
    if (it == connections_.end() || !it->second.connection) {
        return false;
    }

    bool result = CS104_Connection_sendCounterInterrogationCommand(
        it->second.connection,
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
    if (it == connections_.end() || !it->second.connection) {
        std::cerr << "[IEC104] Station not connected: " << stationId << std::endl;
        return false;
    }

    bool result = false;

    if (cmd.type == "single") {
        // C_SC_NA_1 - Single command
        auto sc = SingleCommand_create(nullptr, cmd.ioa, cmd.value != 0, cmd.select, false);
        result = CS104_Connection_sendProcessCommand(it->second.connection, cmd.ca, (InformationObject)sc);
        SingleCommand_destroy(sc);
    } else if (cmd.type == "double") {
        // C_DC_NA_1 - Double command
        auto dc = DoubleCommand_create(nullptr, cmd.ioa, cmd.value, cmd.select, false);
        result = CS104_Connection_sendProcessCommand(it->second.connection, cmd.ca, (InformationObject)dc);
        DoubleCommand_destroy(dc);
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

void IEC104ConnectionManager::setPacketCallback(PacketCallback cb) {
    packetCallback_ = cb;
}

// IPCBridgeCallback implementation
void IEC104ConnectionManager::onAddStation(const StationConfig& config) {
    addStation(config);
}

void IEC104ConnectionManager::onRemoveStation(const std::string& stationId) {
    removeStation(stationId);
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
    auto* self = static_cast<IEC104ConnectionManager*>(param);
    // Get station ID from thread local or connection
    std::string stationId = g_currentStationId;

    if (self) {
        self->parseASDU(stationId, asdu);
    }

    return true;
}

void IEC104ConnectionManager::connectionHandler(void* param, CS104_Connection connection, CS104_ConnectionEvent event) {
    auto* self = static_cast<IEC104ConnectionManager*>(param);
    if (!self) return;

    std::string stationId = g_currentStationId;

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
                self->connectionCallback_(stationId, ConnectionStatus::ERROR, "Connection failed");
            }
            break;

        default:
            break;
    }
}

void IEC104ConnectionManager::parseASDU(const std::string& stationId, CS101_ASDU asdu) {
    int typeId = CS101_ASDU_getTypeID(asdu);
    int numElements = CS101_ASDU_getNumberOfElements(asdu);

    std::vector<DigitalPointData> digital;
    std::vector<TelemetryPointData> telemetry;

    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    switch (typeId) {
        case M_SP_NA_1: { // Single point information
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

        case M_DP_NA_1: { // Double point information
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

        case M_ME_NA_1: { // Measured value, normalized
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueNormalized)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_NA_1";
                p.value = (double)MeasuredValueNormalized_getValue(mv);
                p.quality = 0;
                p.timestamp = timestamp;
                telemetry.push_back(p);
                MeasuredValueNormalized_destroy(mv);
            }
            break;
        }

        case M_ME_NB_1: { // Measured value, scaled
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueScaled)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_NB_1";
                p.value = MeasuredValueScaled_getValue(mv);
                p.quality = 0;
                p.timestamp = timestamp;
                telemetry.push_back(p);
                MeasuredValueScaled_destroy(mv);
            }
            break;
        }

        case M_ME_NC_1: { // Measured value, short float
            for (int i = 0; i < numElements; i++) {
                auto mv = (MeasuredValueShort)CS101_ASDU_getElement(asdu, i);
                TelemetryPointData p;
                p.ioa = InformationObject_getObjectAddress((InformationObject)mv);
                p.type = "M_ME_NC_1";
                p.value = MeasuredValueShort_getValue(mv);
                p.quality = 0;
                p.timestamp = timestamp;
                telemetry.push_back(p);
                MeasuredValueShort_destroy(mv);
            }
            break;
        }

        default:
            std::cout << "[IEC104] Unknown ASDU type: " << typeId << std::endl;
            break;
    }

    // Send to callbacks
    if (dataCallback_ && (!digital.empty() || !telemetry.empty())) {
        dataCallback_(stationId, digital, telemetry);
    }
}

} // namespace tls104
