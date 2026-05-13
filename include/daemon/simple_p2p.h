#ifndef DINERO_SIMPLE_P2P_H
#define DINERO_SIMPLE_P2P_H

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>

namespace dinero {

// Forward declarations
class ILogger;  // Forward declaration for dependency injection

// Simple P2P networking without DNS seeds
class SimpleP2P {
public:
    struct PeerConnection {
        std::string address;
        uint16_t port;
        bool connected;
        uint64_t last_seen;
        
        PeerConnection(const std::string& addr, uint16_t p) 
            : address(addr), port(p), connected(false), last_seen(0) {}
    };

    SimpleP2P(uint16_t listen_port = 8333);
    ~SimpleP2P();

    // Basic network operations
    bool start();
    void stop();
    bool isRunning() const { return m_running; }

    // Peer management
    void addHardcodedPeer(const std::string& address, uint16_t port);
    bool connectToPeer(const std::string& address, uint16_t port);
    void disconnectPeer(const std::string& address, uint16_t port);
    std::vector<PeerConnection> getConnectedPeers() const;

    // Message broadcasting (placeholder)
    bool broadcastTransaction(const std::vector<uint8_t>& tx_data);
    bool broadcastBlock(const std::vector<uint8_t>& block_data);

    // Callbacks for incoming data
    void setTransactionCallback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback);
    void setBlockCallback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback);

    // Logger dependency injection
    void setLogger(ILogger* logger) { m_logger = logger; }

private:
    uint16_t m_listen_port;
    std::atomic<bool> m_running;
    std::vector<PeerConnection> m_peers;
    std::vector<PeerConnection> m_hardcoded_peers;
    
    std::thread m_network_thread;
    std::function<void(const std::string&, const std::vector<uint8_t>&)> m_tx_callback;
    std::function<void(const std::string&, const std::vector<uint8_t>&)> m_block_callback;

    void networkLoop();
    void attemptPeerConnections();
    void handleIncomingConnections();

    // Logger dependency injection
    ILogger* m_logger = nullptr;

    // Helper macros for cleaner DI logging
    #define P2PLOG_INFO(msg)  if (m_logger) m_logger->info(msg)
    #define P2PLOG_DEBUG(msg) if (m_logger) m_logger->debug(msg)
    #define P2PLOG_WARN(msg)  if (m_logger) m_logger->warning(msg)
    #define P2PLOG_ERR(msg)   if (m_logger) m_logger->error(msg)
};

} // namespace dinero

#endif // DINERO_SIMPLE_P2P_H