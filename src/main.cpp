/**
 * @file main.cpp
 * @brief TLS104 Master - Windows Desktop Edition
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
            use_tls = (tlsStr == "true");
        }

        // Create station config
        StationConfig config;
        config.id = host + ":" + std::to_string(port);
        config.host = host;
        config.port = port;
        config.useTLS = use_tls;

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
    std::cout << "  TLS104 Master - Windows Desktop" << std::endl;
    std::cout << "  Mode: HTTP Server + IEC104" << std::endl;
    std::cout << "======================================" << std::endl;

    // Signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Parse arguments
    int httpPort = 9999;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            httpPort = std::stoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: tls104_master_win [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -p <port>    HTTP server port (default: 9999)" << std::endl;
            std::cout << "  --help       Show this help" << std::endl;
            return 0;
        }
    }

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

    // Start HTTP server
    g_httpServer = std::make_unique<HttpServer>(httpPort);
    g_httpServer->setDocumentRoot("F:/__goldwind__/__personal__/__code__/Tls104Master-win/web");
    g_httpServer->setAPIHandler(handleAPIRequest);

    if (!g_httpServer->start()) {
        std::cerr << "[Main] Failed to start HTTP server" << std::endl;
        return 1;
    }

    std::string url = "http://localhost:" + std::to_string(httpPort);
    std::cout << "[Main] Open browser at: " << url << std::endl;

#ifdef _WIN32
    // Open browser automatically on Windows - disabled in Git Bash
    // ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif

    std::cout << "[Main] Ready! Press Ctrl+C to exit" << std::endl;

    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Cleanup
    std::cout << "[Main] Cleaning up..." << std::endl;
    g_iec104.reset();
    g_ipcBridge.reset();
    g_httpServer.reset();

    std::cout << "[Main] Goodbye!" << std::endl;
    return 0;
}
