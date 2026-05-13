#include "common/network_utils.h"
#include "common/logger.h"
#include "compat/net_compat.h"
#include <cstring>
#include <sstream>

namespace dinero {

bool isValidIpAddress(const std::string& ip) {
    struct sockaddr_in sa;
    struct sockaddr_in6 sa6;
    
    // Try IPv4
    if (inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) == 1) {
        return true;
    }
    
    // Try IPv6
    if (inet_pton(AF_INET6, ip.c_str(), &(sa6.sin6_addr)) == 1) {
        return true;
    }
    
    return false;
}

bool isValidPort(int port) {
    return port > 0 && port <= 65535;
}

std::string resolveHostname(const std::string& hostname) {
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    
    int status = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
    if (status != 0) {
        g_logger.error("Failed to resolve hostname '" + hostname + "': " + gai_strerror(status));
        return "";
    }
    
    char ip_str[INET6_ADDRSTRLEN];
    void* addr;
    
    if (result->ai_family == AF_INET) {
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)result->ai_addr;
        addr = &(ipv4->sin_addr);
    } else {
        struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)result->ai_addr;
        addr = &(ipv6->sin6_addr);
    }
    
    inet_ntop(result->ai_family, addr, ip_str, sizeof(ip_str));
    freeaddrinfo(result);
    
    return std::string(ip_str);
}

bool isPortOpen(const std::string& host, int port, int timeout_seconds) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        g_logger.error("Failed to create socket");
        return false;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    // Resolve hostname if needed
    std::string ip = host;
    if (!isValidIpAddress(host)) {
        ip = resolveHostname(host);
        if (ip.empty()) {
            COMPAT_CLOSE_SOCKET(sock);
            return false;
        }
    }
    
    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) <= 0) {
        g_logger.error("Invalid address: " + ip);
        COMPAT_CLOSE_SOCKET(sock);
        return false;
    }
    
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = timeout_seconds;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // Try to connect
    int result = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    COMPAT_CLOSE_SOCKET(sock);
    
    return result == 0;
}

std::string getLocalIpAddress() {
    // This is a simplified implementation
    // In a real application, you might want to get the primary interface IP
    return "127.0.0.1";
}

std::string formatAddress(const std::string& host, int port) {
    std::stringstream ss;
    ss << host << ":" << port;
    return ss.str();
}

bool parseAddress(const std::string& address, std::string& host, int& port) {
    size_t colon_pos = address.find(':');
    if (colon_pos == std::string::npos) {
        return false;
    }
    
    host = address.substr(0, colon_pos);
    std::string port_str = address.substr(colon_pos + 1);
    
    try {
        port = std::stoi(port_str);
    } catch (const std::exception& e) {
        return false;
    }
    
    return isValidPort(port);
}

std::string getNetworkInterface() {
    // This is a placeholder implementation
    // In a real application, you would enumerate network interfaces
    return "eth0";
}

bool isLocalAddress(const std::string& address) {
    return address == "127.0.0.1" || 
           address == "localhost" || 
           address == "::1" ||
           address.find("192.168.") == 0 ||
           address.find("10.") == 0 ||
           address.find("172.") == 0;
}

std::string sanitizeHostname(const std::string& hostname) {
    std::string sanitized = hostname;
    
    // Remove any protocol prefixes
    if (sanitized.find("http://") == 0) {
        sanitized = sanitized.substr(7);
    } else if (sanitized.find("https://") == 0) {
        sanitized = sanitized.substr(8);
    }
    
    // Remove trailing slash
    if (!sanitized.empty() && sanitized.back() == '/') {
        sanitized.pop_back();
    }
    
    return sanitized;
}

} // namespace dinero 