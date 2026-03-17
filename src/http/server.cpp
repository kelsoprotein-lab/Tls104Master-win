/**
 * @file server.cpp
 * @brief Simple HTTP server implementation with API support
 */

#include "server.h"
#include "../platform/socket.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>

#ifdef _WIN32
    #include <filesystem>
#else
    #include <sys/stat.h>
#endif

namespace tls104 {

HttpServer::HttpServer(int port)
    : port_(port), serverFd_(0), running_(false), documentRoot_("./web"), apiHandler_(nullptr) {
}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_) return true;

    if (!socketInit()) {
        std::cerr << "[HTTP] Failed to initialize sockets" << std::endl;
        return false;
    }

    serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (!socketIsValid(serverFd_)) {
        std::cerr << "[HTTP] Failed to create socket" << std::endl;
        return false;
    }

    // Set SO_REUSEADDR
    int opt = 1;
#ifdef _WIN32
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[HTTP] Failed to bind to port " << port_ << std::endl;
        socketClose(serverFd_);
        serverFd_ = 0;
        return false;
    }

    if (listen(serverFd_, 5) < 0) {
        std::cerr << "[HTTP] Failed to listen" << std::endl;
        socketClose(serverFd_);
        serverFd_ = 0;
        return false;
    }

    running_ = true;
    acceptThread_ = std::thread(&HttpServer::acceptLoop, this);

    std::cout << "[HTTP] Server started on port " << port_ << std::endl;
    return true;
}

void HttpServer::stop() {
    if (!running_) return;

    running_ = false;

    // Close server socket to unblock accept
    if (socketIsValid(serverFd_)) {
        socketClose(serverFd_);
        serverFd_ = 0;
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
}

void HttpServer::setDocumentRoot(const std::string& root) {
    documentRoot_ = root;
}

void HttpServer::setAPIHandler(HttpHandler handler) {
    apiHandler_ = handler;
}

void HttpServer::broadcast(const std::string& json) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    messageQueue_.push(json);
}

void HttpServer::acceptLoop() {
    while (running_) {
        SocketType clientFd = socketAccept(serverFd_);
        if (!socketIsValid(clientFd)) {
            if (running_) {
                std::cerr << "[HTTP] Accept failed" << std::endl;
            }
            continue;
        }

        // Handle in a separate thread
        std::thread([this, clientFd]() {
            handleClient(clientFd);
            socketClose(clientFd);
        }).detach();
    }
}

void HttpServer::handleClient(int clientFd) {
    // Read request
    char buffer[4096] = {0};
    int n = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return;

    std::string request(buffer, n);

    // Parse request line
    std::istringstream iss(request);
    std::string method, path, version;
    iss >> method >> path >> version;

    // Get request body if present
    std::string body;
    size_t bodyPos = request.find("\r\n\r\n");
    if (bodyPos != std::string::npos) {
        body = request.substr(bodyPos + 4);
    }

    // Default to index.html
    if (path == "/") {
        path = "/index.html";
    }

    std::string response;

    // Handle API requests
    if (path.find("/api/") == 0 && apiHandler_) {
        response = handleAPI(path, method, body);
    } else if (path == "/events") {
        // Server-Sent Events endpoint
        std::string responseBody = "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";

        send(clientFd, responseBody.c_str(), responseBody.size(), 0);

        // Keep connection open and send events
        time_t lastCheck = time(nullptr);
        while (running_) {
            // Check for new messages every second
            std::this_thread::sleep_for(std::chrono::seconds(1));

            std::lock_guard<std::mutex> lock(queueMutex_);
            while (!messageQueue_.empty()) {
                std::string msg = "data: " + messageQueue_.front() + "\r\n\r\n";
                messageQueue_.pop();
                send(clientFd, msg.c_str(), msg.size(), 0);
            }
        }
        return;
    } else {
        // Static file
        std::string fullPath = documentRoot_ + path;
        std::string content = readFile(fullPath);

        std::string contentType = getContentType(path);

        if (!content.empty()) {
            std::ostringstream resp;
            resp << "HTTP/1.1 200 OK\r\n";
            resp << "Content-Type: " << contentType << "\r\n";
            resp << "Content-Length: " << content.size() << "\r\n";
            resp << "Access-Control-Allow-Origin: *\r\n";
            resp << "Connection: close\r\n";
            resp << "\r\n";
            resp << content;
            response = resp.str();
        } else {
            std::string notFound = "<html><body><h1>404 Not Found</h1></body></html>";
            std::ostringstream resp;
            resp << "HTTP/1.1 404 Not Found\r\n";
            resp << "Content-Type: text/html\r\n";
            resp << "Content-Length: " << notFound.size() << "\r\n";
            resp << "Access-Control-Allow-Origin: *\r\n";
            resp << "Connection: close\r\n";
            resp << "\r\n";
            resp << notFound;
            response = resp.str();
        }
    }

    if (!response.empty()) {
        send(clientFd, response.c_str(), response.size(), 0);
    }
}

std::string HttpServer::handleAPI(const std::string& path, const std::string& method, const std::string& body) {
    if (apiHandler_) {
        return apiHandler_(path, method, body);
    }

    // Default response
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 2\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "\r\n"
           "{}";
}

std::string HttpServer::readFile(const std::string& path) const {
    // Try different path formats
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        // Try with backslash
        std::string altPath = path;
        std::replace(altPath.begin(), altPath.end(), '/', '\\');
        file.open(altPath, std::ios::binary);
        if (!file) {
            std::cerr << "[HTTP] Failed to open: " << path << " or " << altPath << std::endl;
            return "";
        }
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::cerr << "[HTTP] File size: " << size << std::endl;

    std::string content(size, '\0');
    file.read(&content[0], size);

    if (!file) {
        std::cerr << "[HTTP] Failed to read file" << std::endl;
        return "";
    }

    return content;
}

std::string HttpServer::getContentType(const std::string& path) const {
    std::string ext = path.substr(path.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css") return "text/css";
    if (ext == "js") return "application/javascript";
    if (ext == "json") return "application/json";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";

    return "text/plain";
}

} // namespace tls104
