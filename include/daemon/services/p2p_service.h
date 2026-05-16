#pragma once
#include "daemon/iservice.h"
#include "daemon/p2p_manager.h"
#include "daemon/services/prune_service.h"
#include "network/port_mapper.h"
#include "version.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <string>
#include <functional>
#include <cstdint>
#include <utility>
#include <vector>
#include <unordered_map>

namespace dinero {

// Forward declarations
class ILogger;
namespace daemon {
class AddressManagerService;
}

/**
 * P2PService - IService wrapper for P2PManager
 *
 * Wraps the P2P networking layer into IService lifecycle:
 * - Init() wires dependencies from DaemonContext
 * - Start() starts P2P networking (listening + seed connections)
 * - Stop() cleanly disconnects all peers and stops threads
 *
 * Dependencies: Logger, Config, Chainstate, Mempool
 *
 * The P2PManager provides:
 * - Peer discovery and connection management
 * - Bitcoin-like P2P protocol (version, verack, ping/pong, addr, inv, getdata)
 * - Message broadcasting and routing
 * - Persistent peer database
 * - Adaptive keepalive
 * - Thread-safe async message sending
 */
class P2PService : public IService {
public:
    struct NetworkTotals {
        uint64_t bytes_recv{0};
        uint64_t bytes_sent{0};
    };

    P2PService() = default;
    ~P2PService() override;

    std::string Name() const override { return "P2PManager"; }

    /**
     * Initialize P2P service with dependencies from context
     * - Stores logger, config, chainstate, mempool references
     * - Configures listen port and external IP from config
     * - Creates P2PManager instance
     */
    bool Init(DaemonContext& ctx) override;

    /**
     * Start P2P networking
     * - Loads persistent peer database
     * - Starts listening on configured port
     * - Connects to seed nodes
     * - Starts peer connection manager
     * - Wires message handlers to chainstate/mempool
     */
    bool Start() override;

    /**
     * Stop P2P networking
     * - Saves persistent peer database
     * - Disconnects all peers cleanly
     * - Stops all networking threads
     */
    void Stop() override;

    /**
     * Get reference to wrapped P2PManager
     * Use this to access P2P functionality
     */
    ::P2PManager& get() { return *p2p_mgr_; }
    const ::P2PManager& get() const { return *p2p_mgr_; }

    /**
     * Event hooks for block/tx relay
     * These are set by DaemonApp to wire P2P → Chainstate/Mempool
     */
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnNewBlock;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnNewTx;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnInv;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnGetData;

    // Phase C.3: Headers-first sync hooks
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnGetHeaders;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnHeaders;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnCompactBlock;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnGetBlockTxn;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnBlockTxn;

    // Phase P.3: Utreexo block relay (CSN receives block + proof)
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnUtxoBlock;

    // Phase #4: Utreexo tx relay (CSN receives tx + per-input proofs)
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnUtxoTx;

    // Phase 7.4: Utreexo proof serving protocol (bridge nodes)
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnGetUtreexoProof;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnGetUtreexoHeaders;

    // Phase 9.3: Proof gossip protocol (best-effort proof availability)
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnInvProof;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnGetProof;
    std::function<void(const std::string& peer_addr, const ::P2PMessage& msg)> OnProofData;

    // Forward commonly used methods for convenience
    size_t GetPeerCount() const { return p2p_mgr_ ? p2p_mgr_->get_peer_count() : 0; }
    std::vector<::PeerInfo> GetConnectedPeers() const {
        return p2p_mgr_ ? p2p_mgr_->get_connected_peers() : std::vector<::PeerInfo>{};
    }
    bool ConnectToPeer(const std::string& address, uint16_t port) {
        return p2p_mgr_ ? p2p_mgr_->connect_to_peer(address, port) : false;
    }
    bool DisconnectPeer(const std::string& peer_address) {
        if (!p2p_mgr_) return false;
        p2p_mgr_->disconnect_peer(peer_address);
        return true;
    }
    bool AddSeedNode(const std::string& address, uint16_t port) {
        if (!p2p_mgr_) return false;
        p2p_mgr_->add_seed_node(address, port);
        return true;
    }
    bool RemoveSeedNode(const std::string& address, uint16_t port) {
        return p2p_mgr_ ? p2p_mgr_->remove_seed_node(address, port) : false;
    }
    std::vector<std::pair<std::string, uint16_t>> GetAddedNodes() const {
        return p2p_mgr_ ? p2p_mgr_->get_seed_nodes() : std::vector<std::pair<std::string, uint16_t>>{};
    }
    bool SetNetworkActive(bool active);
    bool IsNetworkActive() const { return p2p_mgr_ ? p2p_mgr_->is_network_active() : false; }
    size_t SendPingToAll();
    NetworkTotals GetNetworkTotals() const;
    void BroadcastMessage(const ::P2PMessage& msg) {
        if (p2p_mgr_) p2p_mgr_->broadcast_message(msg);
    }
    
    // Protocol constants (Dinero-specific)
    static constexpr uint32_t GetProtocolVersion() { return 70016; }
    static std::string GetUserAgent() { return DineroUserAgent(); }

private:
    std::unique_ptr<::P2PManager> p2p_mgr_;

    // Logger dependencies (dual pattern during migration):
    // - logger_: Legacy LoggerService (keep for compatibility during migration)
    // - logger_interface_: New ILogger dependency injection (actively used)
    std::shared_ptr<class LoggerService> logger_;
    ILogger* logger_interface_ = nullptr;

    std::shared_ptr<class ConfigService> config_;
    std::shared_ptr<class ChainstateService> chainstate_;
    std::shared_ptr<class MempoolService> mempool_;
    std::shared_ptr<daemon::PruneService> prune_;
    std::shared_ptr<daemon::AddressManagerService> address_manager_;

    // P2P configuration
    uint16_t listen_port_ = 20999;
    std::string external_ip_;
    std::string peers_file_path_;
    std::vector<std::string> seed_nodes_;
    std::vector<std::pair<std::string, uint16_t>> reconnect_targets_;
    bool offline_mode_{false};
    std::unique_ptr<network::PortMappingSession> port_mapping_;

    // Periodic sync loop for headers-first + block scheduler in P2PService mode.
    std::atomic<bool> scheduler_tick_running_{false};
    std::thread scheduler_tick_thread_;
    std::chrono::milliseconds scheduler_tick_interval_{std::chrono::seconds(5)};
    std::chrono::steady_clock::time_point last_reconnect_probe_{};
    std::chrono::seconds reconnect_probe_interval_{std::chrono::seconds(15)};

    // Internal message handler
    void HandleP2PMessage(const std::string& peer_addr, const ::P2PMessage& msg);
    void StartSchedulerTickLoop();
    void StopSchedulerTickLoop();
    void StartPortMappingIfEnabled();
    void StopPortMapping();

    // Per-peer headers rate limiting (eclipse/DoS protection)
    // Tracks timestamps of recent headers messages per peer
    struct HeadersRateState {
        int64_t last_headers_time{0};   // Unix ms of last headers message
        uint32_t headers_count{0};       // Headers messages in current window
        int64_t window_start{0};         // Start of current 1-second window
    };
    std::unordered_map<std::string, HeadersRateState> headers_rate_tracker_;
    static constexpr uint32_t MAX_HEADERS_PER_SECOND = 32;  // Max headers msgs/sec/peer

    /**
     * Check if peer is allowed to send another headers message
     * Returns false if rate exceeded — caller should score the peer
     */
    bool checkHeadersRate(const std::string& peer_addr);

    // Per-peer tx rate limiting (RBF flood protection)
    // Limits total tx messages per peer to prevent RBF churn DoS
    std::unordered_map<std::string, HeadersRateState> tx_rate_tracker_;
    static constexpr uint32_t MAX_TX_PER_SECOND = 10;  // Max tx msgs/sec/peer
    bool checkTxRate(const std::string& peer_addr);
};

} // namespace dinero
