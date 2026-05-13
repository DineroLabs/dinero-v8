#pragma once
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <chrono>
#include <map>

namespace dinero {

// Forward declarations
class P2PMessage;

// Connection state enumeration
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    HANDSHAKE_SENT,
    HANDSHAKE_COMPLETE,
    DISCONNECTING
};

// Peer connection class for individual peer management
class PeerConnection {
public:
    PeerConnection(int socket_fd, const std::string& address, uint16_t port, bool inbound);
    ~PeerConnection();
    
    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const;
    ConnectionState getState() const { return m_state; }
    
    // Message sending
    bool sendMessage(const P2PMessage& message);
    bool sendRawData(const std::vector<uint8_t>& data);
    
    // Message receiving
    std::shared_ptr<P2PMessage> receiveMessage();
    bool hasIncomingMessages() const;
    
    // Peer information
    std::string getPeerId() const { return m_peer_id; }
    std::string getAddress() const { return m_address; }
    uint16_t getPort() const { return m_port; }
    bool isInbound() const { return m_inbound; }
    
    // Protocol information
    uint32_t getProtocolVersion() const { return m_protocol_version; }
    void setProtocolVersion(uint32_t version) { m_protocol_version = version; }
    std::string getUserAgent() const { return m_user_agent; }
    void setUserAgent(const std::string& user_agent) { m_user_agent = user_agent; }
    uint32_t getStartHeight() const { return m_start_height; }
    void setStartHeight(uint32_t height) { m_start_height = height; }
    
    // Connection statistics
    uint64_t getBytesSent() const { return m_bytes_sent; }
    uint64_t getBytesReceived() const { return m_bytes_received; }
    std::chrono::time_point<std::chrono::steady_clock> getLastActivity() const { return m_last_activity; }
    void updateLastActivity() { m_last_activity = std::chrono::steady_clock::now(); }
    
    // Ping/Pong handling
    void setPingTime(uint32_t ping_time_ms) { m_ping_time_ms = ping_time_ms; }
    uint32_t getPingTime() const { return m_ping_time_ms; }
    void setPingNonce(uint64_t nonce) { m_ping_nonce = nonce; }
    uint64_t getPingNonce() const { return m_ping_nonce; }
    
    // Handshake state
    bool isHandshakeComplete() const { return m_state == ConnectionState::HANDSHAKE_COMPLETE; }
    void setHandshakeComplete() { m_state = ConnectionState::HANDSHAKE_COMPLETE; }
    
    // Handshake initiation
    bool initiateHandshake();
    
    // Peer scoring and reputation management
    int32_t getScore() const { return m_score.load(); }
    void adjustScore(int32_t delta);
    void recordSuccessfulConnection();
    void recordFailedConnection();
    void recordValidMessage();
    void recordInvalidMessage();
    bool isReliable() const;
    double getReliabilityRatio() const;
    
    // Rate limiting and DoS protection
    bool checkRateLimit(const std::string& message_type);
    bool isRateLimited() const;
    void updateBandwidthStats(size_t bytes_sent, size_t bytes_received);
    
private:
    // Utility methods
    uint64_t generateNonce();
    std::string resolveHostname(const std::string& hostname);
    // Socket operations
    bool setupSocket();
    void closeSocket();
    bool readData(std::vector<uint8_t>& buffer, size_t size);
    bool writeData(const std::vector<uint8_t>& data);
    
    // Message parsing
    std::shared_ptr<P2PMessage> parseMessage(const std::vector<uint8_t>& data);
    std::vector<uint8_t> serializeMessage(const P2PMessage& message);
    
    // Network I/O thread
    void networkIOThread();
    void startIOThread();
    void processReceivedData();
    bool hasPartialInboundMessage() const;
    void notePartialReceiveProgress(std::chrono::time_point<std::chrono::steady_clock> now);
    void resetPartialReceiveTracking();
    bool checkPartialReceiveTimeout(std::chrono::time_point<std::chrono::steady_clock> now);
    
    // Connection details
    int m_socket_fd;
    std::string m_address;
    uint16_t m_port;
    bool m_inbound;
    std::string m_peer_id;
    std::atomic<ConnectionState> m_state;
    
    // Protocol information
    uint32_t m_protocol_version;
    std::string m_user_agent;
    uint32_t m_start_height;
    
    // Statistics
    std::atomic<uint64_t> m_bytes_sent;
    std::atomic<uint64_t> m_bytes_received;
    std::chrono::time_point<std::chrono::steady_clock> m_last_activity;
    std::atomic<uint32_t> m_ping_time_ms;
    std::atomic<uint64_t> m_ping_nonce;
    
    // Peer scoring and reputation
    std::atomic<int32_t> m_score;
    std::atomic<uint32_t> m_connection_attempts;
    std::atomic<uint32_t> m_successful_connections;
    std::atomic<uint32_t> m_failed_connections;
    std::atomic<uint32_t> m_messages_sent;
    std::atomic<uint32_t> m_messages_received;
    std::atomic<uint32_t> m_invalid_messages;
    std::chrono::time_point<std::chrono::steady_clock> m_first_seen;
    std::chrono::time_point<std::chrono::steady_clock> m_last_success;
    
    // Rate limiting and bandwidth tracking
    std::map<std::string, std::chrono::time_point<std::chrono::steady_clock>> m_last_message_time;
    std::map<std::string, uint32_t> m_message_count_per_minute;
    std::atomic<uint64_t> m_bytes_sent_per_minute;
    std::atomic<uint64_t> m_bytes_received_per_minute;
    std::chrono::time_point<std::chrono::steady_clock> m_last_bandwidth_reset;
    
    // Message queues (bounded to prevent DoS / OOM)
    mutable std::mutex m_send_queue_mutex;
    std::queue<std::vector<uint8_t>> m_send_queue;
    size_t m_send_queue_bytes{0};  // Track total bytes in send queue

    mutable std::mutex m_receive_queue_mutex;
    std::queue<std::shared_ptr<P2PMessage>> m_receive_queue;

    // DoS protection: per-peer queue limits
    static constexpr size_t MAX_SEND_QUEUE_MESSAGES = 200;
    static constexpr size_t MAX_SEND_QUEUE_BYTES = 32 * 1024 * 1024;  // 32 MB
    static constexpr size_t MAX_RECEIVE_QUEUE_MESSAGES = 200;
    
    // Thread management
    std::atomic<bool> m_running;
    std::thread m_io_thread;
    
    // Buffer for partial messages
    std::vector<uint8_t> m_receive_buffer;
    size_t m_expected_message_size;
    std::chrono::time_point<std::chrono::steady_clock> m_partial_receive_started{};
    std::chrono::time_point<std::chrono::steady_clock> m_partial_receive_last_progress{};
    size_t m_partial_receive_size_at_last_progress{0};

    // Slow-client protection: disconnect peers that never finish a message.
    static constexpr std::chrono::seconds MAX_PARTIAL_RECEIVE_AGE{30};
    static constexpr std::chrono::seconds MAX_PARTIAL_RECEIVE_STALL{5};
};

} // namespace dinero
