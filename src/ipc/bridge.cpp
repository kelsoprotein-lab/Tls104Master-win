/**
 * @file bridge.cpp
 * @brief IPC Bridge implementation
 */

#include "bridge.h"
#include "../platform/socket.h"
#include <iostream>
#include <sstream>
#include <iomanip>

// Simple JSON parsing (without external dependency)
#include <cctype>

namespace tls104 {

namespace {

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

std::string escapeJSON(const std::string& s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b"; break;
            case '\f': o << "\\f"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default: o << c;
        }
    }
    return o.str();
}

} // anonymous namespace

IPCBridge::IPCBridge() : callback_(nullptr), sendToJS_(nullptr) {
    socketInit();
}

IPCBridge::~IPCBridge() {
    socketCleanup();
}

void IPCBridge::setCallback(IPCBridgeCallback* callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = callback;
}

void IPCBridge::setSendCallback(SendToJSCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    sendToJS_ = callback;
}

void IPCBridge::handleMessage(const std::string& json) {
    // Simple JSON parsing
    // Expected format: {"type": "add_station", "data": {...}}

    std::cout << "[IPC Bridge] Received: " << json << std::endl;

    // Find type field
    size_t typePos = json.find("\"type\"");
    if (typePos == std::string::npos) {
        std::cerr << "[IPC Bridge] Missing type field" << std::endl;
        return;
    }

    // Extract type value
    size_t colonPos = json.find(":", typePos);
    if (colonPos == std::string::npos) return;

    size_t quoteStart = json.find("\"", colonPos);
    if (quoteStart == std::string::npos) return;
    quoteStart++;

    size_t quoteEnd = json.find("\"", quoteStart);
    if (quoteEnd == std::string::npos) return;

    std::string type = json.substr(quoteStart, quoteEnd - quoteStart);

    // Find data field
    size_t dataPos = json.find("\"data\"", typePos);
    std::string dataStr = "";
    if (dataPos != std::string::npos) {
        size_t dataColon = json.find(":", dataPos);
        if (dataColon != std::string::npos) {
            size_t dataStart = json.find("{", dataColon);
            size_t dataEnd = json.find_last_of("}");
            if (dataStart != std::string::npos && dataEnd != std::string::npos) {
                dataStr = json.substr(dataStart, dataEnd - dataStart + 1);
            }
        }
    }

    // Parse and dispatch
    if (type == "add_station" && callback_) {
        StationConfig config;
        // Parse host
        size_t hostPos = dataStr.find("\"host\"");
        if (hostPos != std::string::npos) {
            size_t hCol = dataStr.find(":", hostPos);
            size_t hQ1 = dataStr.find("\"", hCol);
            size_t hQ2 = dataStr.find("\"", hQ1 + 1);
            config.host = dataStr.substr(hQ1 + 1, hQ2 - hQ1 - 1);
        }
        // Parse port
        size_t portPos = dataStr.find("\"port\"");
        if (portPos != std::string::npos) {
            size_t pCol = dataStr.find(":", portPos);
            config.port = std::stoi(dataStr.substr(pCol + 1, dataStr.find_first_of(",}", pCol) - pCol - 1));
        }
        // Parse useTLS
        size_t tlsPos = dataStr.find("\"use_tls\"");
        if (tlsPos != std::string::npos) {
            size_t tCol = dataStr.find(":", tlsPos);
            std::string tVal = dataStr.substr(tCol + 1, dataStr.find_first_of(",}", tCol) - tCol - 1);
            config.useTLS = (tVal == "true");
        }
        // Generate ID
        config.id = config.host + ":" + std::to_string(config.port);

        callback_->onAddStation(config);
    }
    else if (type == "remove_station" && callback_) {
        // Extract station_id
        size_t idPos = dataStr.find("\"station_id\"");
        if (idPos != std::string::npos) {
            size_t iCol = dataStr.find(":", idPos);
            size_t iQ1 = dataStr.find("\"", iCol);
            size_t iQ2 = dataStr.find("\"", iQ1 + 1);
            std::string id = dataStr.substr(iQ1 + 1, iQ2 - iQ1 - 1);
            callback_->onRemoveStation(id);
        }
    }
    else if (type == "interrogation" && callback_) {
        // station_id from URL or data
        callback_->onSendInterrogation("", 1); // TODO: parse properly
    }
    else if (type == "clock_sync" && callback_) {
        callback_->onSendClockSync("", 1);
    }
    else if (type == "control" && callback_) {
        ControlCommand cmd;
        // Parse control command from data
        // TODO: implement
        callback_->onSendControl("", cmd);
    }
}

std::string IPCBridge::buildJSON(JSMsgType type, const std::string& stationId, const std::string& data) {
    std::string typeStr;
    switch (type) {
        case JSMsgType::TELEMETRY_DATA: typeStr = "telemetry"; break;
        case JSMsgType::DIGITAL_DATA: typeStr = "digital"; break;
        case JSMsgType::CONNECTION_STATUS: typeStr = "connection"; break;
        case JSMsgType::PACKET_DATA: typeStr = "packet"; break;
    }

    std::ostringstream o;
    o << "{\"type\":\"" << typeStr << "\",\"station_id\":\"" << escapeJSON(stationId) << "\",\"data\":" << data << "}";
    return o.str();
}

void IPCBridge::sendTelemetry(const std::string& stationId, int ioa, const std::string& type, double value, int quality) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sendToJS_) return;

    std::ostringstream o;
    o << "{\"objects\":[";
    o << "{\"ioa\":" << ioa
      << ",\"type\":\"" << escapeJSON(type)
      << "\",\"value\":" << value
      << ",\"quality\":" << quality << "}";
    o << "]}";

    std::string json = buildJSON(JSMsgType::TELEMETRY_DATA, stationId, o.str());
    sendToJS_(json);
}

void IPCBridge::sendDigital(const std::string& stationId, int ioa, const std::string& type, int value, int quality) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sendToJS_) return;

    std::ostringstream o;
    o << "{\"objects\":[";
    o << "{\"ioa\":" << ioa
      << ",\"type\":\"" << escapeJSON(type)
      << "\",\"value\":" << value
      << ",\"quality\":" << quality << "}";
    o << "]}";

    std::string json = buildJSON(JSMsgType::DIGITAL_DATA, stationId, o.str());
    sendToJS_(json);
}

void IPCBridge::sendConnectionStatus(const std::string& stationId, const std::string& status, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sendToJS_) return;

    std::ostringstream o;
    o << "{\"status\":\"" << escapeJSON(status) << "\",\"message\":\"" << escapeJSON(message) << "\"}";
    std::string json = buildJSON(JSMsgType::CONNECTION_STATUS, stationId, o.str());
    sendToJS_(json);
}

void IPCBridge::sendPacket(const std::string& stationId, bool sent, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sendToJS_) return;

    // Convert bytes to hex string
    std::ostringstream hex;
    for (uint8_t b : data) {
        hex << std::hex << std::setfill('0') << std::setw(2) << (int)b << " ";
    }

    std::ostringstream o;
    o << "{\"sent\":" << (sent ? "true" : "false") << ",\"data\":\"" << escapeJSON(hex.str()) << "\"}";
    std::string json = buildJSON(JSMsgType::PACKET_DATA, stationId, o.str());
    sendToJS_(json);
}

} // namespace tls104
