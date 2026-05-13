#include "p2p/PeerManager.hpp"
#include <memory>

/**
 * @brief Null implementation of PeerManager for Qt-free builds
 * 
 * This stub implementation is used when P2P networking is disabled
 * (either explicitly or because Qt is not available).
 */
class PeerManagerNull final : public dinero::PeerManager {
public:
    void start() override {
        // No-op: P2P disabled
    }
    
    void stop() override {
        // No-op: P2P disabled
    }
    
    bool isRunning() const override {
        return false; // P2P never runs in null implementation
    }
    
    std::vector<dinero::P2PPeerInfo> getPeers() const override {
        return {}; // No peers when P2P disabled
    }
    
    int getConnectionCount() const override {
        return 0; // No connections when P2P disabled
    }
    
    bool isListening() const override {
        return false; // Not listening when P2P disabled
    }
    
    void addNode(const std::string& address) override {
        // No-op: P2P disabled
        (void)address; // Suppress unused parameter warning
    }
    
    void removeNode(const std::string& address) override {
        // No-op: P2P disabled
        (void)address; // Suppress unused parameter warning
    }
    
    void disconnectNode(const std::string& address) override {
        // No-op: P2P disabled
        (void)address; // Suppress unused parameter warning
    }
    
    int getNetworkPort() const override {
        return 0; 
    }
    
    std::string getNetworkVersion() const override { 
        return "null"; 
    }
    int getBestHeaderHeight() const override { 
        return 0; 
    }
    int getPeerCount() const override { 
        return 0; 
    }
};

namespace dinero {

// Factory function implementation
std::unique_ptr<PeerManager> MakePeerManager() {
#ifdef DINERO_ENABLE_P2P
    // Forward declaration - implemented in Qt-based P2P library
    extern std::unique_ptr<PeerManager> MakePeerManagerQt();
    return MakePeerManagerQt();
#else
    // Return null implementation when P2P is disabled
    return std::make_unique<PeerManagerNull>();
#endif
}

} // namespace dinero
