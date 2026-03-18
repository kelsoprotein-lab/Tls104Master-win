/**
 * @file bridge.h
 * @brief IPC Bridge between WebView2 and C++ Core
 */

#ifndef BRIDGE_H
#define BRIDGE_H

#include <string>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace tls104 {

// Message types from JavaScript
enum class JSCmdType {
    ADD_STATION,
    REMOVE_STATION,
    SEND_INTERROGATION,
    SEND_CLOCK_SYNC,
    SEND_COUNTER_READ,
    SEND_CONTROL,
};

// Message types to JavaScript
enum class JSMsgType {
    TELEMETRY_DATA,
    DIGITAL_DATA,
    CONNECTION_STATUS,
    PACKET_DATA,
};

/**
 * @brief Station information from JS
 */
struct StationConfig {
    std::string id;          // station_id (host:port)
    std::string host;
    int port;
    bool useTLS;
    std::string caFile;
    std::string certFile;
    std::string keyFile;
    int commonAddress;        // CA
    std::string tlsVersion;  // "1.2" or "1.3", default "1.2"
};

/**
 * @brief Control command from JS
 */
struct ControlCommand {
    uint32_t ioa;
    std::string type;         // single, double, normalized, scaled, float
    int value;
    int ca;
    bool select;
};

/**
 * @brief IPC Bridge callback interface
 */
class IPCBridgeCallback {
public:
    virtual ~IPCBridgeCallback() = default;

    virtual void onAddStation(const StationConfig& config) = 0;
    virtual void onRemoveStation(const std::string& stationId) = 0;
    virtual void onSendInterrogation(const std::string& stationId, int ca) = 0;
    virtual void onSendClockSync(const std::string& stationId, int ca) = 0;
    virtual void onSendCounterRead(const std::string& stationId, int ca) = 0;
    virtual void onSendControl(const std::string& stationId, const ControlCommand& cmd) = 0;
};

/**
 * @brief IPC Bridge for WebView2 communication
 */
class IPCBridge {
public:
    using SendToJSCallback = std::function<void(const std::string& json)>;

    explicit IPCBridge();
    ~IPCBridge();

    /**
     * @brief Set callback for handling commands from JS
     */
    void setCallback(IPCBridgeCallback* callback);

    /**
     * @brief Set callback to send messages to JS
     */
    void setSendCallback(SendToJSCallback callback);

    /**
     * @brief Parse and handle message from JS
     * @param json JSON string from JavaScript
     */
    void handleMessage(const std::string& json);

    /**
     * @brief Send telemetry data to JS
     */
    void sendTelemetry(const std::string& stationId, int ioa, const std::string& type, double value, int quality);

    /**
     * @brief Send digital data to JS
     */
    void sendDigital(const std::string& stationId, int ioa, const std::string& type, int value, int quality);

    /**
     * @brief Send connection status to JS
     */
    void sendConnectionStatus(const std::string& stationId, const std::string& status, const std::string& message);

    /**
     * @brief Send raw packet to JS
     */
    void sendPacket(const std::string& stationId, bool sent, const std::vector<uint8_t>& data);

private:
    IPCBridgeCallback* callback_;
    SendToJSCallback sendToJS_;
    std::mutex mutex_;

    std::string buildJSON(JSMsgType type, const std::string& stationId, const std::string& data);
};

} // namespace tls104

#endif // BRIDGE_H
