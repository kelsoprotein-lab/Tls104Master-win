/**
 * @file connection.h
 * @brief IEC 60870-5-104 Connection Manager
 */

#ifndef IEC104_CONNECTION_H
#define IEC104_CONNECTION_H

#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

// lib60870 headers
extern "C" {
#include "cs104_connection.h"
#include "cs101_information_objects.h"
#include "tls_config.h"
}

#include "../ipc/bridge.h"

namespace tls104 {

/**
 * @brief Connection status
 */
enum class ConnectionStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
};

/**
 * @brief IEC 104 connection information
 */
struct IEC104ConnectionInfo {
    CS104_Connection connection;
    std::string stationId;
    std::string host;
    int port;
    bool useTLS;
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    int commonAddress;
    ConnectionStatus status;
    std::atomic<bool> shouldReconnect;
    std::thread reconnectThread;
    std::mutex reconnectMutex;
    bool reconnectThreadRunning;

    IEC104ConnectionInfo()
        : connection(nullptr), port(2404), useTLS(false), commonAddress(1),
          status(ConnectionStatus::DISCONNECTED), shouldReconnect(true), reconnectThreadRunning(false) {}
};

/**
 * @brief Data point structures (matching IPC bridge)
 */
struct DigitalPointData {
    int ioa;
    std::string type;
    int value;
    int quality;
    uint64_t timestamp;
};

struct TelemetryPointData {
    int ioa;
    std::string type;
    double value;
    int quality;
    uint64_t timestamp;
};

/**
 * @brief IEC 104 Connection Manager
 * @brief Manages multiple IEC 104 slave connections
 */
class IEC104ConnectionManager : public IPCBridgeCallback {
public:
    using ConnectionCallback = std::function<void(const std::string& stationId, ConnectionStatus status, const std::string& message)>;
    using DataCallback = std::function<void(const std::string& stationId, const std::vector<DigitalPointData>& digital, const std::vector<TelemetryPointData>& telemetry)>;
    using PacketCallback = std::function<void(const std::string& stationId, bool sent, const std::vector<uint8_t>& data)>;

    IEC104ConnectionManager();
    ~IEC104ConnectionManager();

    // Station management
    bool addStation(const StationConfig& config);
    bool removeStation(const std::string& stationId);

    // Commands
    bool sendInterrogation(const std::string& stationId, int ca);
    bool sendClockSync(const std::string& stationId, int ca);
    bool sendCounterRead(const std::string& stationId, int ca);
    bool sendControl(const std::string& stationId, const ControlCommand& cmd);

    // Callbacks
    void setConnectionCallback(ConnectionCallback cb);
    void setDataCallback(DataCallback cb);
    void setPacketCallback(PacketCallback cb);

    // IPCBridgeCallback implementation
    void onAddStation(const StationConfig& config) override;
    void onRemoveStation(const std::string& stationId) override;
    void onSendInterrogation(const std::string& stationId, int ca) override;
    void onSendClockSync(const std::string& stationId, int ca) override;
    void onSendCounterRead(const std::string& stationId, int ca) override;
    void onSendControl(const std::string& stationId, const ControlCommand& cmd) override;

private:
    CS104_Connection createConnection(const IEC104ConnectionInfo& info);
    void connectThreadFunc(const std::string& stationId, IEC104ConnectionInfo* info);
    void reconnectThreadFunc(const std::string& stationId, IEC104ConnectionInfo* info);

    // lib60870 callbacks
    static bool asduReceivedHandler(void* param, int size, CS101_ASDU asdu);
    static void connectionHandler(void* param, CS104_Connection connection, CS104_ConnectionEvent event);

    // ASDU parsing
    void parseASDU(const std::string& stationId, CS101_ASDU asdu);

    std::map<std::string, IEC104ConnectionInfo> connections_;
    std::mutex connectionsMutex_;

    TLSConfiguration g_tlsConfig;

    ConnectionCallback connectionCallback_;
    DataCallback dataCallback_;
    PacketCallback packetCallback_;

    // Temporary storage for parsed data
    std::vector<DigitalPointData> tempDigital_;
    std::vector<TelemetryPointData> tempTelemetry_;
};

} // namespace tls104

#endif // IEC104_CONNECTION_H
