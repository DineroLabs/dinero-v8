#pragma once
#include "daemon/iservice.h"
#include "daemon/p2p_manager.h"
#include "daemon/services/prune_service.h"
#include "daemon/services/header_refresh_coalescer.h"
#include "daemon/services/stale_tip_recovery.h"  // issue #214: StaleTipState + decision
#include "network/port_mapper.h"
#include "network/stun_client.h"      // NAT traversal Phase C1 (unique_ptr<StunClient> member)
#include "network/tor_control.h"
#include "network/embedded_tor_process.h"
#include "network/tor_runtime_controller.h"
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
#include <mutex>

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
    struct RelayServiceLimits {
        size_t max_circuits{12};
        uint64_t bandwidth_bytes_per_second{2ULL * 1024 * 1024};
        size_t max_circuits_per_peer{2};
        uint32_t circuit_lifetime_seconds{1800};
        uint32_t requests_per_peer_per_minute{6};
    };
    struct RelayServiceStatus {
        std::string mode{"automatic"};
        bool enabled{false};
        RelayServiceLimits limits{};
        std::string message;
    };
    struct NetworkStatus {
        struct AddrmanSnapshot {
            bool available{false};
            size_t total_addresses{0};
            size_t new_addresses{0};
            size_t tried_addresses{0};
            size_t terrible_addresses{0};
            size_t banned_addresses{0};
            size_t relay_candidates{0};
            double avg_success_rate{0.0};
        };

        struct DynamicP2PGovernorSnapshot {
            bool available{false};
            std::string mode{"dry_run"};
            std::string candidate_source{"connected_peers"};
            size_t connected_outbound{0};
            size_t configured_seed_hot{0};
            size_t relay_capable_seen{0};
            std::vector<std::string> hot_peers;
            std::vector<std::string> warm_candidates;
            std::vector<std::string> relay_registration_candidates;
            std::vector<std::string> demote_candidates;
        };

        bool network_active{false};
        bool listening{false};
        uint16_t listen_port{0};
        size_t connections{0};
        size_t inbound{0};
        size_t outbound{0};
        size_t configured_seed_peers{0};
        size_t configured_seed_connections{0};
        size_t discovered_connections{0};
        size_t relay_peer_connections{0};
        std::vector<std::pair<std::string, uint16_t>> advertised_addresses;
        // True iff an EXPLICIT/confirmed reachable address (operator externalip
        // or port-mapping) is advertised — not merely a Gap-1-learned one. Used
        // by the relay-fallback gate so NAT'd nodes stay eligible.
        bool has_explicit_advertised{false};
        AddrmanSnapshot addrman;
        bool dynamic_p2p_enabled{true};
        std::string dynamic_p2p_mode{"active_slow_churn"};
        DynamicP2PGovernorSnapshot dynamic_p2p_governor;
        bool port_mapping_requested{false};
        bool port_mapping_active{false};
        bool port_mapping_upnp_compiled{false};
        bool port_mapping_natpmp_compiled{false};
        uint64_t port_mapping_attempts{0};
        uint64_t port_mapping_renewals{0};
        std::string port_mapping_mode{"disabled"};
        std::string port_mapping_protocol;
        std::string port_mapping_external_address;
        uint16_t port_mapping_external_port{0};
        std::string port_mapping_message;
        bool onion_transport_configured{false};
        bool onion_transport_enabled{false};
        bool onion_transport_reachable{false};
        bool onion_transport_auto_detected{false};
        std::string onion_proxy;
        std::string onion_transport_message;
        bool onion_service_requested{false};
        bool onion_service_active{false};
        std::string onion_service_address;
        std::string onion_service_authentication;
        std::string onion_service_message;
        std::string onion_service_mode{"off"};
        bool onion_service_embedded{false};

        // NAT traversal Phase C1: STUN-discovered public IP+port. Empty
        // when discovery hasn't completed or failed. Independent of
        // port_mapping_external_address (that one comes from UPnP/NAT-PMP);
        // surfacing both lets operators see whether UPnP-discovered and
        // STUN-discovered agree (they usually do; disagreement = double NAT).
        std::string stun_discovered_address;
        std::string stun_server_used;
        std::string stun_message;
        bool local_relay{false};
        bool relay_active{false};
        std::string relay_mode{"auto"};
        size_t relay_hints_received_self{0};
        size_t relay_hints_received_relay{0};
        size_t relay_hints_evicted_expired{0};
        size_t relay_hints_evicted_failure{0};
        size_t relay_directory_entries{0};
        size_t relay_directory_grace_pending{0};
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
    bool BanPeer(const std::string& target, std::chrono::seconds duration) {
        return p2p_mgr_ ? p2p_mgr_->ban_peer(target, duration) : false;
    }
    bool UnbanPeer(const std::string& target) {
        return p2p_mgr_ ? p2p_mgr_->unban_peer(target) : false;
    }
    void ClearBannedPeers() {
        if (p2p_mgr_) p2p_mgr_->clear_banned_peers();
    }
    std::vector<::P2PManager::BanEntry> ListBannedPeers() const {
        return p2p_mgr_ ? p2p_mgr_->list_banned_peers() : std::vector<::P2PManager::BanEntry>{};
    }
    bool IsPeerBanned(const std::string& address, uint16_t port = 0) const {
        return p2p_mgr_ ? p2p_mgr_->is_peer_banned(address, port) : false;
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
    NetworkStatus GetNetworkStatus() const;
    network::TorRuntimeStatus SetOnionServiceEnabled(bool enabled);
    network::TorRuntimeStatus SetOnionServiceMode(const std::string& mode);
    // Setter for the auto-mode relay-active toggle. Today mining.start /
    // mining.stop are the canonical callers (mining → carry more value →
    // worth running as a public relay), but any subsystem with a reason
    // to engage the auto relay role can call this — the lever isn't
    // exclusively a mining concern. See docs/network-participation.md.
    void SetRelayActive(bool active);
    bool IsRelayRoleEnabled() const;
    std::string RelayMode() const;
    RelayServiceStatus GetRelayServiceStatus() const;
    RelayServiceStatus SetRelayService(const std::string& mode,
                                       const RelayServiceLimits& limits);
    void BroadcastMessage(const ::P2PMessage& msg) {
        if (p2p_mgr_) p2p_mgr_->broadcast_message(msg);
    }

    // Headers-first block announcements may arrive hundreds of times during a
    // fast regtest/mining burst.  Coalesce those hints so our own getheaders
    // requests cannot make an honest peer exceed the inbound headers limiter.
    void RequestHeadersRefreshForBlockAnnouncement(const std::string& peer_addr);
    
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
    std::string relay_hints_file_path_;
    std::unique_ptr<network::TorRuntimeController> onion_runtime_;
    std::unique_ptr<network::EmbeddedTorProcess> embedded_tor_;
    network::TorOnionServiceConfig tor_onion_config_{};
    network::TorOnionServiceConfig external_tor_onion_config_{};
    std::string tor_mode_{"automatic"};
    std::string embedded_tor_path_;
    std::atomic<bool> onion_service_requested_{false};
    std::vector<std::string> seed_nodes_;
    std::vector<std::pair<std::string, uint16_t>> reconnect_targets_;
    bool offline_mode_{false};
    std::string onion_proxy_;
    // Preserve the operator's configured endpoint while automatic mode
    // temporarily points P2PManager at the embedded Tor child.
    std::string external_onion_proxy_;
    bool onion_proxy_configured_{false};
    bool onion_proxy_auto_detected_{false};
    bool onion_proxy_reachable_{false};
    std::string onion_proxy_message_{"disabled"};
    mutable std::mutex port_mapping_status_mutex_;
    bool port_mapping_requested_{false};
    bool port_mapping_active_{false};
    std::string port_mapping_mode_{"disabled"};
    std::string port_mapping_protocol_;
    std::string port_mapping_external_address_;
    uint16_t port_mapping_external_port_{0};
    std::string port_mapping_message_{"not requested"};
    uint64_t port_mapping_attempts_{0};
    uint64_t port_mapping_renewals_{0};
    // Port-mapping discovery (UPnP SSDP + IGD SOAP, NAT-PMP) is synchronous
    // and can stall for tens of seconds on routers that respond to SSDP but
    // hang on SOAP. Running it on P2PService::Start() froze dinero-qt at
    // launch. Move the discovery off the init thread; StopPortMapping joins.
    std::thread port_mapping_worker_;
    std::atomic<bool> port_mapping_cancel_{false};
    std::mutex port_mapping_wait_mutex_;
    std::condition_variable port_mapping_wait_cv_;

    // NAT traversal Phase C1: STUN client + mirror of last result. The
    // unique_ptr is heap-allocated to avoid pulling stun_client.h
    // (which pulls udp_socket.h, which pulls winsock2.h) into this
    // public header transitively.
    std::unique_ptr<network::StunClient> stun_client_;
    mutable std::mutex stun_status_mutex_;
    std::string stun_discovered_address_;
    std::string stun_server_used_;
    std::string stun_message_{"not run"};

    // Public relay role policy (Tier 3 of the three-tier model — see
    // docs/network-participation.md). Tier 1 (be a full P2P node) and
    // Tier 2 (try to be reachable, register through a relay if direct
    // inbound fails) are unconditionally on for every dinero-qt /
    // dinerod instance — neither depends on this flag or on mining.
    //
    // This flag governs only Tier 3: do we ADVERTISE NODE_RELAY and
    // accept dial-throughs from OTHER NAT'd peers? That tier carries a
    // real bandwidth/resource cost so it stays opt-in.
    //
    // Modes:
    //   p2p.relay=1/on/true  -> Tier 3 on, always.
    //   p2p.relay=0/off/no   -> Tier 3 off, always.
    //   p2p.relay=auto/unset -> Tier 3 on while this flag is set. Today
    //                          mining.start / mining.stop are the only
    //                          callers that flip the flag (mining moves
    //                          more value through the node, so the
    //                          incremental relay cost pays for itself);
    //                          future triggers — operator dashboard
    //                          opt-in toggle, spare-bandwidth heuristic,
    //                          etc. — can flip the same lever via
    //                          SetRelayActive() without protocol change.
    std::atomic<bool> relay_active_{false};
    mutable std::mutex relay_service_mutex_;
    RelayServiceStatus relay_service_status_{};
    std::string relay_service_policy_path_;

    // Periodic sync loop for headers-first + block scheduler in P2PService mode.
    std::atomic<bool> scheduler_tick_running_{false};
    std::thread scheduler_tick_thread_;
    std::chrono::milliseconds scheduler_tick_interval_{std::chrono::seconds(5)};
    std::chrono::steady_clock::time_point last_reconnect_probe_{};
    std::chrono::steady_clock::time_point dynamic_p2p_started_at_{};
    std::chrono::steady_clock::time_point last_dynamic_p2p_churn_{};
    std::chrono::seconds reconnect_probe_interval_{std::chrono::seconds(15)};

    std::mutex header_refresh_mutex_;
    std::unordered_map<std::string, daemon::HeaderRefreshState> header_refresh_states_;
    std::chrono::milliseconds header_refresh_minimum_interval_{std::chrono::seconds(1)};
    bool SendHeadersRefreshNow(const std::string& peer_addr);
    void FlushTrailingHeaderRefreshes(std::chrono::steady_clock::time_point now);

    // ── In-daemon staleness recovery (issue #214) ────────────────────────────
    // A node that has finished syncing relies on inv/headers announcements to
    // learn about new blocks. If those stop arriving on long-lived connections
    // (stale peer state), the best header freezes and the node silently falls
    // behind — it thinks it is "Synced" while the network moves on. The
    // BlockDownloadScheduler only acts on MISSING blocks below the known header
    // tip, so a frozen header tip leaves it idle. We detect a stalled best
    // header here and recover by re-issuing getheaders (pulling what the stale
    // connections stopped pushing). The external fleet height-watchdog stays as
    // a backstop until this is confirmed against a live stall; a peer-rotation
    // tier is deferred (see #214).
    //
    // The WHEN-to-act decision lives in DecideStaleTipAction (stale_tip_recovery.h)
    // so it can be unit-tested without sockets/singletons; this struct holds the
    // mutable stall-tracking state it advances. MaybeRecoverStaleTip() reads the
    // live height/peer-count, calls the decision, and does the getheaders I/O.
    daemon::StaleTipState stale_tip_state_;
    // Tunables. The threshold MUST sit several block-times above the normal
    // inter-block gap, NOT at it: TARGET_SPACING_SEC is 120s and block arrival is
    // Poisson, so ~37% of healthy gaps already exceed 120s. A 120s threshold
    // would therefore fire getheaders on roughly a third of all normal blocks —
    // log spam, and worse, it destroys the stall signal (you could no longer
    // tell a real stall from a routine quiet gap). 600s = 5× spacing puts the
    // false-fire rate at e^-5 (~0.7%), while staying below the external
    // height-watchdog's 900s so in-daemon recovery remains the first responder.
    std::chrono::seconds staleness_threshold_{std::chrono::seconds(600)};
    std::chrono::seconds staleness_getheaders_interval_{std::chrono::seconds(60)};
    void MaybeRecoverStaleTip(std::chrono::steady_clock::time_point now);

    // Event-driven catch-up: if a peer later advertises or validates to a
    // height above our current header view, ask that peer immediately instead
    // of waiting for the stale-tip timer. Throttled by peer+height.
    std::mutex peer_tip_getheaders_mutex_;
    std::unordered_map<std::string, uint32_t> last_peer_tip_getheaders_height_;
    void MaybeRequestHeadersForPeerTip(const std::string& peer_addr,
                                       uint32_t peer_height,
                                       const char* reason);

    // Internal message handler
    void HandleP2PMessage(const std::string& peer_addr, const ::P2PMessage& msg);
    void StartSchedulerTickLoop();
    void StopSchedulerTickLoop();
    bool IsDynamicP2PActive() const;
    std::string DynamicP2PMode() const;
    void MaybeRunDynamicP2PActiveChurn(std::chrono::steady_clock::time_point now);
    void StartPortMappingIfEnabled();

    // NAT traversal Phase C1: launch a STUN discovery round on a fresh
    // ephemeral UDP socket. Asynchronous — result is mirrored into
    // stun_discovered_address_ / stun_message_ under stun_status_mutex_,
    // and on success calls p2p_mgr_->add_advertised_address().
    void StartStunDiscoveryIfEnabled();
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
