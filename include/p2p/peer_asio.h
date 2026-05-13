#pragma once

#include "p2p/messages.h"
#include "din_json.h"
#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <queue>
#include <functional>
#include <map>

namespace dinero {
namespace p2p {

enum class PeerState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    VERSION_SENT,
    VERSION_RECEIVED,
    HANDSHAKE_COMPLETE,
    DISCONNECTING
};

enum class DisconnectReason {
    NONE,
    TIMEOUT,
    PROTOCOL_ERROR,
    BAD_MESSAGE,
    PEER_MISBEHAVING,
    MANUAL,
    NETWORK_ERROR
};

class PeerAsio : public std::enable_shared_from_this<PeerAsio> {
public:
    using MessageHandler = std::function<void(std::shared_ptr<PeerAsio>, MessageType, const std::vector<uint8_t>&)>;
    using DisconnectHandler = std::function<void(std::shared_ptr<PeerAsio>, DisconnectReason)>;
    
    // Constructor for outbound connections
    PeerAsio(boost::asio::io_context& io_context, const std::string& address, uint16_t port);
    
    // Constructor for inbound connections
    PeerAsio(boost::asio::tcp::socket socket);
    
    ~PeerAsio();
    
    // Connection management
    void connect();
    void disconnect(DisconnectReason reason = DisconnectReason::MANUAL);
    bool isConnected() const;
    
    // Message sending
    void sendMessage(MessageType type, const std::vector<uint8_t>& payload);
    void sendVersion(int32_t start_height);
    void sendVerack();
    void sendPing();
    void sendPong(uint64_t nonce);
    void sendSendHeaders();
    void sendSendCmpct(bool announce, uint64_t version = 1);
    
    // Event handlers
    void setMessageHandler(MessageHandler handler);
    void setDisconnectHandler(DisconnectHandler handler);
    
    // Peer information
    std::string getId() const;
    std::string getAddress() const;
    uint16_t getPort() const;
    PeerState getState() const;
    NetworkAddress getNetworkAddress() const;
    
    // Protocol information
    uint32_t getVersion() const;
    uint64_t getServices() const;
    std::string getUserAgent() const;
    int32_t getStartHeight() const;
    bool supportsWitness() const;
    bool supportsCompactBlocks() const;
    
    // Statistics
    uint64_t getBytesReceived() const;
    uint64_t getBytesSent() const;
    uint64_t getMessagesReceived() const;
    uint64_t getMessagesSent() const;
    std::chrono::steady_clock::time_point getConnectTime() const;
    std::chrono::steady_clock::time_point getLastActivity() const;
    
    // Ping/latency
    void updatePingTime(std::chrono::milliseconds ping_time);
    std::chrono::milliseconds getAveragePingTime() const;
    
    // Rate limiting and DoS protection
    bool checkRateLimit(MessageType type);
    void incrementBanScore(int score);
    int getBanScore() const;
    bool isBanned() const;
    
    // JSON serialization for RPC/logging
    din::Json toJson() const;
    din::Json getStats() const;

private:
    // Network I/O
    boost::asio::tcp::socket socket_;
    boost::asio::io_context& io_context_;
    
    // Connection info
    std::string address_;
    uint16_t port_;
    std::string peer_id_;
    bool is_inbound_;
    
    // State management
    mutable std::mutex mutex_;
    PeerState state_;
    std::chrono::steady_clock::time_point connect_time_;
    std::chrono::steady_clock::time_point last_activity_;
    
    // Protocol state
    uint32_t protocol_version_;
    uint64_t services_;
    std::string user_agent_;
    int32_t start_height_;
    uint64_t nonce_;
    bool relay_;
    
    // Feature flags
    bool sendheaders_enabled_;
    bool sendcmpct_enabled_;
    bool sendcmpct_announce_;
    uint64_t sendcmpct_version_;
    
    // Statistics
    uint64_t bytes_received_;
    uint64_t bytes_sent_;
    uint64_t messages_received_;
    uint64_t messages_sent_;
    
    // Ping tracking
    std::queue<std::chrono::steady_clock::time_point> ping_times_;
    std::chrono::milliseconds average_ping_;
    uint64_t last_ping_nonce_;
    
    // DoS protection
    int ban_score_;
    std::chrono::steady_clock::time_point last_rate_check_;
    std::map<MessageType, uint32_t> message_counts_;
    
    // Event handlers
    MessageHandler message_handler_;
    DisconnectHandler disconnect_handler_;
    
    // Message processing
    std::vector<uint8_t> read_buffer_;
    std::vector<uint8_t> write_buffer_;
    std::queue<std::vector<uint8_t>> send_queue_;
    bool writing_;
    
    // Timeouts
    boost::asio::steady_timer connect_timer_;
    boost::asio::steady_timer ping_timer_;
    boost::asio::steady_timer timeout_timer_;
    
    // Internal methods
    void startRead();
    void handleRead(const boost::system::error_code& error, size_t bytes_transferred);
    void processMessage(const MessageHeader& header, const std::vector<uint8_t>& payload);
    
    void startWrite();
    void handleWrite(const boost::system::error_code& error, size_t bytes_transferred);
    
    void handleConnect(const boost::system::error_code& error);
    void handleTimeout();
    void handlePingTimer();
    
    void processVersionMessage(const std::vector<uint8_t>& payload);
    void processVerackMessage();
    void processPingMessage(const std::vector<uint8_t>& payload);
    void processPongMessage(const std::vector<uint8_t>& payload);
    void processSendHeadersMessage();
    void processSendCmpctMessage(const std::vector<uint8_t>& payload);
    
    void setState(PeerState new_state);
    void updateActivity();
    void generatePeerId();
    
    // Rate limiting constants
    static constexpr uint32_t MAX_MESSAGES_PER_SECOND = 100;
    static constexpr uint32_t MAX_PING_MESSAGES_PER_MINUTE = 10;
    static constexpr int BAN_THRESHOLD = 100;
    static constexpr std::chrono::seconds CONNECT_TIMEOUT{30};
    static constexpr std::chrono::seconds MESSAGE_TIMEOUT{60};
    static constexpr std::chrono::seconds PING_INTERVAL{120};
};

} // namespace p2p
} // namespace dinero
