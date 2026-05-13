#pragma once
#include <string>
#include <vector>
#include <memory>

namespace dinero {

struct P2PPeerInfo {
    std::string addr;
    int inbound = 0;
    int height = 0;
    std::string version;
    int64_t conntime = 0;
    bool inbound_connection = false;
};

/**
 * @brief Abstract P2P networking interface
 * 
 * Provides a Qt-free interface for P2P operations that can be implemented
 * with Qt networking (when available) or as a null stub (when Qt disabled).
 */
class PeerManager {
public:
    virtual ~PeerManager() = default;
    
    // Core P2P operations
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    
    // Peer information
    virtual std::vector<dinero::P2PPeerInfo> getPeers() const = 0;
    virtual int getConnectionCount() const = 0;
    virtual bool isListening() const = 0;
    
    // Network operations
    virtual void addNode(const std::string& address) = 0;
    virtual void removeNode(const std::string& address) = 0;
    virtual void disconnectNode(const std::string& address) = 0;
    
    // Network info for RPC
    virtual int getNetworkPort() const = 0;
    virtual std::string getNetworkVersion() const = 0;
    virtual int getBestHeaderHeight() const = 0;
    virtual int getPeerCount() const = 0;
};

/**
 * @brief Factory function to create PeerManager instance
 * 
 * Returns a real Qt-based implementation when P2P is enabled,
 * or a null stub implementation when P2P is disabled.
 */
std::unique_ptr<dinero::PeerManager> MakePeerManager();

} // namespace dinero
