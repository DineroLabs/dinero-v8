#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace din::net {

/**
 * @brief Network address structure
 */
struct Addr {
    std::string host;
    uint16_t port;
    
    Addr() = default;
    Addr(const std::string& h, uint16_t p) : host(h), port(p) {}
};

/**
 * @brief Cross-platform network interface
 * 
 * This interface provides platform-agnostic network operations.
 * Implementations are provided in src/platform/{posix,windows,apple}/
 */
class NetworkInterface {
public:
    /**
     * @brief Initialize network subsystem
     * 
     * - No-op on POSIX systems
     * - Calls WSAStartup on Windows
     */
    static void init();
    
    /**
     * @brief Cleanup network subsystem
     * 
     * - No-op on POSIX systems  
     * - Calls WSACleanup on Windows
     */
    static void cleanup();
    
    /**
     * @brief Create a TCP listener socket
     * @param port Port to listen on
     * @return Socket file descriptor, or -1 on error
     */
    static int open_tcp_listener(uint16_t port);
    
    /**
     * @brief Connect to a remote address
     * @param addr Remote address to connect to
     * @return Socket file descriptor, or -1 on error
     */
    static int connect(const Addr& addr);
    
    /**
     * @brief Set socket to non-blocking mode
     * @param fd Socket file descriptor
     * @param on True for non-blocking, false for blocking
     * @return 0 on success, -1 on error
     */
    static int set_nonblocking(int fd, bool on);
    
    /**
     * @brief Close a socket
     * @param fd Socket file descriptor
     * @return 0 on success, -1 on error
     */
    static int close(int fd);
    
    /**
     * @brief Get last network error
     * @return Error code
     */
    static int get_last_error();
    
    /**
     * @brief Convert error code to string
     * @param error Error code
     * @return Error description
     */
    static std::string error_to_string(int error);
};

// Convenience functions that delegate to NetworkInterface
inline void init() { NetworkInterface::init(); }
inline void cleanup() { NetworkInterface::cleanup(); }
inline int open_tcp_listener(uint16_t port) { return NetworkInterface::open_tcp_listener(port); }
inline int connect(const Addr& addr) { return NetworkInterface::connect(addr); }
inline int set_nonblocking(int fd, bool on) { return NetworkInterface::set_nonblocking(fd, on); }
inline int close(int fd) { return NetworkInterface::close(fd); }
inline int get_last_error() { return NetworkInterface::get_last_error(); }
inline std::string error_to_string(int error) { return NetworkInterface::error_to_string(error); }

} // namespace din::net
