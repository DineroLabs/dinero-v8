#include "dinero/platform/net.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <stdexcept>

namespace din::net {

void NetworkInterface::init() {
    // No initialization needed on Apple systems (same as POSIX)
}

void NetworkInterface::cleanup() {
    // No cleanup needed on Apple systems (same as POSIX)
}

int NetworkInterface::open_tcp_listener(uint16_t port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    
    // Set SO_REUSEADDR to avoid "Address already in use" errors
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::close(sockfd);
        return -1;
    }
    
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, SOMAXCONN) < 0) {
        ::close(sockfd);
        return -1;
    }
    
    return sockfd;
}

int NetworkInterface::connect(const Addr& addr) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    
    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(addr.port);
    
    if (inet_pton(AF_INET, addr.host.c_str(), &serv_addr.sin_addr) <= 0) {
        ::close(sockfd);
        return -1;
    }
    
    if (::connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        ::close(sockfd);
        return -1;
    }
    
    return sockfd;
}

int NetworkInterface::set_nonblocking(int fd, bool on) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    
    if (on) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    
    return fcntl(fd, F_SETFL, flags);
}

int NetworkInterface::close(int fd) {
    return ::close(fd);
}

int NetworkInterface::get_last_error() {
    return errno;
}

std::string NetworkInterface::error_to_string(int error) {
    return std::strerror(error);
}

} // namespace din::net
