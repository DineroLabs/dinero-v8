#include "dinero/platform/net.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstring>
#include <stdexcept>

#pragma comment(lib, "ws2_32.lib")

namespace din::net {

static bool g_wsa_initialized = false;

void NetworkInterface::init() {
    if (!g_wsa_initialized) {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
        }
        g_wsa_initialized = true;
    }
}

void NetworkInterface::cleanup() {
    if (g_wsa_initialized) {
        WSACleanup();
        g_wsa_initialized = false;
    }
}

int NetworkInterface::open_tcp_listener(uint16_t port) {
    init();
    
    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == INVALID_SOCKET) {
        return -1;
    }
    
    // Set SO_REUSEADDR to avoid "Address already in use" errors
    char opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == SOCKET_ERROR) {
        closesocket(sockfd);
        return -1;
    }
    
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sockfd);
        return -1;
    }
    
    if (listen(sockfd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(sockfd);
        return -1;
    }
    
    return static_cast<int>(sockfd);
}

int NetworkInterface::connect(const Addr& addr) {
    init();
    
    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == INVALID_SOCKET) {
        return -1;
    }
    
    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(addr.port);
    
    if (inet_pton(AF_INET, addr.host.c_str(), &serv_addr.sin_addr) <= 0) {
        closesocket(sockfd);
        return -1;
    }
    
    if (::connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        closesocket(sockfd);
        return -1;
    }
    
    return static_cast<int>(sockfd);
}

int NetworkInterface::set_nonblocking(int fd, bool on) {
    u_long mode = on ? 1 : 0;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode);
}

int NetworkInterface::close(int fd) {
    return closesocket(static_cast<SOCKET>(fd));
}

int NetworkInterface::get_last_error() {
    return WSAGetLastError();
}

std::string NetworkInterface::error_to_string(int error) {
    char buffer[256];
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        buffer,
        sizeof(buffer),
        NULL
    );
    return std::string(buffer);
}

} // namespace din::net
