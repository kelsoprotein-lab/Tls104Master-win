/**
 * @file server.h
 * @brief Simple HTTP server for serving Web UI with API support
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <queue>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
#endif

namespace tls104 {

/**
 * @brief Station data for API
 */
struct StationData {
    std::string station_id;
    std::string host;
    int port;
    std::string status;
    bool use_tls;
};

/**
 * @brief API message
 */
struct APIMessage {
    std::string type;       // "telemetry", "digital", "connection"
    std::string station_id;
    std::string data;      // JSON string
};

/**
 * @brief HTTP request handler
 */
using HttpHandler = std::function<std::string(const std::string& path, const std::string& method, const std::string& body)>;

/**
 * @brief Message broadcast callback
 */
using BroadcastCallback = std::function<void(const std::string& json)>;

/**
 * @brief Simple HTTP server with API support
 */
class HttpServer {
public:
    /**
     * @brief Constructor
     * @param port Port to listen on
     */
    explicit HttpServer(int port);

    /**
     * @brief Destructor
     */
    ~HttpServer();

    /**
     * @brief Start the server
     * @return true on success
     */
    bool start();

    /**
     * @brief Stop the server
     */
    void stop();

    /**
     * @brief Set document root directory
     * @param root Path to web files
     */
    void setDocumentRoot(const std::string& root);

    /**
     * @brief Set API handler
     * @param handler Callback for API requests
     */
    void setAPIHandler(HttpHandler handler);

    /**
     * @brief Broadcast message to all clients
     * @param json JSON message to broadcast
     */
    void broadcast(const std::string& json);

    /**
     * @brief Check if running
     */
    bool isRunning() const { return running_; }

    /**
     * @brief Get the port
     */
    int getPort() const { return port_; }

private:
    void acceptLoop();
    void handleClient(int clientFd);
    std::string readFile(const std::string& path) const;
    std::string getContentType(const std::string& path) const;
    std::string handleAPI(const std::string& path, const std::string& method, const std::string& body);

    int port_;
    std::string documentRoot_;
    int serverFd_;
    std::atomic<bool> running_;
    std::thread acceptThread_;
    std::mutex mutex_;

    // API handler
    HttpHandler apiHandler_;

    // Broadcast queue for SSE
    std::queue<std::string> messageQueue_;
    std::mutex queueMutex_;
};

} // namespace tls104

#endif // HTTP_SERVER_H
