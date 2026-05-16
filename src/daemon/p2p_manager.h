#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>

#include "p2p/addrman.h"

// Cross-platform P2P networking (no Qt dependencies)
// Handles peer discovery, connections, and message routing

// Ring 3 Phase 4c: Peer Lifetime States
// ======================================
// TS1 requires explicit lifetime tracking to prevent use-after-free
enum class PeerLifetimeState {
    ALLOCATED,      // Peer object created, not yet running
    RUNNING,        // Peer thread active, can receive messages
    STOPPING,       // Shutdown requested, thread exiting
    JOINED,         // Thread joined, object still exists
    DESTROYED       // Object destroyed (not used - tracked by shared_ptr)
};

struct PeerInfo {
    std::string address;
    uint16_t port;
    std::string user_agent;
    uint32_t protocol_version{0};
    uint64_t service_flags{0};
    uint32_t start_height{0};
    uint32_t best_known_height{0};
    uint32_t synced_headers{0};
    uint32_t best_height{0};
    uint32_t synced_blocks{0};
    bool compact_blocks_enabled{false};
    bool compact_blocks_announce{false};
    uint64_t compact_blocks_version{0};
    uint64_t bytes_recv{0};
    uint64_t bytes_sent{0};
    std::chrono::system_clock::time_point connected_since;
    std::chrono::system_clock::time_point last_message_at;
    std::chrono::system_clock::time_point last_seen;
    std::chrono::steady_clock::time_point last_ping_sent{std::chrono::steady_clock::now()};
    bool is_outbound;
    bool is_connected;
    int socket_fd;

    // ========================================================================
    // PHASE C: Persistent Peer Database & Adaptive Keepalive
    // ========================================================================
    int64_t last_seen_unix{0};      // Unix timestamp for peers.dat persistence
    double avg_latency_ms{0.0};     // Exponential moving average of ping latency

    // ========================================================================
    // Phase B (v8 peer discovery): periodic getaddr cadence per peer.
    // Initialized to time_point{} (the epoch) so the very-first keepalive
    // tick after handshake triggers an additional getaddr if the initial
    // post-handshake one was missed. complete_handshake() records the
    // initial send; keepalive_loop() re-sends every getaddr_interval_.
    // ========================================================================
    std::chrono::steady_clock::time_point last_getaddr_sent{};

    // ========================================================================
    // Ring 3 Phase 4c: Thread-Safe Lifetime Tracking
    // ========================================================================
    std::atomic<PeerLifetimeState> lifetime_state{PeerLifetimeState::ALLOCATED};

    // Per-peer send mutex: serializes send() calls on this peer's socket
    // to prevent TCP stream interleaving from concurrent threads.
    std::unique_ptr<std::mutex> send_mutex{std::make_unique<std::mutex>()};

    // Ring 3 Phase 4c: Move constructor (atomic is not movable, must load/store)
    PeerInfo(PeerInfo&& other) noexcept
        : address(std::move(other.address)),
          port(other.port),
          user_agent(std::move(other.user_agent)),
          protocol_version(other.protocol_version),
          service_flags(other.service_flags),
          start_height(other.start_height),
          best_known_height(other.best_known_height),
          synced_headers(other.synced_headers),
          best_height(other.best_height),
          synced_blocks(other.synced_blocks),
          compact_blocks_enabled(other.compact_blocks_enabled),
          compact_blocks_announce(other.compact_blocks_announce),
          compact_blocks_version(other.compact_blocks_version),
          bytes_recv(other.bytes_recv),
          bytes_sent(other.bytes_sent),
          connected_since(other.connected_since),
          last_message_at(other.last_message_at),
          last_seen(other.last_seen),
          last_ping_sent(other.last_ping_sent),
          is_outbound(other.is_outbound),
          is_connected(other.is_connected),
          socket_fd(other.socket_fd),
          last_seen_unix(other.last_seen_unix),
          avg_latency_ms(other.avg_latency_ms),
          last_getaddr_sent(other.last_getaddr_sent),
          lifetime_state(other.lifetime_state.load()) {}

    // Default constructor
    PeerInfo() = default;

    // Delete copy constructor (atomic is not copyable)
    PeerInfo(const PeerInfo&) = delete;
    PeerInfo& operator=(const PeerInfo&) = delete;
    PeerInfo& operator=(PeerInfo&&) = delete;

    std::string to_string() const {
        return address + ":" + std::to_string(port);
    }
};

struct P2PMessage {
    std::string command;
    std::vector<uint8_t> payload;
    uint32_t checksum;
    
    // Standard Bitcoin-like message types
    static P2PMessage create_version(uint32_t protocol_version, uint32_t best_height,
                                     uint64_t services = 0,
                                     const std::string& user_agent = "");
    static P2PMessage create_verack();
    static P2PMessage create_ping(uint64_t nonce);
    static P2PMessage create_pong(uint64_t nonce);
    static P2PMessage create_getaddr();
    static P2PMessage create_addr(const std::vector<PeerInfo>& peers);
    static P2PMessage create_inv(const std::vector<std::string>& hashes, const std::string& type);
    static P2PMessage create_inv_binary(const uint8_t* hash, size_t hash_len, uint32_t inv_type);
    static P2PMessage create_getdata(const std::vector<std::string>& hashes, const std::string& type);
    static P2PMessage create_getdata_binary(const uint8_t* hash, size_t hash_len, uint32_t inv_type);
    static P2PMessage create_block(const std::string& block_hex);  // Phase C.2: Block transmission

    // Phase C.3: Headers-first sync
    static P2PMessage create_getheaders(const std::vector<std::string>& locator);
    static P2PMessage create_headers(const std::vector<std::string>& header_hexes);

    // Serialization
    std::vector<uint8_t> serialize() const;
    static std::unique_ptr<P2PMessage> deserialize(const std::vector<uint8_t>& data);
};

class P2PManager {
public:
    struct BanEntry {
        std::string target;
        int64_t ban_created{0};
        int64_t banned_until{0};
    };

    using MessageHandler = std::function<void(const std::string& peer_address, const P2PMessage& message)>;
    using PeerConnectedHandler = std::function<void(const std::string& peer_address)>;
    using PeerDisconnectedHandler = std::function<void(const std::string& peer_address)>;
    using HeightProvider = std::function<uint32_t()>;  // P2P sync fix: Callback to get current chain height
    using ServiceFlagsProvider = std::function<uint64_t()>;  // Returns advertised service flags

    P2PManager(uint16_t listen_port = 20999, const std::string& external_ip = "");
    ~P2PManager();
    
    // Lifecycle
    bool start();
    void stop();
    bool is_running() const { return running_; }

    // Observability: Socket readiness (for deterministic tests)
    bool IsListening() const noexcept {
        return socket_listening_.load(std::memory_order_acquire);
    }

    bool WaitUntilListening(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(ready_mutex_);
        return ready_cv_.wait_for(lock, timeout, [this] {
            return socket_listening_.load(std::memory_order_acquire);
        });
    }

    // Peer management
    void add_seed_node(const std::string& address, uint16_t port);
    bool remove_seed_node(const std::string& address, uint16_t port);
    std::vector<std::pair<std::string, uint16_t>> get_seed_nodes() const;
    bool connect_to_peer(const std::string& address, uint16_t port);
    void disconnect_peer(const std::string& peer_address);
    bool ban_peer(const std::string& target, std::chrono::seconds duration);
    bool unban_peer(const std::string& target);
    void clear_banned_peers();
    std::vector<BanEntry> list_banned_peers() const;
    bool is_peer_banned(const std::string& address, uint16_t port = 0) const;
    void set_network_active(bool active);
    bool is_network_active() const { return network_active_.load(std::memory_order_acquire); }
    void set_address_manager(dinero::p2p::AddressManager* address_manager);
    void set_onion_proxy(const std::string& proxy_host, uint16_t proxy_port, bool log_change = true);
    bool onion_proxy_enabled() const;
    std::string onion_proxy_endpoint() const;
    bool probe_onion_proxy(std::string* message = nullptr);
    void add_advertised_address(const std::string& address, uint16_t port);
    std::vector<std::pair<std::string, uint16_t>> get_advertised_addresses() const;
    
    // Message handling
    void set_message_handler(MessageHandler handler) { message_handler_ = handler; }
    void set_peer_connected_handler(PeerConnectedHandler handler) { peer_connected_handler_ = handler; }
    void set_peer_disconnected_handler(PeerDisconnectedHandler handler) { peer_disconnected_handler_ = handler; }

    // P2P sync fix: Set height provider for version handshake
    void set_height_provider(HeightProvider provider) { height_provider_ = provider; }

    // Set service flags provider (prune-aware: NODE_NETWORK vs NODE_NETWORK_LIMITED)
    void set_service_flags_provider(ServiceFlagsProvider provider) { service_flags_provider_ = provider; }
    void set_user_agent(const std::string& user_agent);
    
    // Send messages
    bool send_to_peer(const std::string& peer_address, const P2PMessage& message);
    void broadcast_message(const P2PMessage& message);
    void broadcast_message_async(const P2PMessage& message);  // Non-blocking broadcast
    
    // Peer info
    std::vector<PeerInfo> get_connected_peers() const;
    size_t get_peer_count() const;
    PeerInfo* get_peer_info(const std::string& peer_address);
    bool peer_has_service_flags(const std::string& peer_address, uint64_t required_flags) const;
    bool peer_prefers_compact_blocks(const std::string& peer_address) const;
    void update_peer_height(const std::string& peer_address, uint32_t height);
    void update_peer_synced_headers(const std::string& peer_address, uint32_t height);
    void update_peer_synced_blocks(const std::string& peer_address, uint32_t height);
    
    // Network info
    uint16_t get_listen_port() const { return listen_port_; }
    std::string get_user_agent() const { return user_agent_; }

    // ========================================================================
    // PHASE C: Persistent Peer Database & Adaptive Keepalive
    // ========================================================================
    void load_peers(const std::string& peers_file_path);
    void save_peers(const std::string& peers_file_path);
    void save_peers_with_seeds(const std::string& peers_file_path);

    // Phase C (v8 peer discovery): atomic write helper. Writes `content`
    // to `peers_file_path + ".tmp"`, fsync()s the fd, then renames in
    // place. Survives mid-write crashes and power loss. Used by both
    // save_peers and save_peers_with_seeds.
    void write_peers_file_atomic(const std::string& peers_file_path,
                                 const std::string& content);

    // Phase B (v8 peer discovery): forward newly-received addresses to a
    // small random subset of other outbound peers. Without this, addresses
    // learned by one node never propagate through the network and
    // community-hosted peers can't join the reachable set.
    void relay_addresses_to_peers(
        const std::string& source_peer,
        const std::vector<std::pair<std::string, uint16_t>>& addresses);
    void mark_peer_seen(const std::string& peer_address);

private:
    // Outbox for async sending
    struct OutMsg {
        std::string peer_id;
        std::shared_ptr<std::vector<uint8_t>> data;
        size_t offset{0};
        int tries{0};
        std::chrono::steady_clock::time_point queued_at;
    };
    
    std::mutex outbox_mutex_;
    std::condition_variable outbox_cv_;
    std::deque<OutMsg> outbox_queue_;
    std::unique_ptr<std::thread> outbox_thread_;
    static constexpr size_t MAX_OUTBOX_SIZE = 10000;
    static constexpr int MAX_SEND_TRIES = 10;
    static constexpr int SEND_TIMEOUT_SEC = 5;
    static constexpr size_t MAX_OUTBOUND_CONNECTIONS = 8;
    static constexpr size_t MAX_INBOUND_CONNECTIONS = 125;
    static constexpr size_t MAX_INBOUND_PER_IP = 6;
    // Serialize writes per-socket (not globally) to avoid interleaved frames
    // while preventing one stalled peer from blocking all other peers.
    mutable std::mutex socket_send_mutexes_guard_;
    mutable std::unordered_map<int, std::shared_ptr<std::mutex>> socket_send_mutexes_;

    // Ring 3 Phase 4e: TS3 Liveness - Interruptible waits for event loops
    std::mutex keepalive_mutex_;
    std::condition_variable keepalive_cv_;
    std::mutex connection_manager_mutex_;
    std::condition_variable connection_manager_cv_;

    uint16_t listen_port_;
    std::string user_agent_;
    std::string external_ip_;  // Node's external IP (for self-loop detection)
    uint32_t protocol_version_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> network_active_{true};

    // Socket readiness observability (for deterministic tests)
    std::atomic<bool> socket_listening_{false};
    mutable std::mutex ready_mutex_;
    std::condition_variable ready_cv_;

    // Threading
    std::unique_ptr<std::thread> listen_thread_;
    std::unique_ptr<std::thread> connection_manager_thread_;
    std::unique_ptr<std::thread> keepalive_thread_;  // Phase C: Adaptive keepalive
    std::vector<std::unique_ptr<std::thread>> peer_threads_;

    // Peer management
    mutable std::mutex peers_mutex_;
    // Ring 3 Phase 4c: Changed unique_ptr → shared_ptr for TS1 compliance
    // Allows worker threads to hold weak_ptr, preventing use-after-free
    std::unordered_map<std::string, std::shared_ptr<PeerInfo>> connected_peers_;
    std::unordered_set<std::string> connecting_peers_;  // Guards against duplicate connection attempts
    std::vector<std::pair<std::string, uint16_t>> seed_nodes_;
    std::vector<std::pair<std::string, uint16_t>> advertised_addresses_;
    std::string onion_proxy_host_;
    uint16_t onion_proxy_port_{0};
    dinero::p2p::AddressManager* address_manager_{nullptr};  // Owned by AddressManagerService
    std::string peers_file_path_;  // Phase C: Persistent peer database path
    mutable std::mutex bans_mutex_;
    std::unordered_map<std::string, BanEntry> banned_peers_;
    
    // Message handling
    MessageHandler message_handler_;
    PeerConnectedHandler peer_connected_handler_;
    PeerDisconnectedHandler peer_disconnected_handler_;
    HeightProvider height_provider_;  // P2P sync fix: Get chain height for version handshake
    ServiceFlagsProvider service_flags_provider_;  // Returns advertised service flags (prune-aware)

    // Network threads
    void listen_loop();
    void connection_manager_loop();
    // Ring 3 Phase 4c: Changed to shared_ptr for TS1 compliance
    void peer_handler_loop(std::shared_ptr<PeerInfo> peer);
    void outbox_loop();
    void keepalive_loop();  // Phase C: Adaptive keepalive thread
    
    // Connection management
    void handle_incoming_connection(int client_socket, const std::string& client_address);
    bool perform_handshake(PeerInfo* peer);
    void cleanup_peer(const std::string& peer_address);
    bool remember_peer_address(const std::string& address,
                               uint16_t port,
                               const std::string& source_peer);
    void mark_peer_address_attempt(const std::string& address, uint16_t port, bool success);
    std::vector<std::pair<std::string, uint16_t>> get_local_advertised_addresses() const;
    std::vector<std::pair<std::string, uint16_t>> collect_advertisable_addresses(size_t max_count) const;
    bool send_addr_list_to_socket(int socket_fd,
                                  const std::vector<std::pair<std::string, uint16_t>>& addresses);
    
    // Message processing
    bool send_message(int socket_fd, const P2PMessage& message);
    std::unique_ptr<P2PMessage> receive_message(int socket_fd);
    void process_message(const std::string& peer_address, const P2PMessage& message);
    
    // Built-in message handlers
    void handle_version(const std::string& peer_address, const P2PMessage& message);
    void handle_verack(const std::string& peer_address, const P2PMessage& message);
    void handle_ping(const std::string& peer_address, const P2PMessage& message);
    void handle_pong(const std::string& peer_address, const P2PMessage& message);
    void handle_sendcmpct(const std::string& peer_address, const P2PMessage& message);
    void handle_addr(const std::string& peer_address, const P2PMessage& message);
    void handle_getaddr(const std::string& peer_address, const P2PMessage& message);
    
    // Socket utilities
    int create_listen_socket();
    int create_client_socket(const std::string& address, uint16_t port);
    int create_socks5_client_socket(const std::string& proxy_host,
                                    uint16_t proxy_port,
                                    const std::string& target_host,
                                    uint16_t target_port);
    void close_socket(int socket_fd);
    void set_socket_nonblocking(int socket_fd);
    void set_socket_send_timeout(int socket_fd, int seconds);
    std::shared_ptr<std::mutex> get_socket_send_mutex(int socket_fd);
    void erase_socket_send_mutex(int socket_fd);
    std::string get_peer_address(int socket_fd);
    
    // Utility functions
    uint32_t calculate_checksum(const std::vector<uint8_t>& data) const;
    std::vector<uint8_t> serialize_uint32(uint32_t value) const;
    std::vector<uint8_t> serialize_string(const std::string& str) const;
    uint32_t deserialize_uint32(const std::vector<uint8_t>& data, size_t& offset) const;
    std::string deserialize_string(const std::vector<uint8_t>& data, size_t& offset) const;
};
