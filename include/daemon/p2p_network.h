#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <queue>
#include <chrono>
#include "compat/net_compat.h"
#include "common/logger.h"
#include "primitives/block.h"
#include "wallet/transaction.h"

namespace dinero {

// Forward declarations
class P2PConnection;
class P2PMessage;
class P2PNetwork;

// P2P message types (Bitcoin-compatible)
enum class P2PMessageType : uint32_t {
    VERSION = 0x76657273,      // "vers"
    VERACK = 0x76657261,       // "vera"
    INV = 0x696e76,            // "inv "
    GETDATA = 0x67657464,      // "getd"
    BLOCK = 0x626c6f63,        // "bloc"
    TX = 0x7478,               // "tx  "
    PING = 0x70696e67,         // "ping"
    PONG = 0x706f6e67,         // "pong"
    ADDR = 0x61646472,         // "addr"
    GETADDR = 0x67657461,      // "geta"
    REJECT = 0x72656a65,       // "reje"
    ALERT = 0x616c6572,        // "aler"
    MEMPOOL = 0x6d656d70,      // "memp"
    FILTERLOAD = 0x66696c74,   // "filt"
    FILTERADD = 0x66696c61,    // "fila"
    FILTERCLEAR = 0x66696c63,  // "filc"
    MERKLEBLOCK = 0x6d65726b,  // "merk"
    SENDHEADERS = 0x73656e64,  // "send"
    HEADERS = 0x68656164,      // "head"
    GETBLOCKS = 0x67657462,    // "getb"
    GETHEADERS = 0x67657468,   // "geth"
    FEEFILTER = 0x66656566,    // "feef"
    SENDCMPCT = 0x73656e63,    // "senc"
    CMPCTBLOCK = 0x636d7063,   // "cmpc"
    GETBLOCKTXN = 0x67657474,  // "gett"
    BLOCKTXN = 0x626c6f74,     // "blot"
    UNKNOWN = 0x00000000
};

// P2P message structure
struct P2PMessage {
    P2PMessageType type;
    std::vector<uint8_t> payload;
    uint32_t checksum;
    uint32_t length;
    
    P2PMessage() : type(P2PMessageType::UNKNOWN), checksum(0), length(0) {}
    P2PMessage(P2PMessageType t, const std::vector<uint8_t>& p = {}) 
        : type(t), payload(p), checksum(0), length(p.size()) {}
};

// Peer information
struct PeerInfo {
    std::string address;
    uint16_t port;
    std::string user_agent;
    uint32_t version;
    uint64_t services;
    int64_t last_seen;
    bool inbound;
    bool connected;
    uint32_t score;
    
    PeerInfo() : port(0), version(0), services(0), last_seen(0), 
                 inbound(false), connected(false), score(100) {}
    PeerInfo(const std::string& addr, uint16_t p) 
        : address(addr), port(p), version(0), services(0), last_seen(0),
          inbound(false), connected(false), score(100) {}
};

// Connection state
enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    HANDSHAKING,
    READY,
    ERROR
};

// P2P connection class
class P2PConnection {
private:
    int socket_fd;
    std::string remote_address;
    uint16_t remote_port;
    ConnectionState state;
    std::atomic<bool> running;
    std::thread read_thread;
    std::thread write_thread;
    std::mutex write_mutex;
    std::queue<P2PMessage> write_queue;
    std::condition_variable write_cv;
    
    // Message handlers
    std::map<P2PMessageType, std::function<void(const P2PMessage&)>> message_handlers;
    
    // Statistics
    uint64_t bytes_received;
    uint64_t bytes_sent;
    uint64_t messages_received;
    uint64_t messages_sent;
    
public:
    P2PConnection(const std::string& address, uint16_t port);
    ~P2PConnection();
    
    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const { return state == ConnectionState::READY; }
    ConnectionState getState() const { return state; }
    
    // Message handling
    bool sendMessage(const P2PMessage& message);
    void setMessageHandler(P2PMessageType type, std::function<void(const P2PMessage&)> handler);
    
    // Statistics
    uint64_t getBytesReceived() const { return bytes_received; }
    uint64_t getBytesSent() const { return bytes_sent; }
    uint64_t getMessagesReceived() const { return messages_received; }
    uint64_t getMessagesSent() const { return messages_sent; }
    
    // Getters
    std::string getRemoteAddress() const { return remote_address; }
    uint16_t getRemotePort() const { return remote_port; }
    
private:
    void readLoop();
    void writeLoop();
    bool performHandshake();
    P2PMessage readMessage();
    bool writeMessage(const P2PMessage& message);
    uint32_t calculateChecksum(const std::vector<uint8_t>& data);
};

// P2P network manager
class P2PNetwork {
private:
    std::vector<std::shared_ptr<P2PConnection>> connections;
    std::map<std::string, std::shared_ptr<PeerInfo>> peers;
    std::mutex connections_mutex;
    std::mutex peers_mutex;
    
    // Network configuration
    uint16_t listen_port;
    uint32_t max_connections;
    uint32_t max_inbound_connections;
    uint32_t max_outbound_connections;
    
    // Network state
    std::atomic<bool> running;
    std::thread listener_thread;
    std::thread discovery_thread;
    std::thread maintenance_thread;
    
    // DNS seeds and hardcoded peers
    std::vector<std::string> dns_seeds;
    std::vector<std::pair<std::string, uint16_t>> hardcoded_peers;
    
    // Callbacks
    std::function<void(const std::string&, const std::vector<uint8_t>&)> transaction_callback;
    std::function<void(const std::string&, const std::vector<uint8_t>&)> block_callback;
    
public:
    P2PNetwork(uint16_t port = 8333);
    ~P2PNetwork();
    
    // Network management
    bool start();
    void stop();
    bool isRunning() const { return running; }
    
    // Connection management
    bool connectToPeer(const std::string& address, uint16_t port);
    void disconnectPeer(const std::string& address);
    std::vector<std::string> getConnectedPeers() const;
    
    // Message broadcasting
    bool broadcastTransaction(const std::vector<uint8_t>& tx_data);
    bool broadcastBlock(const std::vector<uint8_t>& block_data);
    
    // Peer discovery
    void addDNSSeed(const std::string& seed);
    void addHardcodedPeer(const std::string& address, uint16_t port);
    void discoverPeers();
    
    // Callbacks
    void setTransactionCallback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback);
    void setBlockCallback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback);
    
    // Statistics
    uint32_t getConnectionCount() const;
    uint32_t getPeerCount() const;
    std::map<std::string, uint64_t> getNetworkStats() const;
    
private:
    void listenerLoop();
    void discoveryLoop();
    void maintenanceLoop();
    void handleIncomingConnection(int client_socket, const std::string& client_address);
    void handlePeerDisconnection(const std::string& peer_address);
    void processMessage(const std::string& peer_address, const P2PMessage& message);
    
    // Message handlers
    void handleVersion(const std::string& peer_address, const P2PMessage& message);
    void handleVerack(const std::string& peer_address, const P2PMessage& message);
    void handleInv(const std::string& peer_address, const P2PMessage& message);
    void handleGetData(const std::string& peer_address, const P2PMessage& message);
    void handleBlock(const std::string& peer_address, const P2PMessage& message);
    void handleTx(const std::string& peer_address, const P2PMessage& message);
    void handlePing(const std::string& peer_address, const P2PMessage& message);
    void handlePong(const std::string& peer_address, const P2PMessage& message);
    void handleAddr(const std::string& peer_address, const P2PMessage& message);
    void handleGetAddr(const std::string& peer_address, const P2PMessage& message);
};

// P2P message utilities
class P2PMessageUtils {
public:
    static std::string messageTypeToString(P2PMessageType type);
    static P2PMessageType stringToMessageType(const std::string& str);
    static std::vector<uint8_t> serializeMessage(const P2PMessage& message);
    static bool deserializeMessage(const std::vector<uint8_t>& data, P2PMessage& message);
    static uint32_t calculateChecksum(const std::vector<uint8_t>& data);
    static bool verifyChecksum(const std::vector<uint8_t>& data, uint32_t checksum);
    
    // Message creation helpers
    static P2PMessage createVersionMessage(uint32_t version, uint64_t services, 
                                         const std::string& user_agent, int64_t timestamp);
    static P2PMessage createVerackMessage();
    static P2PMessage createPingMessage(uint64_t nonce);
    static P2PMessage createPongMessage(uint64_t nonce);
    static P2PMessage createInvMessage(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& inventory);
    static P2PMessage createGetDataMessage(const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& inventory);
    static P2PMessage createBlockMessage(const std::vector<uint8_t>& block_data);
    static P2PMessage createTxMessage(const std::vector<uint8_t>& tx_data);
    static P2PMessage createAddrMessage(const std::vector<PeerInfo>& peers);
    static P2PMessage createGetAddrMessage();
};

} // namespace dinero 