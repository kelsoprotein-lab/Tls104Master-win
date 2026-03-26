/**
 * @file main.cpp
 * @brief TLS104 Master - Cross-Platform Edition
 * @brief Main entry point with IEC104 and HTTP server
 */

#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>
#include <cstring>
#include <map>
#include <sstream>
#include <iomanip>

// Platform-specific headers
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <unistd.h>
#endif

#include "platform/socket.h"
#include "ipc/bridge.h"
#include "http/server.h"
#include "iec104/connection.h"

#ifdef ENABLE_WEBVIEW_GUI
#include "gui/webview_window.h"
#endif

namespace tls104 {

// Global state
static std::atomic<bool> g_running{true};
static std::unique_ptr<HttpServer> g_httpServer;
static std::unique_ptr<IPCBridge> g_ipcBridge;
static std::unique_ptr<IEC104ConnectionManager> g_iec104;

// Station data storage (for API)
static std::map<std::string, StationData> g_stations;

// Signal handler
void signalHandler(int signal) {
    std::cout << "\n[Main] Shutting down..." << std::endl;
    g_running = false;
}

} // namespace tls104

// IPC Bridge callback implementation
class AppIPCBridgeCallback : public tls104::IPCBridgeCallback {
public:
    void onAddStation(const tls104::StationConfig& config) override {
        std::cout << "[Main] Adding station: " << config.id << std::endl;

        // Store station data
        tls104::StationData sd;
        sd.station_id = config.id;
        sd.host = config.host;
        sd.port = config.port;
        sd.status = "connecting";
        sd.use_tls = config.useTLS;
        tls104::g_stations[config.id] = sd;

        if (tls104::g_iec104) {
            tls104::g_iec104->addStation(config);
        }
    }

    void onRemoveStation(const std::string& stationId) override {
        std::cout << "[Main] Removing station: " << stationId << std::endl;
        tls104::g_stations.erase(stationId);

        if (tls104::g_iec104) {
            tls104::g_iec104->removeStation(stationId);
        }
    }

    void onSendInterrogation(const std::string& stationId, int ca) override {
        std::cout << "[Main] Send interrogation to: " << stationId << std::endl;
        if (tls104::g_iec104) {
            tls104::g_iec104->sendInterrogation(stationId, ca);
        }
    }

    void onSendClockSync(const std::string& stationId, int ca) override {
        std::cout << "[Main] Send clock sync to: " << stationId << std::endl;
        if (tls104::g_iec104) {
            tls104::g_iec104->sendClockSync(stationId, ca);
        }
    }

    void onSendCounterRead(const std::string& stationId, int ca) override {
        std::cout << "[Main] Send counter read to: " << stationId << std::endl;
        if (tls104::g_iec104) {
            tls104::g_iec104->sendCounterRead(stationId, ca);
        }
    }

    void onSendControl(const std::string& stationId, const tls104::ControlCommand& cmd) override {
        std::cout << "[Main] Send control to: " << stationId << " IOA=" << cmd.ioa << std::endl;
        if (tls104::g_iec104) {
            tls104::g_iec104->sendControl(stationId, cmd);
        }
    }

    void onDisconnectStation(const std::string& stationId) override {
        std::cout << "[Main] Disconnecting station: " << stationId << std::endl;
        if (tls104::g_iec104) {
            tls104::g_iec104->disconnectStation(stationId);
        }
    }

    void onConnectStation(const std::string& stationId) override {
        std::cout << "[Main] Connecting station: " << stationId << std::endl;
        if (tls104::g_iec104) {
            tls104::g_iec104->connectStation(stationId);
        }
    }
};

static AppIPCBridgeCallback* g_appCallback = nullptr;

// Handle API requests
std::string handleAPIRequest(const std::string& path, const std::string& method, const std::string& body) {
    using namespace tls104;

    std::cout << "[API] " << method << " " << path << std::endl;

    // GET /api/stations - list all stations
    if (method == "GET" && path == "/api/stations") {
        std::ostringstream resp;
        resp << "{\"code\":0,\"data\":[";
        bool first = true;
        for (const auto& pair : g_stations) {
            if (!first) resp << ",";
            first = false;
            const auto& s = pair.second;
            resp << "{\"station_id\":\"" << s.station_id << "\","
                 << "\"host\":\"" << s.host << "\","
                 << "\"port\":" << s.port << ","
                 << "\"status\":\"" << s.status << "\","
                 << "\"use_tls\":" << (s.use_tls ? "true" : "false") << "}";
        }
        resp << "]}";
        std::string json = resp.str();
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // POST /api/stations - add station
    if (method == "POST" && path == "/api/stations") {
        // Parse JSON body (simple parsing)
        std::string host = "127.0.0.1";
        int port = 2404;
        bool use_tls = false;

        // Extract host
        size_t hostPos = body.find("\"host\"");
        if (hostPos != std::string::npos) {
            size_t colon = body.find(":", hostPos);
            size_t quote1 = body.find("\"", colon);
            size_t quote2 = body.find("\"", quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                host = body.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }

        // Extract port
        size_t portPos = body.find("\"port\"");
        if (portPos != std::string::npos) {
            size_t colon = body.find(":", portPos);
            std::string portStr = body.substr(colon + 1, body.find_first_of(",}", colon) - colon - 1);
            port = std::stoi(portStr);
        }

        // Extract use_tls
        size_t tlsPos = body.find("\"use_tls\"");
        if (tlsPos != std::string::npos) {
            size_t colon = body.find(":", tlsPos);
            std::string tlsStr = body.substr(colon + 1, body.find_first_of(",}", colon) - colon - 1);
            tlsStr.erase(0, tlsStr.find_first_not_of(" \t\r\n"));
            tlsStr.erase(tlsStr.find_last_not_of(" \t\r\n") + 1);
            use_tls = (tlsStr == "true");
        }

        // Extract ca_file
        std::string ca_file;
        size_t caFilePos = body.find("\"ca_file\"");
        if (caFilePos != std::string::npos) {
            size_t colon = body.find(":", caFilePos);
            size_t q1 = body.find("\"", colon);
            size_t q2 = body.find("\"", q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                ca_file = body.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // Extract cert_file
        std::string cert_file;
        size_t certFilePos = body.find("\"cert_file\"");
        if (certFilePos != std::string::npos) {
            size_t colon = body.find(":", certFilePos);
            size_t q1 = body.find("\"", colon);
            size_t q2 = body.find("\"", q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                cert_file = body.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // Extract key_file
        std::string key_file;
        size_t keyFilePos = body.find("\"key_file\"");
        if (keyFilePos != std::string::npos) {
            size_t colon = body.find(":", keyFilePos);
            size_t q1 = body.find("\"", colon);
            size_t q2 = body.find("\"", q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                key_file = body.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // Extract tls_version
        std::string tls_version = "1.2";
        size_t tlsVerPos = body.find("\"tls_version\"");
        if (tlsVerPos != std::string::npos) {
            size_t colon = body.find(":", tlsVerPos);
            size_t q1 = body.find("\"", colon);
            size_t q2 = body.find("\"", q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                tls_version = body.substr(q1 + 1, q2 - q1 - 1);
            }
        }

        // Create station config
        StationConfig config;
        config.id = host + ":" + std::to_string(port);
        config.host = host;
        config.port = port;
        config.useTLS = use_tls;
        config.caFile = ca_file;
        config.certFile = cert_file;
        config.keyFile = key_file;
        config.tlsVersion = tls_version;

        // Notify callback
        if (g_appCallback) {
            g_appCallback->onAddStation(config);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // DELETE /api/stations/:id - remove station
    if (method == "DELETE" && path.find("/api/stations/") == 0) {
        std::string stationId = path.substr(14); // Remove "/api/stations/"

        if (g_appCallback) {
            g_appCallback->onRemoveStation(stationId);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // POST /api/stations/:id/disconnect
    if (method == "POST" && path.find("/api/stations/") == 0 && path.find("/disconnect") != std::string::npos) {
        std::string stationId = path.substr(14, path.find("/", 14) - 14);

        if (g_appCallback) {
            g_appCallback->onDisconnectStation(stationId);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // POST /api/stations/:id/connect
    if (method == "POST" && path.find("/api/stations/") == 0 && path.find("/connect") != std::string::npos
        && path.find("/disconnect") == std::string::npos) {
        std::string stationId = path.substr(14, path.find("/", 14) - 14);

        if (g_appCallback) {
            g_appCallback->onConnectStation(stationId);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // POST /api/stations/:id/interrogation
    if (method == "POST" && path.find("/api/stations/") == 0 && path.find("/interrogation") != std::string::npos) {
        std::string stationId = path.substr(14, path.find("/", 14) - 14);

        if (g_appCallback) {
            g_appCallback->onSendInterrogation(stationId, 1);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // POST /api/stations/:id/clock-sync
    if (method == "POST" && path.find("/api/stations/") == 0 && path.find("/clock-sync") != std::string::npos) {
        std::string stationId = path.substr(14, path.find("/", 14) - 14);

        if (g_appCallback) {
            g_appCallback->onSendClockSync(stationId, 1);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // POST /api/stations/:id/counter
    if (method == "POST" && path.find("/api/stations/") == 0 && path.find("/counter") != std::string::npos) {
        std::string stationId = path.substr(14, path.find("/", 14) - 14);

        if (g_appCallback) {
            g_appCallback->onSendCounterRead(stationId, 1);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // POST /api/stations/:id/control
    if (method == "POST" && path.find("/api/stations/") == 0 && path.find("/control") != std::string::npos) {
        std::string stationId = path.substr(14, path.find("/", 14) - 14);

        std::cout << "[API] Control request for station: " << stationId << std::endl;
        std::cout << "[API] Request body: " << body << std::endl;

        // Parse control command from JSON body
        tls104::ControlCommand cmd;
        cmd.ioa = 0;
        cmd.ca = 1;
        cmd.value = 0;
        cmd.type = "single";
        cmd.select = false;

        // Extract ioa
        size_t ioaPos = body.find("\"ioa\"");
        if (ioaPos != std::string::npos) {
            size_t colon = body.find(":", ioaPos);
            std::string ioaStr = body.substr(colon + 1, body.find_first_of(",}", colon) - colon - 1);
            // Trim whitespace
            ioaStr.erase(0, ioaStr.find_first_not_of(" \t"));
            ioaStr.erase(ioaStr.find_last_not_of(" \t") + 1);
            cmd.ioa = std::stoi(ioaStr);
        }

        // Extract ca
        size_t caPos = body.find("\"ca\"");
        if (caPos != std::string::npos) {
            size_t colon = body.find(":", caPos);
            std::string caStr = body.substr(colon + 1, body.find_first_of(",}", colon) - colon - 1);
            caStr.erase(0, caStr.find_first_not_of(" \t"));
            caStr.erase(caStr.find_last_not_of(" \t") + 1);
            cmd.ca = std::stoi(caStr);
        }

        // Extract type
        size_t typePos = body.find("\"type\"");
        if (typePos != std::string::npos) {
            size_t colon = body.find(":", typePos);
            size_t quote1 = body.find("\"", colon);
            size_t quote2 = body.find("\"", quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                cmd.type = body.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }

        // Extract value
        size_t valuePos = body.find("\"value\"");
        if (valuePos != std::string::npos) {
            size_t colon = body.find(":", valuePos);
            std::string valueStr = body.substr(colon + 1, body.find_first_of(",}", colon) - colon - 1);
            valueStr.erase(0, valueStr.find_first_not_of(" \t"));
            valueStr.erase(valueStr.find_last_not_of(" \t") + 1);
            cmd.value = std::stoi(valueStr);
        }

        // Extract select
        size_t selectPos = body.find("\"select\"");
        if (selectPos != std::string::npos) {
            size_t colon = body.find(":", selectPos);
            std::string selectStr = body.substr(colon + 1, body.find_first_of(",}", colon) - colon - 1);
            selectStr.erase(0, selectStr.find_first_not_of(" \t"));
            selectStr.erase(selectStr.find_last_not_of(" \t") + 1);
            cmd.select = (selectStr == "true");
        }

        std::cout << "[API] Parsed control: IOA=" << cmd.ioa << " CA=" << cmd.ca
                  << " type=" << cmd.type << " value=" << cmd.value
                  << " select=" << (cmd.select ? "true" : "false") << std::endl;

        if (g_appCallback) {
            g_appCallback->onSendControl(stationId, cmd);
        }

        std::string json = "{\"code\":0,\"message\":\"ok\"}";
        return "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: " + std::to_string(json.size()) + "\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "\r\n" + json;
    }

    // Default response
    std::string json = "{\"code\":404,\"message\":\"not found\"}";
    return "HTTP/1.1 404 Not Found\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: " + std::to_string(json.size()) + "\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "\r\n" + json;
}

// Main
int main(int argc, char* argv[]) {
    using namespace tls104;

    std::cout << "======================================" << std::endl;
    std::cout << "  TLS104 Master" << std::endl;
    std::cout << "  Mode: HTTP Server + IEC104" << std::endl;
    std::cout << "======================================" << std::endl;

    // Signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Parse arguments
    int httpPort = 19876;
    bool openBrowser = true;
    bool useNativeGui = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            httpPort = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-browser") == 0) {
            openBrowser = false;
        } else if (strcmp(argv[i], "--gui") == 0) {
            useNativeGui = true;
            openBrowser = false;
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: tls104_master_win [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -p <port>        HTTP server port (default: 19876)" << std::endl;
            std::cout << "  --no-browser     Do not open browser on startup" << std::endl;
#ifdef ENABLE_WEBVIEW_GUI
            std::cout << "  --gui            Launch in native window mode" << std::endl;
#endif
            std::cout << "  --help           Show this help" << std::endl;
            return 0;
        }
    }

#ifndef ENABLE_WEBVIEW_GUI
    if (useNativeGui) {
        std::cerr << "[Main] Error: --gui requires building with -DENABLE_WEBVIEW_GUI=ON" << std::endl;
        return 1;
    }
#endif

    // Initialize IPC Bridge (not used in this mode, but kept for compatibility)
    std::cout << "[Main] Initializing IPC Bridge..." << std::endl;
    g_ipcBridge = std::make_unique<IPCBridge>();

    // Initialize IEC104 Connection Manager
    std::cout << "[Main] Initializing IEC104..." << std::endl;
    g_iec104 = std::make_unique<IEC104ConnectionManager>();

    // Initialize IPC callback for API
    g_appCallback = new AppIPCBridgeCallback();

    // Set up connection callback
    g_iec104->setConnectionCallback([](const std::string& stationId, ConnectionStatus status, const std::string& message) {
        std::string statusStr;
        switch (status) {
            case ConnectionStatus::CONNECTED: statusStr = "connected"; break;
            case ConnectionStatus::DISCONNECTED: statusStr = "disconnected"; break;
            case ConnectionStatus::CONNECTING: statusStr = "connecting"; break;
            case ConnectionStatus::CONN_ERROR: statusStr = "error"; break;
        }

        // Update station status
        auto it = g_stations.find(stationId);
        if (it != g_stations.end()) {
            it->second.status = statusStr;
        }

        // Broadcast to all clients
        if (g_httpServer) {
            std::ostringstream json;
            json << "{\"type\":\"connection\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"status\":\"" << statusStr << "\",\"message\":\"" << message << "\"}}";
            g_httpServer->broadcast(json.str());
        }
    });

    // Set up data callback
    g_iec104->setDataCallback([](const std::string& stationId,
                                const std::vector<DigitalPointData>& digital,
                                const std::vector<TelemetryPointData>& telemetry) {
        if (!g_httpServer) return;

        // Send digital data
        for (const auto& p : digital) {
            std::ostringstream json;
            json << "{\"type\":\"digital\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << "\",\"value\":" << p.value
                 << ",\"quality\":" << p.quality << "}]}}";
            g_httpServer->broadcast(json.str());
        }

        // Send telemetry data
        for (const auto& p : telemetry) {
            std::ostringstream json;
            json << "{\"type\":\"telemetry\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << "\",\"value\":" << p.value
                 << ",\"quality\":" << p.quality << "}]}}";
            g_httpServer->broadcast(json.str());
        }
    });

    // Set up ASDU data callback - includes counter (遥脉/累积量), step, bitstring, protection
    g_iec104->setASDUDataCallback([](const std::string& stationId,
                                     const std::vector<DigitalPointData>& digital,
                                     const std::vector<TelemetryPointData>& telemetry,
                                     const std::vector<StepPositionData>& step,
                                     const std::vector<BitstringData>& bitstring,
                                     const std::vector<CounterPointData>& counter,
                                     const std::vector<ProtectionEventData>& protection) {
        if (!g_httpServer) return;

        // Send digital data
        for (const auto& p : digital) {
            std::ostringstream json;
            json << "{\"type\":\"digital\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << "\",\"value\":" << p.value
                 << ",\"quality\":" << p.quality << "}]}}";
            g_httpServer->broadcast(json.str());
        }

        // Send telemetry data
        for (const auto& p : telemetry) {
            std::ostringstream json;
            json << "{\"type\":\"telemetry\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << "\",\"value\":" << p.value
                 << ",\"quality\":" << p.quality << "}]}}";
            g_httpServer->broadcast(json.str());
        }

        // Send counter data (累积量/遥脉)
        for (const auto& p : counter) {
            std::ostringstream json;
            json << "{\"type\":\"counter\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << "\",\"value\":" << p.value
                 << ",\"quality\":" << p.quality
                 << ",\"carry\":" << (p.carry ? "true" : "false")
                 << ",\"sequenceNumber\":" << p.sequenceNumber << "}]}}";
            g_httpServer->broadcast(json.str());
        }

        // Send step position data
        for (const auto& p : step) {
            std::ostringstream json;
            json << "{\"type\":\"telemetry\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << "\",\"value\":" << p.value
                 << ",\"quality\":" << p.quality << "}]}}";
            g_httpServer->broadcast(json.str());
        }

        // Send bitstring data
        for (const auto& p : bitstring) {
            std::ostringstream json;
            json << "{\"type\":\"telemetry\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << "\",\"value\":" << p.value
                 << ",\"quality\":" << p.quality << "}]}}";
            g_httpServer->broadcast(json.str());
        }

        // Send protection event data
        for (const auto& p : protection) {
            std::ostringstream json;
            json << "{\"type\":\"telemetry\",\"station_id\":\"" << stationId << "\","
                 << "\"data\":{\"objects\":[{\"ioa\":" << p.ioa
                 << ",\"type\":\"" << p.type
                 << ",\"value\":" << p.eventType
                 << ",\"quality\":0}]}}";
            g_httpServer->broadcast(json.str());
        }
    });

    // Set up packet callback - forward raw bytes to frontend via SSE
    g_iec104->setPacketCallback([](const std::string& stationId, bool sent, const std::vector<uint8_t>& data) {
        if (!g_httpServer) return;

        // Convert bytes to uppercase hex string
        std::ostringstream hex;
        for (size_t i = 0; i < data.size(); i++) {
            if (i > 0) hex << " ";
            hex << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)data[i];
        }

        std::ostringstream json;
        json << "{\"type\":\"packet\",\"station_id\":\"" << stationId << "\","
             << "\"data\":{\"sent\":" << (sent ? "true" : "false")
             << ",\"data\":\"" << hex.str() << "\"}}";
        g_httpServer->broadcast(json.str());
    });

    // Set up control result callback
    g_iec104->setControlResultCallback([](const std::string& stationId,
                                           uint32_t ioa,
                                           const std::string& type,
                                           bool success,
                                           const std::string& message) {
        std::cout << "[Main] Broadcasting control result: IOA=" << ioa
                  << " success=" << (success ? "true" : "false")
                  << " message=" << message << std::endl;

        if (!g_httpServer) {
            std::cerr << "[Main] g_httpServer is null!" << std::endl;
            return;
        }

        std::ostringstream json;
        json << "{\"type\":\"control_result\",\"station_id\":\"" << stationId << "\","
             << "\"data\":{\"ioa\":" << ioa
             << ",\"type\":\"" << type << "\""
             << ",\"success\":" << (success ? "true" : "false")
             << ",\"message\":\"" << message << "\"}}";
        g_httpServer->broadcast(json.str());
        std::cout << "[Main] Broadcast sent: " << json.str() << std::endl;
    });

    // Start HTTP server
    g_httpServer = std::make_unique<HttpServer>(httpPort);
    g_httpServer->setAPIHandler(handleAPIRequest);

    if (!g_httpServer->start()) {
        std::cerr << "[Main] Failed to start HTTP server" << std::endl;
        return 1;
    }

    std::string url = "http://localhost:" + std::to_string(httpPort);
    std::cout << "[Main] Open browser at: " << url << std::endl;

#ifdef ENABLE_WEBVIEW_GUI
    if (useNativeGui) {
        std::cout << "[Main] Launching native GUI window..." << std::endl;
        tls104::runWebviewWindow("IEC 104 Master", url, 1280, 800);
        g_running = false;
    } else {
#endif
        // Cross-platform browser auto-open
#ifdef _WIN32
        if (openBrowser) {
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#elif defined(__APPLE__)
        if (openBrowser) {
            std::string cmd = "open " + url;
            system(cmd.c_str());
        }
#else
        if (openBrowser) {
            std::string cmd = "xdg-open " + url;
            system(cmd.c_str());
        }
#endif

        std::cout << "[Main] Ready! Press Ctrl+C to exit" << std::endl;

        // Main loop
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
#ifdef ENABLE_WEBVIEW_GUI
    }
#endif

    // Cleanup
    std::cout << "[Main] Cleaning up..." << std::endl;
    g_iec104.reset();
    g_ipcBridge.reset();
    g_httpServer.reset();

    std::cout << "[Main] Goodbye!" << std::endl;
    return 0;
}
