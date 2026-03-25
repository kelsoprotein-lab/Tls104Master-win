/**
 * @file socket.h
 * @brief Cross-platform socket wrapper
 */

#ifndef SOCKET_H
#define SOCKET_H

#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET SocketType;
    #define SOCKET_INVALID INVALID_SOCKET
    #define SOCKET_CLOSE closesocket
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    typedef int SocketType;
    #define SOCKET_INVALID (-1)
    #define SOCKET_CLOSE close
#endif

namespace tls104 {

/**
 * @brief Initialize socket system (call once at startup)
 */
bool socketInit();

/**
 * @brief Cleanup socket system (call once at shutdown)
 */
void socketCleanup();

/**
 * @brief Create a TCP socket
 * @return Socket descriptor or SOCKET_INVALID on error
 */
SocketType socketCreate();

/**
 * @brief Connect to a server
 * @param sock Socket descriptor
 * @param host Hostname or IP address
 * @param port Port number
 * @return true on success
 */
bool socketConnect(SocketType sock, const std::string& host, int port);

/**
 * @brief Bind and listen
 * @param sock Socket descriptor
 * @param port Port to listen on
 * @return true on success
 */
bool socketListen(SocketType sock, int port);

/**
 * @brief Accept a connection
 * @param sock Listening socket
 * @return Connected socket or SOCKET_INVALID on error
 */
SocketType socketAccept(SocketType sock);

/**
 * @brief Send data
 * @param sock Socket
 * @param data Data buffer
 * @param len Data length
 * @return Bytes sent or -1 on error
 */
int socketSend(SocketType sock, const uint8_t* data, int len);

/**
 * @brief Receive data (non-blocking)
 * @param sock Socket
 * @param buffer Buffer to receive into
 * @param buflen Buffer size
 * @return Bytes received, 0 if no data, -1 on error
 */
int socketRecv(SocketType sock, uint8_t* buffer, int buflen);

/**
 * @brief Close a socket
 * @param sock Socket to close
 */
void socketClose(SocketType sock);

/**
 * @brief Set socket to non-blocking mode
 * @param sock Socket
 * @return true on success
 */
bool socketSetNonBlocking(SocketType sock);

/**
 * @brief Check if socket is valid
 * @param sock Socket
 * @return true if valid
 */
bool socketIsValid(SocketType sock);

} // namespace tls104

#endif // SOCKET_H
