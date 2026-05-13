// Copyright (c) 2024 The Dinero Core developers
// Distributed under the MIT software license

#include "lightning/lightning_transport.h"

#ifndef _WIN32  // Unix domain socket IPC — not available on Windows

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <atomic>

namespace dinero {
namespace lightning {

/**
 * Socket-based Lightning Transport (Bitcoin Core Style)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Wire Protocol (Bitcoin-style framing):
 *   [4 bytes] message_type (uint32_t, network byte order)
 *   [4 bytes] payload_size (uint32_t, network byte order)
 *   [N bytes] payload (binary data)
 *
 * This implementation provides:
 *   - Zero external dependencies (no gRPC/protobuf/abseil)
 *   - Unix sockets for local IPC
 *   - TCP sockets for network communication
 *   - Thread-safe send/recv operations
 *   - Exchange-ready binaries
 *
 * Example endpoints:
 *   Unix socket: "unix:///tmp/dinerod.sock"
 *   TCP socket:  "tcp://127.0.0.1:8332"
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */
class SocketLightningTransport : public LightningTransport {
private:
    int socket_fd_;
    std::atomic<bool> connected_;
    std::mutex send_mutex_;
    std::mutex recv_mutex_;
    std::string endpoint_;

    // Wire protocol constants
    static constexpr size_t HEADER_SIZE = 8;  // 4 bytes type + 4 bytes size
    static constexpr uint32_t MAX_MESSAGE_SIZE = 32 * 1024 * 1024;  // 32MB limit

    /**
     * Read exactly N bytes from socket (blocking)
     *
     * @param buffer Output buffer
     * @param size Number of bytes to read
     * @return true on success, false on error/disconnect
     */
    bool read_exact(uint8_t* buffer, size_t size) {
        size_t total_read = 0;
        while (total_read < size) {
            ssize_t n = ::read(socket_fd_, buffer + total_read, size - total_read);
            if (n <= 0) {
                if (n == 0) {
                    // Connection closed
                    connected_ = false;
                    return false;
                }
                if (errno == EINTR) {
                    // Interrupted by signal, retry
                    continue;
                }
                // Error
                connected_ = false;
                return false;
            }
            total_read += n;
        }
        return true;
    }

    /**
     * Write exactly N bytes to socket (blocking)
     *
     * @param buffer Input buffer
     * @param size Number of bytes to write
     * @return true on success, false on error/disconnect
     */
    bool write_exact(const uint8_t* buffer, size_t size) {
        size_t total_written = 0;
        while (total_written < size) {
            ssize_t n = ::write(socket_fd_, buffer + total_written, size - total_written);
            if (n <= 0) {
                if (n == 0) {
                    // Connection closed
                    connected_ = false;
                    return false;
                }
                if (errno == EINTR) {
                    // Interrupted by signal, retry
                    continue;
                }
                // Error
                connected_ = false;
                return false;
            }
            total_written += n;
        }
        return true;
    }

    /**
     * Parse endpoint string into socket type and address
     *
     * @param endpoint Endpoint string ("unix://path" or "tcp://host:port")
     * @param[out] is_unix true if Unix socket, false if TCP
     * @param[out] address Parsed address
     * @return true on success, false on parse error
     */
    static bool parse_endpoint(const std::string& endpoint, bool& is_unix, std::string& address) {
        if (endpoint.substr(0, 7) == "unix://") {
            is_unix = true;
            address = endpoint.substr(7);
            return true;
        }
        if (endpoint.substr(0, 6) == "tcp://") {
            is_unix = false;
            address = endpoint.substr(6);
            return true;
        }
        return false;
    }

    /**
     * Connect to Unix socket
     *
     * @param path Socket file path
     * @return true on success, false on error
     */
    bool connect_unix(const std::string& path) {
        socket_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            return false;
        }

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
            return false;
        }

        connected_ = true;
        return true;
    }

    /**
     * Connect to TCP socket
     *
     * @param host_port Host and port (e.g., "127.0.0.1:8332")
     * @return true on success, false on error
     */
    bool connect_tcp(const std::string& host_port) {
        // Parse host:port
        size_t colon_pos = host_port.find(':');
        if (colon_pos == std::string::npos) {
            return false;
        }

        std::string host = host_port.substr(0, colon_pos);
        std::string port = host_port.substr(colon_pos + 1);

        // Resolve hostname
        struct addrinfo hints, *result;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;

        if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
            return false;
        }

        // Try each address until we successfully connect
        bool connected = false;
        for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
            socket_fd_ = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (socket_fd_ < 0) {
                continue;
            }

            if (::connect(socket_fd_, rp->ai_addr, rp->ai_addrlen) == 0) {
                connected = true;
                break;
            }

            ::close(socket_fd_);
            socket_fd_ = -1;
        }

        ::freeaddrinfo(result);

        if (connected) {
            connected_ = true;
            return true;
        }

        return false;
    }

public:
    /**
     * Constructor - creates disconnected transport
     */
    SocketLightningTransport(const std::string& endpoint)
        : socket_fd_(-1), connected_(false), endpoint_(endpoint) {
    }

    /**
     * Destructor - ensures socket is closed
     */
    ~SocketLightningTransport() override {
        close();
    }

    /**
     * Connect to the endpoint
     *
     * @return true on success, false on error
     */
    bool connect() {
        bool is_unix;
        std::string address;

        if (!parse_endpoint(endpoint_, is_unix, address)) {
            return false;
        }

        if (is_unix) {
            return connect_unix(address);
        } else {
            return connect_tcp(address);
        }
    }

    /**
     * Send a message to the peer
     *
     * Wire format:
     *   [4 bytes] message_type (network byte order)
     *   [4 bytes] payload_size (network byte order)
     *   [N bytes] payload
     *
     * @param msg Message to send
     * @return true on success, false on error
     */
    bool send(const LightningMessage& msg) override {
        if (!connected_) {
            return false;
        }

        std::lock_guard<std::mutex> lock(send_mutex_);

        // Validate message size
        if (msg.payload.size() > MAX_MESSAGE_SIZE) {
            return false;
        }

        // Build header (8 bytes)
        uint8_t header[HEADER_SIZE];
        uint32_t msg_type_net = htonl(msg.message_type);
        uint32_t payload_size_net = htonl(static_cast<uint32_t>(msg.payload.size()));

        std::memcpy(header, &msg_type_net, 4);
        std::memcpy(header + 4, &payload_size_net, 4);

        // Send header
        if (!write_exact(header, HEADER_SIZE)) {
            return false;
        }

        // Send payload (if non-empty)
        if (!msg.payload.empty()) {
            if (!write_exact(msg.payload.data(), msg.payload.size())) {
                return false;
            }
        }

        return true;
    }

    /**
     * Receive a message from the peer
     *
     * Wire format:
     *   [4 bytes] message_type (network byte order)
     *   [4 bytes] payload_size (network byte order)
     *   [N bytes] payload
     *
     * @param msg [out] Received message
     * @return true on success, false on connection error
     *
     * This call blocks until a message arrives
     */
    bool recv(LightningMessage& msg) override {
        if (!connected_) {
            return false;
        }

        std::lock_guard<std::mutex> lock(recv_mutex_);

        // Read header (8 bytes)
        uint8_t header[HEADER_SIZE];
        if (!read_exact(header, HEADER_SIZE)) {
            return false;
        }

        // Parse header
        uint32_t msg_type_net;
        uint32_t payload_size_net;
        std::memcpy(&msg_type_net, header, 4);
        std::memcpy(&payload_size_net, header + 4, 4);

        msg.message_type = ntohl(msg_type_net);
        uint32_t payload_size = ntohl(payload_size_net);

        // Validate payload size
        if (payload_size > MAX_MESSAGE_SIZE) {
            connected_ = false;
            return false;
        }

        // Read payload
        msg.payload.resize(payload_size);
        if (payload_size > 0) {
            if (!read_exact(msg.payload.data(), payload_size)) {
                return false;
            }
        }

        return true;
    }

    /**
     * Check if connection is alive
     *
     * @return true if connected, false otherwise
     */
    bool is_connected() const override {
        return connected_.load();
    }

    /**
     * Close the connection
     */
    void close() override {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
        connected_ = false;
    }

    /**
     * Accept (no-op for client)
     */
    bool accept() override {
        return true;  // Client doesn't accept connections
    }
};

/**
 * Server-side socket transport listener
 */
class SocketLightningTransportServer : public LightningTransport {
private:
    int listen_fd_;
    int client_fd_;
    std::atomic<bool> connected_;
    std::mutex send_mutex_;
    std::mutex recv_mutex_;
    std::string endpoint_;

    static constexpr size_t HEADER_SIZE = 8;
    static constexpr uint32_t MAX_MESSAGE_SIZE = 32 * 1024 * 1024;

    bool read_exact(uint8_t* buffer, size_t size) {
        size_t total_read = 0;
        while (total_read < size) {
            ssize_t n = ::read(client_fd_, buffer + total_read, size - total_read);
            if (n <= 0) {
                if (n == 0) {
                    connected_ = false;
                    return false;
                }
                if (errno == EINTR) {
                    continue;
                }
                connected_ = false;
                return false;
            }
            total_read += n;
        }
        return true;
    }

    bool write_exact(const uint8_t* buffer, size_t size) {
        size_t total_written = 0;
        while (total_written < size) {
            ssize_t n = ::write(client_fd_, buffer + total_written, size - total_written);
            if (n <= 0) {
                if (n == 0) {
                    connected_ = false;
                    return false;
                }
                if (errno == EINTR) {
                    continue;
                }
                connected_ = false;
                return false;
            }
            total_written += n;
        }
        return true;
    }

    static bool parse_endpoint(const std::string& endpoint, bool& is_unix, std::string& address) {
        if (endpoint.substr(0, 7) == "unix://") {
            is_unix = true;
            address = endpoint.substr(7);
            return true;
        }
        if (endpoint.substr(0, 6) == "tcp://") {
            is_unix = false;
            address = endpoint.substr(6);
            return true;
        }
        return false;
    }

    bool bind_unix(const std::string& path) {
        // Remove existing socket file if it exists
        ::unlink(path.c_str());

        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        if (::listen(listen_fd_, 5) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        return true;
    }

    bool bind_tcp(const std::string& host_port) {
        size_t colon_pos = host_port.find(':');
        if (colon_pos == std::string::npos) {
            return false;
        }

        std::string host = host_port.substr(0, colon_pos);
        std::string port = host_port.substr(colon_pos + 1);

        struct addrinfo hints, *result;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
            return false;
        }

        bool bound = false;
        for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
            listen_fd_ = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (listen_fd_ < 0) {
                continue;
            }

            int opt = 1;
            ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

            if (::bind(listen_fd_, rp->ai_addr, rp->ai_addrlen) == 0) {
                if (::listen(listen_fd_, 5) == 0) {
                    bound = true;
                    break;
                }
            }

            ::close(listen_fd_);
            listen_fd_ = -1;
        }

        ::freeaddrinfo(result);
        return bound;
    }

public:
    SocketLightningTransportServer(const std::string& endpoint)
        : listen_fd_(-1), client_fd_(-1), connected_(false), endpoint_(endpoint) {
    }

    ~SocketLightningTransportServer() override {
        close();
    }

    bool bind_and_listen() {
        bool is_unix;
        std::string address;

        if (!parse_endpoint(endpoint_, is_unix, address)) {
            return false;
        }

        if (is_unix) {
            return bind_unix(address);
        } else {
            return bind_tcp(address);
        }
    }

    bool accept_connection() {
        if (listen_fd_ < 0) {
            return false;
        }

        client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ < 0) {
            return false;
        }

        connected_ = true;
        return true;
    }

    bool send(const LightningMessage& msg) override {
        if (!connected_) {
            return false;
        }

        std::lock_guard<std::mutex> lock(send_mutex_);

        if (msg.payload.size() > MAX_MESSAGE_SIZE) {
            return false;
        }

        uint8_t header[HEADER_SIZE];
        uint32_t msg_type_net = htonl(msg.message_type);
        uint32_t payload_size_net = htonl(static_cast<uint32_t>(msg.payload.size()));

        std::memcpy(header, &msg_type_net, 4);
        std::memcpy(header + 4, &payload_size_net, 4);

        if (!write_exact(header, HEADER_SIZE)) {
            return false;
        }

        if (!msg.payload.empty()) {
            if (!write_exact(msg.payload.data(), msg.payload.size())) {
                return false;
            }
        }

        return true;
    }

    bool recv(LightningMessage& msg) override {
        if (!connected_) {
            return false;
        }

        std::lock_guard<std::mutex> lock(recv_mutex_);

        uint8_t header[HEADER_SIZE];
        if (!read_exact(header, HEADER_SIZE)) {
            return false;
        }

        uint32_t msg_type_net;
        uint32_t payload_size_net;
        std::memcpy(&msg_type_net, header, 4);
        std::memcpy(&payload_size_net, header + 4, 4);

        msg.message_type = ntohl(msg_type_net);
        uint32_t payload_size = ntohl(payload_size_net);

        if (payload_size > MAX_MESSAGE_SIZE) {
            connected_ = false;
            return false;
        }

        msg.payload.resize(payload_size);
        if (payload_size > 0) {
            if (!read_exact(msg.payload.data(), payload_size)) {
                return false;
            }
        }

        return true;
    }

    bool is_connected() const override {
        return connected_.load();
    }

    void close() override {
        if (client_fd_ >= 0) {
            ::close(client_fd_);
            client_fd_ = -1;
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        connected_ = false;
    }

    /**
     * Accept incoming connection
     */
    bool accept() override {
        return accept_connection();
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Factory Implementation
// ═══════════════════════════════════════════════════════════════════════════

std::unique_ptr<LightningTransport> LightningTransportFactory::create(const std::string& endpoint) {
    auto transport = std::make_unique<SocketLightningTransport>(endpoint);
    if (!transport->connect()) {
        return nullptr;
    }
    return transport;
}

std::unique_ptr<LightningTransport> LightningTransportFactory::create_server(const std::string& endpoint) {
    auto server = std::make_unique<SocketLightningTransportServer>(endpoint);
    if (!server->bind_and_listen()) {
        return nullptr;
    }
    return server;
}

} // namespace lightning
} // namespace dinero

#endif // !_WIN32
