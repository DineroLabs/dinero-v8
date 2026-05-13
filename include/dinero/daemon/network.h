#pragma once
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <functional>
#include "compat/net_compat.h"
#include "compat/jsoncpp_compat.h"

namespace dinero {

// Forward declarations
class Blockchain;
struct Transaction;
struct Block;

// Network message types (similar to Bitcoin)
enum class MessageType {
    VERSION = 0,
    VERACK = 1,
    PING = 2,
    PONG = 3,
    GETADDR = 4,
    ADDR = 5,
    INV = 6,
    GETDATA = 7,
    BLOCK = 8,
    TX = 9,
    GETBLOCKS = 10,
    HEADERS = 11,
    GETHEADERS = 12,
    MEMPOOL = 13,
    REJECT = 14,
    ALERT = 15
};

// Network message structure
struct NetworkMessage {
    MessageType type;
    std::string payload;
    uint32_t checksum;
    uint64_t timestamp;
    
    NetworkMessage() : type(MessageType::VERSION), checksum(0), timestamp(0) {}
    NetworkMessage(MessageType t, const std::string& p) : type(t), payload(p), checksum(0), timestamp(0) {}
};

// Peer information
struct PeerInfo {
    std::string address;
    int port;
    std::string user_agent;
    uint64_t last_seen;
    uint64_t last_ping;
    bool connected;
    int socket_fd;
    std::string version;
    uint32_t services;
    uint64_t starting_height;
    
    PeerInfo() : port(8333), last_seen(0), last_ping(0), connected(false), socket_fd(-1), services(0), starting_height(0) {}
    PeerInfo(const std::string& addr, int p) : address(addr), port(p), last_seen(0), last_ping(0), connected(false), socket_fd(-1), services(0), starting_height(0) {}
};

// Network statistics
struct NetworkStats {
    uint32_t total_peers;
    uint32_t connected_peers;
    uint32_t total_transactions_sent;
    uint32_t total_blocks_sent;
    uint64_t total_bytes_sent;
    uint64_t total_bytes_received;
    uint64_t uptime_seconds;
    
    NetworkStats() : total_peers(0), connected_peers(0), total_transactions_sent(0), total_blocks_sent(0), total_bytes_sent(0), total_bytes_received(0), uptime_seconds(0) {}
};

class Network {
public:
    Network();
    ~Network();
    
    // Core network functionality
    bool initialize(int port = 8333, int max_peers = 8);
    void shutdown();
    bool start();
    void stop();
    
    // Peer management
    bool addPeer(const std::string& address, int port = 8333);
    bool removePeer(const std::string& address);
    bool connectToPeer(const std::string& address, int port = 8333);
    void disconnectPeer(const std::string& address);
    std::vector<PeerInfo> getConnectedPeers() const;
    std::vector<PeerInfo> getKnownPeers() const;
    
    // Message handling
    bool sendMessage(const std::string& peer_address, const NetworkMessage& message);
    bool broadcastMessage(const NetworkMessage& message);
    bool broadcastTransaction(const Transaction& transaction);
    bool broadcastBlock(const Block& block);
    
    // Network discovery
    bool discoverPeers();
    bool announceToPeers();
    
    // Statistics and monitoring
    NetworkStats getStats() const;
    std::string getStatus() const;
    
    // Configuration
    void setMaxPeers(int max_peers) { m_max_peers = max_peers; }
    void setConnectionTimeout(int timeout) { m_connection_timeout = timeout; }
    void setKeepAliveInterval(int interval) { m_keepalive_interval = interval; }
    
    // Callbacks
    void setTransactionCallback(std::function<void(const Transaction&)> callback) { m_transaction_callback = callback; }
    void setBlockCallback(std::function<void(const Block&)> callback) { m_block_callback = callback; }
    void setPeerCallback(std::function<void(const PeerInfo&)> callback) { m_peer_callback = callback; }

private:
    // Internal methods
    void run();
    void acceptConnections();
    void handlePeer(int client_socket, struct sockaddr_in client_addr);
    void handleMessage(const std::string& peer_address, const NetworkMessage& message);
    void processMessage(const std::string& peer_address, const std::string& raw_message);
    
    // Message creation
    NetworkMessage createVersionMessage(const std::string& peer_address);
    NetworkMessage createVerAckMessage();
    NetworkMessage createPingMessage();
    NetworkMessage createPongMessage();
    NetworkMessage createInvMessage(const std::vector<std::string>& inventory);
    NetworkMessage createGetDataMessage(const std::vector<std::string>& inventory);
    NetworkMessage createTxMessage(const Transaction& transaction);
    NetworkMessage createBlockMessage(const Block& block);
    
    // Message parsing
    bool parseMessage(const std::string& raw_message, NetworkMessage& message);
    bool validateMessage(const NetworkMessage& message);
    uint32_t calculateChecksum(const std::string& payload);
    std::string serializeMessage(const NetworkMessage& message);
    
    // Peer management
    void addPeerInternal(const PeerInfo& peer);
    void removePeerInternal(const std::string& address);
    void updatePeerStatus(const std::string& address, bool connected);
    void cleanupDisconnectedPeers();
    
    // Network utilities
    bool isLocalAddress(const std::string& address) const;
    std::string getLocalAddress() const;
    int createSocket() const;
    bool setSocketOptions(int socket_fd) const;
    
    // Threading
    void startWorkerThreads();
    void stopWorkerThreads();
    void workerThread();
    void discoveryThread();
    void keepAliveThread();
    
    // Member variables
    std::atomic<bool> m_running;
    std::atomic<bool> m_initialized;
    int m_port;
    int m_max_peers;
    int m_connection_timeout;
    int m_keepalive_interval;
    int m_server_socket;
    
    // Peer management
    mutable std::mutex m_peers_mutex;
    std::map<std::string, PeerInfo> m_peers;
    std::map<std::string, int> m_peer_sockets;
    
    // Message queues
    mutable std::mutex m_message_queue_mutex;
    std::queue<std::pair<std::string, NetworkMessage>> m_message_queue;
    
    // Threading
    std::thread m_accept_thread;
    std::thread m_worker_thread;
    std::thread m_discovery_thread;
    std::thread m_keepalive_thread;
    
    // Callbacks
    std::function<void(const Transaction&)> m_transaction_callback;
    std::function<void(const Block&)> m_block_callback;
    std::function<void(const PeerInfo&)> m_peer_callback;
    
    // Statistics
    mutable std::mutex m_stats_mutex;
    NetworkStats m_stats;
    uint64_t m_start_time;
    
    // Configuration
    bool m_discovery_enabled;
    bool m_manual_peers_only;
    std::vector<std::string> m_seed_nodes;
};

} // namespace dinero 