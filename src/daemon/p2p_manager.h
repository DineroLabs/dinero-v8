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
#include <array>
#include <optional>

#include "p2p/addrman.h"
#include "p2p/addr_v2.h"  // NAT traversal Phase 1A.2: AddrV2Entry struct for create_addrv2()
#include "network/clock_source.h"     // relay-hints Phase 1a: injectable time source for TTL logic
#include "network/quic_session.h"     // NAT traversal Phase B2: encrypted relay virtual peers
#include "network/relay_registry.h"   // NAT traversal Phase C3 slice 2: relay-side directory
#include "network/token_bucket.h"     // NAT traversal: relay circuit bandwidth caps

namespace dinero { namespace daemon { class NodeIdentity; } }

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
    struct ViaRelayInfo {
        uint64_t circuit_id{0};
        std::string relay_peer_address;
        uint8_t outbound_direction{0};  // P2PMessage::RelayDirection wire value
        bool encrypted_quic{false};
    };

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

    // ─── NAT traversal Phase 1A: proven node identity ────────────────────
    // our_nonce: random 8-byte value we put in our outgoing version message.
    //   Used as the data the REMOTE peer signs in their `dineroid` message.
    // their_nonce: random 8-byte value the remote peer put in their version.
    //   Used as the data WE sign in our outgoing `dineroid`.
    // their_pubkey / their_node_id: populated only after dineroid is received
    //   AND its signature verifies against our_nonce.
    // identity_proven: gating flag — relay subsystem (later phases) MUST
    //   refuse to advertise a peer's reachability unless this is true.
    uint64_t our_nonce{0};
    uint64_t their_nonce{0};
    std::array<uint8_t, 33> their_pubkey{};
    std::array<uint8_t, 20> their_node_id{};
    bool identity_proven{false};

    // ─── NAT traversal Phase 1A.2: BIP155 addrv2 negotiation ─────────────
    // Set true after the remote peer sends `sendaddrv2` during handshake.
    // Outbound addr-relay paths will consult this flag (later commit) —
    // when true, send via create_addrv2() (typed); when false, fall back
    // to legacy create_addr() (string-form). We always advertise our own
    // support to peers that signal NODE_DINERO_V2.
    bool peer_wants_addrv2{false};

    // ─── NAT traversal Phase C3 slice 4a: outbound relay registration ────
    // is_our_relay: true when this peer is configured via
    //   `relayregister=...` AND dineroid+RELAY_REGISTER succeeded with
    //   them. We refresh registration every TTL/2 from keepalive_loop.
    // last_register_sent_at: steady-clock timestamp of the most recent
    //   RELAY_REGISTER we sent on this connection. Used to drive the
    //   refresh cadence; epoch sentinel means "never sent yet".
    bool is_our_relay{false};
    std::chrono::steady_clock::time_point last_register_sent_at{};

    // NAT traversal slice 4c: synthetic peer carried over a relay circuit
    // instead of a direct TCP socket. address is a stable virtual key:
    // relay:<target_node_id_hex>:<circuit_id_hex>.
    std::optional<ViaRelayInfo> via_relay;
    std::mutex relay_inbox_mutex;
    std::condition_variable relay_inbox_cv;
    std::deque<std::vector<uint8_t>> relay_inbox_frames;
    std::shared_ptr<dinero::network::QuicSession> relay_quic_session;
    std::optional<dinero::network::QuicSessionOptions> relay_quic_options;

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
          via_relay(std::move(other.via_relay)),
          relay_quic_session(std::move(other.relay_quic_session)),
          relay_quic_options(std::move(other.relay_quic_options)),
          lifetime_state(other.lifetime_state.load()) {}

    // Default constructor
    PeerInfo() = default;

    // Delete copy constructor (atomic is not copyable)
    PeerInfo(const PeerInfo&) = delete;
    PeerInfo& operator=(const PeerInfo&) = delete;
    PeerInfo& operator=(PeerInfo&&) = delete;

    std::string to_string() const {
        if (via_relay.has_value()) {
            return address;
        }
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
                                     const std::string& user_agent = "",
                                     uint64_t explicit_nonce = 0);
    static P2PMessage create_verack();

    // NAT traversal Phase 1A: post-verack node-identity exchange.
    // Builds payload {pubkey_33, sig_len_1, sig_DER}. `nonce_to_sign` is the
    // REMOTE peer's version nonce — signing it binds this dineroid to the
    // specific TCP handshake and prevents cross-connection replay.
    // Returns empty payload if the identity isn't initialized; caller MUST
    // handle that as a non-fatal handshake skip.
    static P2PMessage create_dineroid(const dinero::daemon::NodeIdentity& identity,
                                      uint64_t nonce_to_sign);

    // NAT traversal Phase 1A.2 / BIP155:
    //   sendaddrv2 has empty payload — its presence is the negotiation.
    //   addrv2 wraps a typed entry list via dinero::p2p::EncodeAddrV2.
    static P2PMessage create_sendaddrv2();
    static P2PMessage create_addrv2(const std::vector<dinero::p2p::AddrV2Entry>& entries);

    // ─── Circuit relay protocol (NAT Phase C3, slice 1: wire format) ────
    // Each helper builds the on-wire payload per the spec in
    // include/network/types.h's MessageCommands section. Slice 1 lands
    // the wire format only — handlers in process_message log + reject
    // until slice 2 adds the relay registry.

    // relay_register: identity = the SAME daemon-wide NodeIdentity used
    // by dineroid. nonce_to_sign is the their_nonce captured from this
    // connection's version handshake (so the signature can't be replayed
    // on a different connection). Returns empty payload on signing
    // failure; caller must check.
    static P2PMessage create_relay_register(
        const dinero::daemon::NodeIdentity& identity,
        uint64_t nonce_to_sign,
        uint32_t ttl_seconds);

    // relay_connect: request_id is opaque to the relay; the originator
    // uses it to match relay_connect_ack responses (and possibly
    // concurrent connect requests in flight to the same relay).
    static P2PMessage create_relay_connect(
        const std::array<uint8_t, 20>& target_node_id,
        uint64_t request_id);

    enum class RelayConnectStatus : uint8_t {
        Ok            = 0,
        NoSuchPeer    = 1,
        RelayFull     = 2,
        RateLimited   = 3,
        InternalError = 4,
    };
    static P2PMessage create_relay_connect_ack(
        uint64_t request_id,
        uint64_t circuit_id,
        RelayConnectStatus status,
        const std::string& message);

    enum class RelayDirection : uint8_t {
        ClientToTarget = 0,
        TargetToClient = 1,
    };
    static P2PMessage create_relay_data(
        uint64_t circuit_id,
        RelayDirection direction,
        const std::vector<uint8_t>& payload);

    static P2PMessage create_relay_ping(uint64_t circuit_id, uint64_t nonce);

    // relay_hints: target_node_id is usually our own node_id; relay_addr
    // is one of the relays we've registered with. Multiple entries per
    // message — speaker can advertise itself via 2-3 relays to give
    // dialers a choice if one relay is overloaded.
    struct RelayHint {
        std::array<uint8_t, 20> target_node_id{};
        dinero::p2p::NetworkType relay_net{dinero::p2p::NetworkType::IPV4};
        std::vector<uint8_t> relay_addr;
        uint16_t relay_port{0};
    };
    static P2PMessage create_relay_hints(const std::vector<RelayHint>& hints);

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

    // Test-only constructor: inject a custom ClockSource (e.g.,
    // FakeClockSource) for deterministic TTL tests. Existing default
    // ctor stays untouched — defaults clock_ to SystemClockSource.
    P2PManager(uint16_t listen_port,
               const std::string& external_ip,
               std::unique_ptr<dinero::network::ClockSource> clock);

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

    // NAT traversal Phase 1A: caller wires in the daemon-wide NodeIdentity
    // (initialized in DaemonApp from node_identity.dat). When set AND the
    // remote peer advertises NODE_DINERO_V2, perform_handshake will exchange
    // `dineroid` messages between version and verack. Safe to call with
    // nullptr: identity is treated as "not available", dineroid is skipped,
    // and the handshake completes legacy-style.
    void set_node_identity(std::shared_ptr<dinero::daemon::NodeIdentity> identity);

    // NAT traversal Phase C3 slice 4a: caller declares which peers we
    // want to register with as a relay. Strings are "host:port"
    // canonicalized to lowercase. After dineroid succeeds with one of
    // these peers, perform_handshake sends RELAY_REGISTER on our behalf;
    // keepalive_loop refreshes the registration every 3600s.
    void set_configured_relay_endpoints(std::vector<std::string> endpoints);

    // NAT traversal Phase D-1: kick off a RELAY_CONNECT toward a target
    // node_id by way of an existing TCP connection to `relay_peer_address`.
    //
    // Returns the request_id (a 64-bit token > 0) on success, 0 if the
    // relay isn't currently connected. The callback fires exactly once
    // when RELAY_CONNECT_ACK arrives (ok=true with valid circuit_id, or
    // ok=false with the relay's error string) OR when kRelayConnectTimeout
    // elapses with no ack (ok=false, msg="timed out").
    //
    // The completion callback owns any virtual-peer creation. On success
    // this API records the opened circuit_id, then reports it to the
    // orchestrator so the normal P2P handshake can run through the relay
    // transport helpers.
    uint64_t SendRelayConnect(
        const std::string& relay_peer_address,
        const std::array<uint8_t, 20>& target_node_id,
        std::function<void(bool ok, uint64_t circuit_id,
                           const std::string& msg)> callback);

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
    size_t relay_hints_received_self_count() const {
        return hints_received_self_.load(std::memory_order_acquire);
    }
    size_t relay_hints_received_relay_count() const {
        return hints_received_relay_.load(std::memory_order_acquire);
    }
    size_t relay_hints_evicted_expired_count() const {
        return hints_evicted_expired_.load(std::memory_order_acquire);
    }
    size_t relay_hints_evicted_failure_count() const {
        return hints_evicted_failure_.load(std::memory_order_acquire);
    }
    size_t relay_registry_entry_count() const {
        return relay_registry_.size();
    }
    size_t relay_registry_grace_pending_count() const {
        return relay_registry_.grace_pending_count();
    }

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

    // NAT traversal Phase 1A: drives the symmetric `dineroid` exchange in
    // the middle of perform_handshake. Sends our signed pubkey and waits
    // (with the same timeout receive_message uses) for the remote one;
    // populates peer->their_pubkey / their_node_id / identity_proven on
    // success. Failures are non-fatal — handshake continues legacy-style.
    void ExchangeDineroId(PeerInfo* peer);

    // NAT traversal Phase 1A.2: symmetric `sendaddrv2` negotiation. Sends
    // our (empty-payload) sendaddrv2; receives remote's. Sets
    // peer->peer_wants_addrv2 = true on receipt. Same gating as
    // ExchangeDineroId — both peers advertise NODE_DINERO_V2 — and
    // similarly non-fatal on failure (legacy `addr` continues to work).
    void ExchangeSendAddrV2(PeerInfo* peer);

    // BIP155 addrv2 ingestion. Mirrors handle_addr but decodes via
    // dinero::p2p::DecodeAddrV2 and currently feeds only IPV4 / IPV6
    // entries into addrman (TORV3 / I2P parsed-and-skipped until the
    // onion-string-roundtrip codec lands in a follow-up commit).
    void handle_addrv2(const std::string& peer_address, const P2PMessage& message);

    // NAT traversal Phase C3 slice 2: validates a RELAY_REGISTER
    // payload from `peer_address` and, on success, inserts/refreshes
    // the corresponding RelayRegistry entry. Validation requires the
    // peer's dineroid identity to already be proven — otherwise we
    // have no pubkey to verify the signature against.
    void handle_relay_register(const std::string& peer_address,
                               const P2PMessage& message);

    // NAT traversal Phase C3 slice 3: relay-side circuit handling.
    //
    // handle_relay_connect — external peer requests a circuit to a
    // NAT'd peer identified by node_id. Looks up the registration,
    // allocates a fresh circuit_id, returns a RELAY_CONNECT_ACK
    // (Ok / NoSuchPeer / RelayFull).
    //
    // handle_relay_data — opaque bytes flow either direction between
    // the two endpoints of a circuit. Relay looks up the circuit by
    // id, identifies which side sent (requester vs target), forwards
    // the same RELAY_DATA frame to the other side. Refreshes
    // last_data_at for idle-timeout tracking.
    //
    // handle_relay_ping — keepalive that just refreshes last_data_at.
    // No reply is generated; if the circuit died on the relay's path
    // to the other endpoint the next data send will surface that.
    void handle_relay_connect(const std::string& peer_address,
                              const P2PMessage& message);
    void handle_relay_data(const std::string& peer_address,
                           const P2PMessage& message);
    void handle_relay_ping(const std::string& peer_address,
                           const P2PMessage& message);

    // Periodic maintenance: drop circuits whose last_data_at is older
    // than kCircuitIdleTimeout. Called from keepalive_loop.
    void SweepIdleCircuits();

    // NAT traversal Phase C3 slice 4b: advertise + ingest RELAY_HINTS.
    //
    // SendRelayHintsIfApplicable is called from perform_handshake right
    // after verack on either side, when the local node has at least one
    // is_our_relay connection AND the peer we just handshook with is NOT
    // one of our relays. We build a RELAY_HINTS payload listing
    // (our_node_id, relay_endpoint) for each registered relay and send.
    //
    // handle_relay_hints replaces the slice-1 stub. It decodes the
    // payload and stuffs each entry into relay_hints_by_target_, keyed
    // by target_node_id_hex. The dialing orchestrator (slice D) reads
    // this side-table to find relays for a target it wants to reach.
    void SendRelayHintsIfApplicable(PeerInfo* peer, uint64_t our_services);
    void handle_relay_hints(const std::string& peer_address,
                            const P2PMessage& message);
    void AdvertiseRegisteredRelayTarget(
        const std::array<uint8_t, 20>& target_node_id,
        const std::string& registrant_peer_address);
    // Address fields (relay_net/relay_addr/relay_port) for every routable
    // relay endpoint this node can be reached at; target_node_id is left
    // unset for the caller to fill. Shared by AdvertiseRegisteredRelayTarget
    // and SendRelayRegistryToNewPeer.
    std::vector<P2PMessage::RelayHint> CollectLocalRelayEndpointHints();
    // On-connect catch-up: push relay_hints for every currently-registered
    // target to a freshly-connected NODE_DINERO_V2 peer, so an origin that
    // joins after a target registered still learns how to reach it.
    void SendRelayRegistryToNewPeer(PeerInfo* peer);

    // NAT traversal Phase D-1: match an incoming RELAY_CONNECT_ACK
    // against pending_connects_ and (on Ok) install the circuit_id into
    // originated_circuits_. Always fires the caller's completion callback
    // exactly once.
    void handle_relay_connect_ack(const std::string& peer_address,
                                  const P2PMessage& message);

    // NAT traversal Phase D-1: timeout sweeper called from keepalive_loop
    // (existing 30s cadence). Walks pending_connects_, fires the
    // completion callback with ok=false for any entry older than
    // kRelayConnectTimeout, then removes it.
    void SweepRelayConnectTimeouts();

    // NAT traversal Phase D-2: relay-aware outbound dialing orchestrator.
    //
    // Called from connection_manager_loop after the seed/addrman direct-
    // dial pass. Walks relay_hints_by_target_ looking for target_node_ids
    // we don't currently have a peer for (direct or virtual) AND for
    // which we have at least one usable relay hint (relay is in
    // connected_peers_). For each eligible target, picks the freshest
    // hint and calls SendRelayConnect; the completion callback installs
    // an outbound virtual PeerInfo and spawns its handler thread (which
    // perform_handshakes through the existing send/receive_peer_message
    // path — virtual-peer routing is transparent there per slice 4c).
    //
    // Bounded by MAX_OUTBOUND_CONNECTIONS (counts direct + virtual).
    // Per-target backoff via last_relay_dial_attempt_ prevents thrashing
    // on dead hints; the SendRelayConnect 10s timeout from D-1 surfaces
    // failure cleanly.
    void OrchestrateRelayDials();

    // Production analog of test_install_virtual_relay_peer for the
    // outbound (originator) side of a circuit. Mirrors the inbound
    // synthesis pattern used by handle_relay_data when a previously-
    // unseen circuit_id arrives, but with is_outbound=true and
    // outbound_direction=ClientToTarget. Inserts into connected_peers_
    // keyed on the synthetic "relay:<node_id_hex>:<circuit_id_hex>" key
    // and returns that key (caller passes it to start_peer_handler_thread).
    std::string install_outbound_virtual_relay_peer(
        const std::array<uint8_t, 20>& target_node_id,
        const std::string& relay_peer_address,
        uint64_t circuit_id);

    // NAT traversal slice 4c: create/lookup the synthetic peer address
    // backing an opened relay circuit. The returned key can be passed to
    // send_to_peer(), which wraps the original P2P frame as RELAY_DATA.
    std::string RelayVirtualPeerAddress(const std::array<uint8_t, 20>& target_node_id,
                                        uint64_t circuit_id) const;

    // NAT traversal Phase C3 slice 4a: client-side outbound registration.
    // SendRelayRegisterIfConfigured is called from perform_handshake on
    // the outbound side after dineroid succeeds; checks whether `peer`
    // matches one of configured_relay_endpoints_ and, if so, sends a
    // RELAY_REGISTER over the existing connection. RefreshRelayRegistrations
    // is called from keepalive_loop on its 30s tick; walks connected
    // peers, re-sends RELAY_REGISTER on any is_our_relay peer whose
    // last_register_sent_at is older than kRelayRegisterRefreshInterval.
    void SendRelayRegisterIfConfigured(PeerInfo* peer);
    void RefreshRelayRegistrations();

#ifdef DINERO_TEST_BUILD
    void set_plaintext_relay_dev_override_for_tests(bool allowed);
    void set_encrypted_relay_dev_override_for_tests(bool allowed);
    bool test_plaintext_relay_transport_allowed() const;
    void test_install_connected_direct_peer(
        const std::string& peer_address,
        int socket_fd,
        bool is_outbound,
        bool identity_proven,
        const std::array<uint8_t, 20>& node_id);
    void test_insert_pending_relay_connect(
        uint64_t request_id,
        const std::array<uint8_t, 20>& target_node_id,
        const std::string& relay_peer_address,
        std::function<void(bool ok, uint64_t circuit_id,
                           const std::string& msg)> callback);
    size_t test_pending_relay_connect_count() const;
    size_t test_originated_circuit_count() const;
    std::string test_install_virtual_relay_peer(
        const std::string& virtual_peer_key,
        const std::string& relay_peer_address,
        uint64_t circuit_id,
        P2PMessage::RelayDirection outbound_direction,
        bool is_outbound);
    bool test_enqueue_relay_frame(const std::string& virtual_peer_key,
                                  const std::vector<uint8_t>& frame);
    std::unique_ptr<P2PMessage> test_receive_peer_message(
        const std::string& peer_key,
        std::chrono::milliseconds timeout);
#endif

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
    std::mutex peer_threads_mutex_;
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

    // NAT traversal Phase 1A: daemon-wide node identity. Read-only after
    // start(); set_node_identity() is expected during init. Held by
    // shared_ptr because the same instance is also consumed by other
    // services (serverinfo signer, future relay register).
    std::shared_ptr<dinero::daemon::NodeIdentity> node_identity_;

    // QUIC relay TLS material. Populated once at start() with an ephemeral
    // self-signed cert+key generated in-process via
    // dinero::network::GenerateRelayTlsKeypair. The encryption layer is
    // opportunistic (verify_peer=false): we do not authenticate the relay or
    // the peer at the TLS layer — the existing dineroid identity exchange
    // running inside the encrypted stream is the trust anchor. This struct
    // is reused for every relay circuit (client side and server side both).
    // Empty `certificate_pem` indicates init failed and encrypted relay
    // should refuse to engage; install paths check via_relay->encrypted_quic
    // to gate.
    dinero::network::QuicSessionOptions relay_tls_options_;
    bool relay_tls_ready_{false};

    // NAT traversal Phase C3 slice 2: in-memory directory of NAT'd
    // peers that have registered with us as their relay. Empty until
    // a RELAY_REGISTER arrives. handle_relay_connect (slice 3) looks
    // up the registration to find the target peer's TCP connection.
    dinero::network::RelayRegistry relay_registry_;

    // NAT traversal Phase C3 slice 4a: configured-relay endpoints.
    // Lower-cased "host:port" strings. Lookup is O(N) per handshake,
    // but N is small (typically 1-3 relays per node).
    std::vector<std::string> configured_relay_endpoints_;
    // configured_relay_endpoints_ is set from config once, but also
    // rewritten periodically by MaybeAutoRegisterWithRelays on the
    // keepalive thread, so it is mutex-guarded. relay_endpoints_from_config_
    // records whether an operator pinned them (relayregister=) — when true,
    // auto-register leaves the list alone.
    mutable std::mutex relay_endpoints_mutex_;
    std::atomic<bool> relay_endpoints_from_config_{false};
    static constexpr uint32_t kRelayRegisterTtlSeconds = 7200;       // 2h
    static constexpr std::chrono::seconds kRelayRegisterRefreshInterval{3600};  // 1h
    // NAT traversal: NAT-gated relay auto-registration. When this node has
    // no confirmed inbound path it keeps up to kAutoRelayTargetCount relay
    // registrations alive, discovered via addrman NODE_RELAY peers with a
    // hardcoded bootstrap fallback. Runs on the keepalive tick.
    static constexpr size_t kAutoRelayTargetCount = 3;
    void MaybeAutoRegisterWithRelays();

    // NAT traversal Phase C3 slice 3: relay-side circuit table.
    // Keyed on circuit_id (allocated by handle_relay_connect from a
    // hardware-random 8 bytes). Forwarders mutate last_data_at on
    // every RELAY_DATA / RELAY_PING so SweepIdleCircuits can reap
    // stalled circuits without disrupting active ones.
    // Relay bandwidth caps (NAT plan). Over any cap the RELAY_DATA frame
    // is dropped — the inner QUIC retransmits and backs off, so a drop
    // paces the sender rather than corrupting the stream.
    static constexpr double kRelayPerCircuitRateBps = 64.0 * 1024.0;      // 64 KB/s steady
    static constexpr double kRelayPerCircuitBurstBytes = 256.0 * 1024.0;  // 256 KB burst
    struct CircuitInfo {
        std::string requester_addr;   // external peer (originator)
        std::string target_addr;      // registered NAT'd peer
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_data_at;
        // Per-circuit token bucket; lives and dies with the circuit.
        dinero::network::TokenBucket circuit_bucket{kRelayPerCircuitRateBps,
                                                    kRelayPerCircuitBurstBytes};
    };
    mutable std::mutex circuits_mutex_;
    std::unordered_map<uint64_t, CircuitInfo> circuits_;
    static constexpr size_t kMaxConcurrentCircuits = 25;
    static constexpr std::chrono::seconds kCircuitIdleTimeout{300};  // 5 min

    // Relay bandwidth caps — global egress token bucket, fixed-window
    // daily quota, and the chain-behind auto-suspend gate. The quota is
    // a fixed 24h window (resets on the boundary); worst case is ~2x the
    // cap across an unlucky boundary — acceptable vs uncapped, upgrade to
    // an hourly-ring rolling window only if operational data shows it
    // matters. relay_global_bucket_ / relay_quota_* are touched only
    // under circuits_mutex_; the atomics need no lock.
    static constexpr double kRelayGlobalRateBps = 5.0 * 1024.0 * 1024.0;     // 5 MB/s
    static constexpr double kRelayGlobalBurstBytes = 5.0 * 1024.0 * 1024.0;  // 1s burst
    static constexpr uint64_t kRelayDailyQuotaBytes =
        50ULL * 1024 * 1024 * 1024;  // 50 GB / 24h
    static constexpr uint32_t kRelayMaxBlocksBehind = 100;
    dinero::network::TokenBucket relay_global_bucket_{kRelayGlobalRateBps,
                                                      kRelayGlobalBurstBytes};
    uint64_t relay_quota_bytes_ = 0;
    std::chrono::steady_clock::time_point relay_quota_window_start_{};
    std::atomic<bool> relay_behind_throttle_{false};
    std::atomic<uint64_t> relay_drops_circuit_{0};
    std::atomic<uint64_t> relay_drops_global_{0};
    std::atomic<uint64_t> relay_drops_quota_{0};
    std::atomic<uint64_t> relay_drops_behind_{0};
    // Fixed-window quota check; caller holds circuits_mutex_.
    bool RelayQuotaAllows(size_t bytes,
                          std::chrono::steady_clock::time_point now);
    // Recompute the cached >kRelayMaxBlocksBehind throttle (keepalive tick).
    void RecomputeRelayBehindThrottle();

    // NAT traversal Phase D-1: originator-side circuit state.
    //
    // pending_connects_ holds in-flight RELAY_CONNECT requests keyed
    // by request_id. When the RELAY_CONNECT_ACK arrives, we look up
    // the request_id, fire the caller's completion callback, and (on
    // success) move the entry into originated_circuits_ keyed by the
    // newly-assigned circuit_id.
    //
    // originated_circuits_ holds circuits we successfully opened.
    // D-2/virtual-peer routing consumes this table to unwrap inbound
    // RELAY_DATA for circuits we opened. Outbound wrapping happens through
    // the synthetic peer ('relay:<node_id_hex>:<circuit_id_hex>') and the
    // normal send_to_peer() path.
    struct PendingConnect {
        std::array<uint8_t, 20> target_node_id{};
        std::string relay_peer_address;
        std::chrono::steady_clock::time_point sent_at;
        std::function<void(bool ok, uint64_t circuit_id,
                           const std::string& msg)> callback;
    };
    struct OriginatedCircuit {
        std::array<uint8_t, 20> target_node_id{};
        std::string relay_peer_address;
        std::chrono::steady_clock::time_point opened_at;
    };
    mutable std::mutex originator_mutex_;
    std::unordered_map<uint64_t /*request_id*/, PendingConnect> pending_connects_;
    std::unordered_map<uint64_t /*circuit_id*/, OriginatedCircuit> originated_circuits_;
    static constexpr std::chrono::seconds kRelayConnectTimeout{10};

    // NAT traversal Phase C3 slice 4b: ingested relay-hints side-table.
    // Keyed on target_node_id_hex (40 chars). Value is a list of relay
    // endpoints we've learned about from peers' RELAY_HINTS messages.
    // The dialing orchestrator (slice D) consults this when it wants
    // to reach a peer whose IP-keyed direct address is unknown but
    // whose node_id has at least one relay advertised.
    //
    // Entries age out by stale-replacement: each new RELAY_HINTS from
    // any peer carrying a given target_node_id replaces the prior
    // entry. No explicit TTL today — slice 4b+ will add one.
    struct RelayHintRecord {
        dinero::p2p::NetworkType net{dinero::p2p::NetworkType::IPV4};
        std::vector<uint8_t> relay_addr;  // raw bytes per network type
        uint16_t relay_port{0};
        std::chrono::steady_clock::time_point learned_at;
        // Phase 1a: per-hint failure counter for eviction.
        // Incremented when a dial via this hint fails (RELAY_CONNECT error
        // OR QUIC handshake timeout on the resulting circuit). Reset to 0
        // on any successful handshake or on receipt of a fresh duplicate
        // hint. Drop when >= kHintMaxFailures.
        int consecutive_dial_failures{0};
    };
    // Time source for TTL/expiry logic in the hints subsystem.
    // Defaults to SystemClockSource; tests inject a FakeClockSource.
    std::unique_ptr<dinero::network::ClockSource> clock_;

    mutable std::mutex relay_hints_mutex_;
    std::unordered_map<std::string, std::vector<RelayHintRecord>> relay_hints_by_target_;
    static constexpr size_t kMaxHintsPerTarget = 4;

    // Phase 1a observability — incremented from sweep + counter paths.
    std::atomic<size_t> hints_evicted_expired_{0};
    std::atomic<size_t> hints_evicted_failure_{0};
    std::atomic<size_t> hints_received_self_{0};
    std::atomic<size_t> hints_received_relay_{0};

    // NAT traversal Phase D-2: per-target dial backoff. When the
    // orchestrator decides to attempt a relay-dial for a target, we record
    // the timestamp here; subsequent orchestrator iterations skip the same
    // target for kRelayDialBackoff seconds whether the prior attempt
    // succeeded (we'll have a connected virtual peer; check that first
    // anyway), timed out, or got an explicit rejection. Avoids thrashing
    // when a relay has stale hints.
    static constexpr std::chrono::seconds kRelayDialBackoff{60};
    static constexpr std::chrono::minutes kHintTtl{15};
    static constexpr int kHintMaxFailures{3};
    static constexpr std::chrono::minutes kHintResendPeriod{5};
    static constexpr std::chrono::seconds kRelayDirectoryGracePeriod{90};
    std::unordered_map<std::string /*target_node_id_hex*/,
                       std::chrono::steady_clock::time_point>
        last_relay_dial_attempt_;

#ifdef DINERO_TEST_BUILD
    std::atomic<bool> plaintext_relay_dev_override_for_tests_{false};
    std::atomic<bool> encrypted_relay_dev_override_for_tests_{false};
#endif

    // Network threads
    void listen_loop();
    void connection_manager_loop();
    // Ring 3 Phase 4c: Changed to shared_ptr for TS1 compliance
    void start_peer_handler_thread(std::shared_ptr<PeerInfo> peer);
    void peer_handler_loop(std::shared_ptr<PeerInfo> peer);
    // Task 5: Decrypted-stream reader for QUIC relay virtual peers.
    // Spawned by start_peer_handler_thread (Task 6) for encrypted circuits.
    void run_relay_quic_reader_loop(std::shared_ptr<PeerInfo> peer);
    void outbox_loop();
    void keepalive_loop();  // Phase C: Adaptive keepalive thread
    // Phase 1a: evict stale relay-hint records from relay_hints_by_target_.
    // Called from keepalive_loop on its existing 30s cadence; no new thread.
    // Acquires relay_hints_mutex_. Logs eviction reason per entry.
    void SweepRelayHintsCache();

    // Phase 1a: re-send our own RELAY_HINTS(target=self) to every
    // NODE_DINERO_V2 peer that isn't one of our configured relayregister=
    // endpoints. Called from keepalive_loop; gated by kHintResendPeriod
    // so the effective cadence is 5min ± 30s.
    void MaybeReSendRelayHints();

    std::chrono::steady_clock::time_point last_relay_hints_resend_{};
    
    // Connection management
    void handle_incoming_connection(int client_socket, const std::string& client_address);
    bool perform_handshake(PeerInfo* peer);
    void cleanup_peer(const std::string& peer_address);
    bool remember_peer_address(const std::string& address,
                               uint16_t port,
                               const std::string& source_peer,
                               uint64_t services = 0);
    void mark_peer_address_attempt(const std::string& address, uint16_t port, bool success);
    std::vector<std::pair<std::string, uint16_t>> get_local_advertised_addresses() const;
    std::vector<std::pair<std::string, uint16_t>> collect_advertisable_addresses(size_t max_count) const;
    bool send_addr_list_to_peer(PeerInfo* peer,
                                const std::vector<std::pair<std::string, uint16_t>>& addresses);
    
    // Message processing
    bool send_message(int socket_fd, const P2PMessage& message);
    std::unique_ptr<P2PMessage> receive_message(int socket_fd);
    bool send_peer_message(PeerInfo* peer, const P2PMessage& message);
    std::unique_ptr<P2PMessage> receive_peer_message(
        PeerInfo* peer,
        std::chrono::milliseconds timeout = std::chrono::seconds(10));
    bool enqueue_relay_frame(const std::string& virtual_peer_key,
                             const std::vector<uint8_t>& frame);
    bool plaintext_relay_transport_allowed() const;
    bool encrypted_relay_transport_allowed() const;
    void process_message(const std::string& peer_address, const P2PMessage& message);
    bool send_relay_data_to_virtual_peer(PeerInfo& peer, const P2PMessage& message);
    bool send_relay_payload_to_virtual_peer(PeerInfo& peer,
                                            const std::vector<uint8_t>& payload);
    bool unwrap_relay_quic_packet(const std::string& virtual_peer_key,
                                  PeerInfo& peer,
                                  const std::vector<uint8_t>& packet);
    bool unwrap_relay_data_endpoint(const std::string& relay_peer_address,
                                    const P2PMessage& message);
    
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
