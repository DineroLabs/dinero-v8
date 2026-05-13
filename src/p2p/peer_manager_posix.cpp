/**
 * peer_manager_posix.cpp - Non-Qt P2P implementation using POSIX sockets
 *
 * This file provides a cross-platform P2P implementation for builds without Qt.
 * Uses BSD sockets on Unix/macOS and Winsock on Windows.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QT_CORE_LIB  // Only compile for non-Qt builds

#include "peer_manager.h"
#include "common/logger.h"

#include <cstring>
#include <chrono>
#include <algorithm>

// Platform-specific includes
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    #define CLOSE_SOCKET closesocket
    #define SOCKET_ERROR_CODE WSAGetLastError()
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <poll.h>
    #include <errno.h>
    #define CLOSE_SOCKET close
    #define SOCKET_ERROR_CODE errno
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

// P2P Protocol constants
static const uint16_t DEFAULT_P2P_PORT = 20999;
static const int MAX_PENDING_CONNECTIONS = 16;
static const int PING_INTERVAL_MS = 30000;  // 30 seconds
static const double GOOD_PING_THRESHOLD_MS = 300.0;

PeerManager::PeerManager(const Options& opts)
    : options_(opts)
    , server_socket_(-1)
    , boundPort_(0)
    , running_(false)
{
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

bool PeerManager::start() {
    if (running_.load()) {
        return true;  // Already running
    }

    if (!options_.enabled) {
        dinero::g_logger.info("[P2P-POSIX] P2P networking disabled");
        return true;
    }

    // Create listening socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket_ == INVALID_SOCKET) {
        dinero::g_logger.error("[P2P-POSIX] Failed to create socket: " + std::to_string(SOCKET_ERROR_CODE));
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    // Bind to port
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(options_.listenPort);

    if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        // Try auto port if specified port fails
        if (options_.listenPort != 0) {
            dinero::g_logger.warning("[P2P-POSIX] Failed to bind to port " + std::to_string(options_.listenPort) +
                           ", trying auto port");
            addr.sin_port = 0;
            if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
                dinero::g_logger.error("[P2P-POSIX] Failed to bind to any port: " + std::to_string(SOCKET_ERROR_CODE));
                CLOSE_SOCKET(server_socket_);
                server_socket_ = -1;
                return false;
            }
        } else {
            dinero::g_logger.error("[P2P-POSIX] Failed to bind: " + std::to_string(SOCKET_ERROR_CODE));
            CLOSE_SOCKET(server_socket_);
            server_socket_ = -1;
            return false;
        }
    }

    // Get actual bound port
    socklen_t addr_len = sizeof(addr);
    getsockname(server_socket_, (struct sockaddr*)&addr, &addr_len);
    boundPort_ = ntohs(addr.sin_port);

    // Start listening
    if (listen(server_socket_, MAX_PENDING_CONNECTIONS) == SOCKET_ERROR) {
        dinero::g_logger.error("[P2P-POSIX] Failed to listen: " + std::to_string(SOCKET_ERROR_CODE));
        CLOSE_SOCKET(server_socket_);
        server_socket_ = -1;
        return false;
    }

    // Set non-blocking mode
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(server_socket_, FIONBIO, &mode);
#else
    int flags = fcntl(server_socket_, F_GETFL, 0);
    fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK);
#endif

    running_.store(true);
    dinero::g_logger.info("[P2P-POSIX] Listening on port " + std::to_string(boundPort_));

    // Start network thread
    network_thread_ = std::thread(&PeerManager::runNetworkLoop, this);

    // Connect to seed nodes
    for (const auto& node : options_.addNodes) {
        dialPeer(node, DEFAULT_P2P_PORT);
    }

    return true;
}

void PeerManager::stop() {
    if (!running_.exchange(false)) {
        return;  // Already stopped
    }

    dinero::g_logger.info("[P2P-POSIX] Stopping peer manager");

    // Close server socket to wake up accept()
    if (server_socket_ != -1) {
        CLOSE_SOCKET(server_socket_);
        server_socket_ = -1;
    }

    // Wait for network thread
    if (network_thread_.joinable()) {
        network_thread_.join();
    }

    // Close all peer connections
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (int sock : peer_sockets_) {
            if (sock != -1) {
                CLOSE_SOCKET(sock);
            }
        }
        peer_sockets_.clear();
        peer_addresses_.clear();
        peer_latencies_.clear();
    }

    boundPort_ = 0;

#ifdef _WIN32
    WSACleanup();
#endif
}

void PeerManager::startP2P() {
    start();
}

void PeerManager::stopP2P() {
    stop();
}

uint16_t PeerManager::boundPort() const {
    return boundPort_;
}

int PeerManager::peerCount() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return static_cast<int>(peer_sockets_.size());
}

PeerManager::PeerQualityStats PeerManager::GetQualityStats() const {
    PeerQualityStats stats;

    std::lock_guard<std::mutex> lock(peers_mutex_);

    stats.total_peers = static_cast<uint32_t>(peer_sockets_.size());

    if (stats.total_peers == 0) {
        return stats;
    }

    // Calculate average ping and count good/bad peers
    double total_ping = 0.0;
    for (size_t i = 0; i < peer_latencies_.size(); ++i) {
        double latency = peer_latencies_[i];
        total_ping += latency;

        if (latency < GOOD_PING_THRESHOLD_MS) {
            stats.good_peers++;
        } else {
            stats.bad_peers++;
        }
    }

    stats.avg_ping_ms = total_ping / stats.total_peers;

    return stats;
}

void PeerManager::requestHeaders(void* peer) {
    // TODO: Implement header request for non-Qt builds
    // This would send a "getheaders" message to the peer
    (void)peer;
}

void PeerManager::dialPeer(const std::string& host, uint16_t port) {
    if (!running_.load()) {
        return;
    }

    // Parse host:port if port is in host string
    std::string actual_host = host;
    uint16_t actual_port = port;

    size_t colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        actual_host = host.substr(0, colon_pos);
        actual_port = static_cast<uint16_t>(std::stoi(host.substr(colon_pos + 1)));
    }

    dinero::g_logger.info("[P2P-POSIX] Connecting to " + actual_host + ":" + std::to_string(actual_port));

    // Resolve hostname
    struct addrinfo hints, *result;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(actual_port);
    if (getaddrinfo(actual_host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        dinero::g_logger.warning("[P2P-POSIX] Failed to resolve " + actual_host);
        pendingConnections_.push_back(host);
        return;
    }

    // Create socket
    int sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        pendingConnections_.push_back(host);
        return;
    }

    // Connect
    if (connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR) {
        CLOSE_SOCKET(sock);
        freeaddrinfo(result);
        pendingConnections_.push_back(host);
        return;
    }

    freeaddrinfo(result);

    // Add to peer list
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peer_sockets_.push_back(sock);
        peer_addresses_.push_back(actual_host + ":" + std::to_string(actual_port));
        peer_latencies_.push_back(100.0);  // Initial estimate
    }

    dinero::g_logger.info("[P2P-POSIX] Connected to " + actual_host + ":" + std::to_string(actual_port));
}

void PeerManager::scheduleRetries() {
    if (!running_.load() || pendingConnections_.empty()) {
        return;
    }

    // Retry failed connections
    std::vector<std::string> to_retry;
    to_retry.swap(pendingConnections_);

    for (const auto& addr : to_retry) {
        dialPeer(addr, DEFAULT_P2P_PORT);
    }
}

void PeerManager::runNetworkLoop() {
    auto last_retry = std::chrono::steady_clock::now();
    auto last_ping = std::chrono::steady_clock::now();

    while (running_.load()) {
        // Accept new connections
        acceptConnections();

        // Retry failed connections periodically
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_retry).count() >= 30) {
            scheduleRetries();
            last_retry = now;
        }

        // Send pings periodically (simplified - just update latency estimates)
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ping).count() >= PING_INTERVAL_MS) {
            // In a full implementation, we would send ping messages and measure RTT
            last_ping = now;
        }

        // Sleep to avoid busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void PeerManager::acceptConnections() {
    if (server_socket_ == -1) {
        return;
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
    if (client_socket == INVALID_SOCKET) {
        // No pending connections (non-blocking)
        return;
    }

    // Get peer address
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
    std::string peer_addr = std::string(addr_str) + ":" + std::to_string(ntohs(client_addr.sin_port));

    // Check max peers
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (static_cast<int>(peer_sockets_.size()) >= options_.maxPeers) {
            dinero::g_logger.warning("[P2P-POSIX] Max peers reached, rejecting " + peer_addr);
            CLOSE_SOCKET(client_socket);
            return;
        }

        peer_sockets_.push_back(client_socket);
        peer_addresses_.push_back(peer_addr);
        peer_latencies_.push_back(150.0);  // Initial estimate for inbound
    }

    dinero::g_logger.info("[P2P-POSIX] Accepted connection from " + peer_addr);
}

#endif // !QT_CORE_LIB
