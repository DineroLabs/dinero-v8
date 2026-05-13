#include "daemon/peer_connection.h"
#include "daemon/p2p_message.h"
#include "common/logger.h"
#include "compat/net_compat.h"
#include <cstring>
#include <random>
#include <ctime>

namespace dinero {

PeerConnection::PeerConnection(int socket_fd, const std::string& address, uint16_t port, bool inbound)
    : m_socket_fd(socket_fd)
    , m_address(address)
    , m_port(port)
    , m_inbound(inbound)
    , m_peer_id(address + ":" + std::to_string(port))
    , m_state(ConnectionState::DISCONNECTED)
    , m_protocol_version(0)
    , m_start_height(0)
    , m_bytes_sent(0)
    , m_bytes_received(0)
    , m_ping_time_ms(0)
    , m_ping_nonce(0)
    , m_score(0)
    , m_connection_attempts(0)
    , m_successful_connections(0)
    , m_failed_connections(0)
    , m_messages_sent(0)
    , m_messages_received(0)
    , m_invalid_messages(0)
    , m_bytes_sent_per_minute(0)
    , m_bytes_received_per_minute(0)
    , m_running(false)
    , m_expected_message_size(0) {
    
    auto now = std::chrono::steady_clock::now();
    m_last_activity = now;
    m_first_seen = now;
    m_last_success = now;
    m_last_bandwidth_reset = now;
    
    if (m_socket_fd != -1) {
        m_state = ConnectionState::CONNECTED;
        setupSocket();
    }
}

PeerConnection::~PeerConnection() {
    disconnect();
}

bool PeerConnection::connect() {
    // Record connection attempt
    m_connection_attempts.fetch_add(1);
    
    if (m_inbound) {
        // For inbound connections, socket is already connected
        if (m_socket_fd != -1) {
            m_state = ConnectionState::CONNECTED;
            setupSocket();
            startIOThread();
            recordSuccessfulConnection();
            return true;
        }
        recordFailedConnection();
        return false;
    }
    
    // For outbound connections, create socket and connect
    m_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket_fd == -1) {
        g_logger.error("Failed to create socket for peer " + m_peer_id + ": " + std::string(strerror(errno)));
        return false;
    }
    
    // Set up server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(m_port);
    
    // Try to parse as IP address first, then resolve as hostname
    std::string resolved_ip = m_address;
    if (inet_pton(AF_INET, m_address.c_str(), &server_addr.sin_addr) <= 0) {
        // Not a valid IP, try DNS resolution
        resolved_ip = resolveHostname(m_address);
        if (resolved_ip.empty() || inet_pton(AF_INET, resolved_ip.c_str(), &server_addr.sin_addr) <= 0) {
            g_logger.error("Failed to resolve hostname for peer " + m_peer_id);
            closeSocket();
            return false;
        }
        g_logger.info("Resolved " + m_address + " to " + resolved_ip);
    }
    
    // Set socket to non-blocking for timeout control
#ifdef _WIN32
    u_long nb_mode = 1;
    if (ioctlsocket(m_socket_fd, FIONBIO, &nb_mode) != 0) {
        g_logger.warning("Failed to set socket non-blocking for peer " + m_peer_id);
    }
#else
    int flags = fcntl(m_socket_fd, F_GETFL, 0);
    if (flags == -1 || fcntl(m_socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        g_logger.warning("Failed to set socket non-blocking for peer " + m_peer_id);
    }
#endif
    
    m_state = ConnectionState::CONNECTING;
    
    // Attempt to connect
    int result = ::connect(m_socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result == -1 && errno != EINPROGRESS) {
        g_logger.error("Failed to connect to peer " + m_peer_id + ": " + std::string(strerror(errno)));
        closeSocket();
        return false;
    }

    // Use select() to wait for connection completion with timeout (OPTIMIZED)
    // This replaces the hardcoded 100ms sleep with proper async I/O
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(m_socket_fd, &write_fds);

    struct timeval timeout;
    timeout.tv_sec = 5;   // 5 second timeout (generous for cross-country)
    timeout.tv_usec = 0;

    int select_result = select(m_socket_fd + 1, nullptr, &write_fds, nullptr, &timeout);
    if (select_result <= 0) {
        if (select_result == 0) {
            g_logger.error("Connection timeout to peer " + m_peer_id);
        } else {
            g_logger.error("Select failed for peer " + m_peer_id + ": " + std::string(strerror(errno)));
        }
        closeSocket();
        recordFailedConnection();
        return false;
    }

    // Check if connection succeeded
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(m_socket_fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) == -1 || error != 0) {
        g_logger.error("Connection failed to peer " + m_peer_id + ": " + std::string(strerror(error)));
        closeSocket();
        recordFailedConnection();
        return false;
    }
    
    m_state = ConnectionState::CONNECTED;
    setupSocket();
    startIOThread();
    
    g_logger.info("Connected to peer " + m_peer_id);
    
    // Initiate handshake for outbound connections
    if (!initiateHandshake()) {
        g_logger.error("Failed to initiate handshake with peer " + m_peer_id);
        disconnect();
        recordFailedConnection();
        return false;
    }
    
    recordSuccessfulConnection();
    return true;
}

void PeerConnection::disconnect() {
    if (m_state == ConnectionState::DISCONNECTED) {
        return;
    }
    
    g_logger.info("Disconnecting from peer " + m_peer_id);
    m_state = ConnectionState::DISCONNECTING;
    m_running = false;
    
    // Wait for IO thread to finish
    if (m_io_thread.joinable()) {
        m_io_thread.join();
    }
    
    closeSocket();
    m_state = ConnectionState::DISCONNECTED;
    
    g_logger.info("Disconnected from peer " + m_peer_id);
}

bool PeerConnection::isConnected() const {
    return m_state == ConnectionState::CONNECTED || 
           m_state == ConnectionState::HANDSHAKE_SENT || 
           m_state == ConnectionState::HANDSHAKE_COMPLETE;
}

bool PeerConnection::sendMessage(const P2PMessage& message) {
    if (!isConnected()) {
        g_logger.warning("Cannot send message to disconnected peer " + m_peer_id);
        return false;
    }
    
    try {
        std::vector<uint8_t> payload = message.serialize();
        std::vector<uint8_t> frame = P2PMessage::createMessageFrame(message.getCommand(), payload);
        
        return sendRawData(frame);
    } catch (const std::exception& e) {
        g_logger.error("Failed to serialize message for peer " + m_peer_id + ": " + std::string(e.what()));
        return false;
    }
}

bool PeerConnection::sendRawData(const std::vector<uint8_t>& data) {
    if (!isConnected() || data.empty()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_send_queue_mutex);

    // DoS protection: disconnect slow-draining peers
    if (m_send_queue.size() >= MAX_SEND_QUEUE_MESSAGES ||
        m_send_queue_bytes + data.size() > MAX_SEND_QUEUE_BYTES) {
        g_logger.warning("Send queue overflow for peer " + m_peer_id +
                        " (" + std::to_string(m_send_queue.size()) + " msgs, " +
                        std::to_string(m_send_queue_bytes / 1024) + " KB) — disconnecting");
        m_running = false;
        return false;
    }

    m_send_queue.push(data);
    m_send_queue_bytes += data.size();

    return true;
}

std::shared_ptr<P2PMessage> PeerConnection::receiveMessage() {
    std::lock_guard<std::mutex> lock(m_receive_queue_mutex);
    
    if (m_receive_queue.empty()) {
        return nullptr;
    }
    
    auto message = m_receive_queue.front();
    m_receive_queue.pop();
    return message;
}

bool PeerConnection::hasIncomingMessages() const {
    std::lock_guard<std::mutex> lock(m_receive_queue_mutex);
    return !m_receive_queue.empty();
}

bool PeerConnection::setupSocket() {
    if (m_socket_fd == -1) {
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(m_socket_fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&opt), sizeof(opt)) == -1) {
        g_logger.warning("Failed to set SO_KEEPALIVE for peer " + m_peer_id);
    }

    // Set TCP_NODELAY to disable Nagle's algorithm for low latency
    if (setsockopt(m_socket_fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), sizeof(opt)) == -1) {
        g_logger.warning("Failed to set TCP_NODELAY for peer " + m_peer_id);
    }

    // Set socket to non-blocking mode
#ifdef _WIN32
    u_long nb_mode2 = 1;
    ioctlsocket(m_socket_fd, FIONBIO, &nb_mode2);
#else
    int flags = fcntl(m_socket_fd, F_GETFL, 0);
    if (flags != -1) {
        fcntl(m_socket_fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    
    return true;
}

void PeerConnection::closeSocket() {
    if (m_socket_fd != -1) {
        COMPAT_CLOSE_SOCKET(m_socket_fd);
        m_socket_fd = -1;
    }
}

void PeerConnection::startIOThread() {
    m_running = true;
    m_io_thread = std::thread(&PeerConnection::networkIOThread, this);
}

void PeerConnection::networkIOThread() {
    g_logger.info("Starting network I/O thread for peer " + m_peer_id);
    
    std::vector<uint8_t> buffer(4096);
    
    while (m_running && isConnected()) {
        try {
            auto now = std::chrono::steady_clock::now();

            // Handle outgoing messages
            {
                std::lock_guard<std::mutex> lock(m_send_queue_mutex);
                while (!m_send_queue.empty()) {
                    const auto& data = m_send_queue.front();
                    if (writeData(data)) {
                        m_bytes_sent += data.size();
                        m_send_queue_bytes -= std::min(m_send_queue_bytes, data.size());
                        updateLastActivity();
                    } else {
                        g_logger.error("Failed to send data to peer " + m_peer_id);
                        m_running = false;
                        break;
                    }
                    m_send_queue.pop();
                }
            }
            
            // Handle incoming data
            if (readData(buffer, buffer.size())) {
                // Safety: Prevent unbounded buffer growth (DoS protection)
                const size_t MAX_RECEIVE_BUFFER = 16 * 1024 * 1024; // 16MB max buffer
                if (m_receive_buffer.size() + buffer.size() > MAX_RECEIVE_BUFFER) {
                    g_logger.error("Receive buffer overflow from peer " + m_peer_id +
                                 " (buffer=" + std::to_string(m_receive_buffer.size()) +
                                 " + incoming=" + std::to_string(buffer.size()) + ")");
                    m_running = false;
                    break;
                }

                g_logger.info("Read " + std::to_string(buffer.size()) + " bytes from peer " + m_peer_id);
                // Process received data
                const bool was_empty = m_receive_buffer.empty();
                m_receive_buffer.insert(m_receive_buffer.end(), buffer.begin(), buffer.end());
                if (was_empty) {
                    notePartialReceiveProgress(now);
                } else {
                    m_partial_receive_last_progress = now;
                    m_partial_receive_size_at_last_progress = m_receive_buffer.size();
                }
                processReceivedData();
                updateLastActivity();
            }

            if (checkPartialReceiveTimeout(now)) {
                m_running = false;
                break;
            }
            
            // Small delay to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
        } catch (const std::exception& e) {
            g_logger.error("Exception in network I/O thread for peer " + m_peer_id + ": " + std::string(e.what()));
            m_running = false;
        }
    }
    
    g_logger.info("Network I/O thread finished for peer " + m_peer_id);
}

bool PeerConnection::readData(std::vector<uint8_t>& buffer, size_t max_size) {
    if (m_socket_fd == -1) {
        return false;
    }
    
    ssize_t bytes_read = recv(m_socket_fd, reinterpret_cast<char*>(buffer.data()), static_cast<int>(max_size), 0);
    
    if (bytes_read > 0) {
        buffer.resize(bytes_read);
        m_bytes_received += bytes_read;
        return true;
    } else if (bytes_read == 0) {
        // Connection closed by peer
        g_logger.info("Peer " + m_peer_id + " closed connection (EOF)");
        m_running = false;
        return false;
    } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
        // No data available (non-blocking socket)
        buffer.clear();
        return false;
    } else {
        // Error occurred
        g_logger.error("Error reading from peer " + m_peer_id + ": " + std::string(strerror(errno)));
        m_running = false;
        return false;
    }
}

bool PeerConnection::writeData(const std::vector<uint8_t>& data) {
    if (m_socket_fd == -1 || data.empty()) {
        return false;
    }
    
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t bytes_sent = send(m_socket_fd, reinterpret_cast<const char*>(data.data() + total_sent), static_cast<int>(data.size() - total_sent), MSG_NOSIGNAL);
        
        if (bytes_sent > 0) {
            total_sent += bytes_sent;
        } else if (bytes_sent == 0) {
            g_logger.warning("Send returned 0 for peer " + m_peer_id);
            return false;
        } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Socket buffer full, wait a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            g_logger.error("Error sending to peer " + m_peer_id + ": " + std::string(strerror(errno)));
            return false;
        }
    }
    
    return total_sent == data.size();
}

void PeerConnection::processReceivedData() {
    while (m_receive_buffer.size() >= MESSAGE_HEADER_SIZE) {
        // Check if we have a complete message header
        if (m_expected_message_size == 0) {
            // Parse message header
            if (m_receive_buffer.size() < MESSAGE_HEADER_SIZE) {
                break;
            }
            
            // Check magic bytes
            uint32_t magic = (m_receive_buffer[0]) |
                           (m_receive_buffer[1] << 8) |
                           (m_receive_buffer[2] << 16) |
                           (m_receive_buffer[3] << 24);
            
            if (magic != MAGIC_BYTES) {
                g_logger.error("Invalid magic bytes from peer " + m_peer_id);
                m_running = false;
                return;
            }
            
            // Extract payload length
            m_expected_message_size = (m_receive_buffer[16]) |
                                    (m_receive_buffer[17] << 8) |
                                    (m_receive_buffer[18] << 16) |
                                    (m_receive_buffer[19] << 24);
            
            if (m_expected_message_size > MAX_MESSAGE_SIZE) {
                g_logger.error("Message too large from peer " + m_peer_id + ": " + std::to_string(m_expected_message_size));
                m_running = false;
                return;
            }
        }
        
        // Check if we have the complete message
        size_t total_message_size = MESSAGE_HEADER_SIZE + m_expected_message_size;
        if (m_receive_buffer.size() < total_message_size) {
            break; // Wait for more data
        }
        
        // Extract message
        std::vector<uint8_t> message_data(m_receive_buffer.begin(), m_receive_buffer.begin() + total_message_size);
        m_receive_buffer.erase(m_receive_buffer.begin(), m_receive_buffer.begin() + total_message_size);
        
        // Update bandwidth statistics
        updateBandwidthStats(0, message_data.size());
        
        // Parse and queue the message
        auto message = parseMessage(message_data);
        if (message) {
            // Check rate limits before processing
            if (!checkRateLimit(message->getCommand())) {
                g_logger.warning("Rate limit exceeded for message '" + message->getCommand() + 
                               "' from peer " + m_peer_id + ", dropping message");
                recordInvalidMessage();
                continue; // Skip this message
            }
            
            // Check if peer is rate limited overall
            if (isRateLimited()) {
                g_logger.warning("Peer " + m_peer_id + " is rate limited, dropping message");
                adjustScore(-10); // Heavy penalty for bandwidth abuse
                recordInvalidMessage();
                continue;
            }
            
            g_logger.info("Received message '" + message->getCommand() + "' from peer " + m_peer_id);
            std::lock_guard<std::mutex> lock(m_receive_queue_mutex);

            // DoS protection: drop if receive queue is full
            if (m_receive_queue.size() >= MAX_RECEIVE_QUEUE_MESSAGES) {
                g_logger.warning("Receive queue full for peer " + m_peer_id +
                                " (" + std::to_string(m_receive_queue.size()) + " msgs) — dropping message");
                adjustScore(-5);
                continue;
            }

            m_receive_queue.push(message);
            recordValidMessage();
        } else {
            g_logger.error("Failed to parse message from peer " + m_peer_id);
            recordInvalidMessage();
        }
        
        m_expected_message_size = 0; // Reset for next message
    }

    if (!hasPartialInboundMessage()) {
        resetPartialReceiveTracking();
    }
}

bool PeerConnection::hasPartialInboundMessage() const {
    return !m_receive_buffer.empty();
}

void PeerConnection::notePartialReceiveProgress(std::chrono::time_point<std::chrono::steady_clock> now) {
    if (!hasPartialInboundMessage()) {
        resetPartialReceiveTracking();
        return;
    }

    if (m_partial_receive_started == std::chrono::steady_clock::time_point{}) {
        m_partial_receive_started = now;
    }

    m_partial_receive_last_progress = now;
    m_partial_receive_size_at_last_progress = m_receive_buffer.size();
}

void PeerConnection::resetPartialReceiveTracking() {
    m_partial_receive_started = std::chrono::steady_clock::time_point{};
    m_partial_receive_last_progress = std::chrono::steady_clock::time_point{};
    m_partial_receive_size_at_last_progress = 0;
}

bool PeerConnection::checkPartialReceiveTimeout(std::chrono::time_point<std::chrono::steady_clock> now) {
    if (!hasPartialInboundMessage()) {
        resetPartialReceiveTracking();
        return false;
    }

    if (m_partial_receive_started == std::chrono::steady_clock::time_point{}) {
        notePartialReceiveProgress(now);
        return false;
    }

    if ((now - m_partial_receive_started) > MAX_PARTIAL_RECEIVE_AGE) {
        g_logger.warning("Peer " + m_peer_id +
                         " exceeded partial message age limit (" +
                         std::to_string(m_receive_buffer.size()) + " buffered bytes)");
        return true;
    }

    if ((now - m_partial_receive_last_progress) > MAX_PARTIAL_RECEIVE_STALL &&
        m_receive_buffer.size() == m_partial_receive_size_at_last_progress) {
        g_logger.warning("Peer " + m_peer_id +
                         " stalled during message receive (" +
                         std::to_string(m_receive_buffer.size()) + " buffered bytes)");
        return true;
    }

    return false;
}

std::shared_ptr<P2PMessage> PeerConnection::parseMessage(const std::vector<uint8_t>& data) {
    if (data.size() < MESSAGE_HEADER_SIZE) {
        g_logger.error("Message too short from peer " + m_peer_id);
        return nullptr;
    }
    
    // Extract command
    std::string command(data.begin() + 4, data.begin() + 16);
    // Remove null padding
    size_t null_pos = command.find('\0');
    if (null_pos != std::string::npos) {
        command = command.substr(0, null_pos);
    }
    
    // Extract payload length
    uint32_t payload_length = (data[16]) |
                            (data[17] << 8) |
                            (data[18] << 16) |
                            (data[19] << 24);
    
    // Extract checksum
    uint32_t expected_checksum = (data[20]) |
                               (data[21] << 8) |
                               (data[22] << 16) |
                               (data[23] << 24);
    
    // Extract payload
    std::vector<uint8_t> payload;
    if (payload_length > 0) {
        if (data.size() < MESSAGE_HEADER_SIZE + payload_length) {
            g_logger.error("Incomplete message from peer " + m_peer_id);
            return nullptr;
        }
        payload.assign(data.begin() + MESSAGE_HEADER_SIZE, data.begin() + MESSAGE_HEADER_SIZE + payload_length);
        
        // Verify checksum
        uint32_t actual_checksum = P2PMessage::calculateChecksum(payload);
        if (actual_checksum != expected_checksum) {
            g_logger.error("Checksum mismatch from peer " + m_peer_id);
            return nullptr;
        }
    }
    
    // Create message object
    return P2PMessage::createFromData(command, payload);
}

bool PeerConnection::initiateHandshake() {
    g_logger.info("Initiating handshake with peer " + m_peer_id);

    // Note: Version message is now sent by the active P2P manager with correct blockchain height
    // This prevents duplicate version messages and ensures height is accurate

    g_logger.info("Handshake initiated with peer " + m_peer_id);
    return true;
}

uint64_t PeerConnection::generateNonce() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    return gen();
}

std::string PeerConnection::resolveHostname(const std::string& hostname) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(hostname.c_str(), nullptr, &hints, &result);
    if (rc != 0 || result == nullptr) {
#ifdef _WIN32
        g_logger.error("DNS resolution failed for " + hostname + ": error " + std::to_string(rc));
#else
        g_logger.error("DNS resolution failed for " + hostname + ": " + gai_strerror(rc));
#endif
        return "";
    }

    char ip_str[INET_ADDRSTRLEN];
    auto* sockaddr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
    if (sockaddr == nullptr ||
        inet_ntop(AF_INET, &sockaddr->sin_addr, ip_str, INET_ADDRSTRLEN) == nullptr) {
        freeaddrinfo(result);
        g_logger.error("Failed to convert IP address for " + hostname);
        return "";
    }

    freeaddrinfo(result);
    return std::string(ip_str);
}

void PeerConnection::adjustScore(int32_t delta) {
    m_score.fetch_add(delta);

    // Clamp score to reasonable bounds
    int32_t current_score = m_score.load();
    if (current_score > 1000) {
        m_score.store(1000);
    } else if (current_score < -1000) {
        m_score.store(-1000);
    }

    // Auto-disconnect misbehaving peers
    if (current_score <= -100) {
        g_logger.warning("Peer " + m_peer_id + " score dropped to " +
                        std::to_string(current_score) + " — disconnecting");
        m_running = false;
    }
}

void PeerConnection::recordSuccessfulConnection() {
    m_successful_connections.fetch_add(1);
    m_last_success = std::chrono::steady_clock::now();
    adjustScore(10); // Reward successful connections
}

void PeerConnection::recordFailedConnection() {
    m_failed_connections.fetch_add(1);
    adjustScore(-20); // Penalize failed connections more heavily
}

void PeerConnection::recordValidMessage() {
    m_messages_received.fetch_add(1);
    adjustScore(1); // Small reward for valid messages
}

void PeerConnection::recordInvalidMessage() {
    m_invalid_messages.fetch_add(1);
    adjustScore(-10); // Penalize invalid messages
}

bool PeerConnection::isReliable() const {
    // Consider a peer reliable if:
    // 1. Score is positive
    // 2. Success rate is above 70%
    // 3. Has had at least one successful connection
    
    if (m_score.load() <= 0) {
        return false;
    }
    
    if (m_successful_connections.load() == 0) {
        return false;
    }
    
    return getReliabilityRatio() >= 0.7;
}

double PeerConnection::getReliabilityRatio() const {
    uint32_t total_attempts = m_connection_attempts.load();
    uint32_t successful = m_successful_connections.load();
    
    if (total_attempts == 0) {
        return 0.0;
    }
    
    return static_cast<double>(successful) / static_cast<double>(total_attempts);
}

bool PeerConnection::checkRateLimit(const std::string& message_type) {
    auto now = std::chrono::steady_clock::now();
    
    // Reset counters every minute
    if (std::chrono::duration_cast<std::chrono::minutes>(now - m_last_bandwidth_reset).count() >= 1) {
        m_message_count_per_minute.clear();
        m_bytes_sent_per_minute.store(0);
        m_bytes_received_per_minute.store(0);
        m_last_bandwidth_reset = now;
    }
    
    // Define rate limits per message type (messages per minute)
    std::map<std::string, uint32_t> rate_limits = {
        {"version", 5},      // 5 version messages per minute max
        {"verack", 5},       // 5 verack messages per minute max  
        {"ping", 60},        // 1 ping per second max
        {"pong", 60},        // 1 pong per second max
        {"inv", 100},        // 100 inventory messages per minute
        {"getdata", 100},    // 100 getdata requests per minute
        {"block", 10},       // 10 blocks per minute max
        {"tx", 1000},        // 1000 transactions per minute max
        {"addr", 10},        // 10 addr messages per minute max
        {"getaddr", 5},      // 5 getaddr requests per minute max
        {"getblocks", 10},   // 10 getblocks requests per minute max
        {"getheaders", 10},  // 10 getheaders requests per minute max
        {"headers", 10}      // 10 headers messages per minute max
    };
    
    auto limit_it = rate_limits.find(message_type);
    if (limit_it == rate_limits.end()) {
        // Unknown message type, apply conservative limit
        limit_it = rate_limits.insert({message_type, 10}).first;
    }
    
    uint32_t limit = limit_it->second;
    uint32_t current_count = m_message_count_per_minute[message_type];
    
    if (current_count >= limit) {
        g_logger.warning("Rate limit exceeded for message type '" + message_type + 
                        "' from peer " + m_peer_id + " (" + std::to_string(current_count) + 
                        "/" + std::to_string(limit) + ")");
        adjustScore(-5); // Penalize rate limit violations
        return false;
    }
    
    m_message_count_per_minute[message_type]++;
    return true;
}

bool PeerConnection::isRateLimited() const {
    // Check if peer is sending too much data overall
    uint64_t bytes_per_minute = m_bytes_received_per_minute.load();
    const uint64_t MAX_BYTES_PER_MINUTE = 10 * 1024 * 1024; // 10MB per minute
    
    return bytes_per_minute > MAX_BYTES_PER_MINUTE;
}

void PeerConnection::updateBandwidthStats(size_t bytes_sent, size_t bytes_received) {
    m_bytes_sent_per_minute.fetch_add(bytes_sent);
    m_bytes_received_per_minute.fetch_add(bytes_received);
    
    // Update total counters
    m_bytes_sent.fetch_add(bytes_sent);
    m_bytes_received.fetch_add(bytes_received);
}

} // namespace dinero
