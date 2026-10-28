#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <chrono>
#include <array>
#include <optional>

#include "p2p/addrman.h"
#include "p2p/addr_v2.h"  // NAT traversal Phase 1A.2: AddrV2Entry struct for create_addrv2()
#include "p2p/hole_punch.h"
#include "p2p/outbound_policy.h"
#include "network/clock_source.h"     // relay-hints Phase 1a: injectable time source for TTL logic
#include "network/rolling_24h_counter.h"  // Phase 2b: rolling 24h byte counter for relay-virtual traffic
#include "network/endpoint_format.h"   // [v6]:port peer keys
#include "p2p/relay_hint_v2.h"         // relayhn2: target-signed, forwardable relay hints
#include "p2p/self_reachability.h"      // dial-back verified self addresses
#include "network/quic_session.h"     // NAT traversal Phase B2: encrypted relay virtual peers
#include "network/relay_registry.h"   // NAT traversal Phase C3 slice 2: relay-side directory
#include "network/token_bucket.h"     // NAT traversal: relay circuit bandwidth caps
#include "network/version_nonce_tracker.h"

namespace dinero { namespace daemon { class NodeIdentity; } }
namespace dinero { namespace network { class QuicEndpoint; } }

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
    // Short-lived outbound probe used to validate a NEW AddrMan entry.
    // A feeler completes the normal handshake, marks the address good, and
    // disconnects before entering the durable peer/message lifecycle.
    bool is_feeler{false};
    bool is_connected;
    int socket_fd;

    // issue #241/#214: consecutive send_message() failures on this peer's
    // socket (see daemon/peer_send_health.h). Reaching the threshold evicts
    // the peer from send_to_peer — a socket that stops draining must not
    // convoy senders on its send mutex forever.
    std::atomic<uint32_t> consecutive_send_failures{0};

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
    // Gap 1 (peer-reported external-IP learning): the address THIS peer reported
    // as OURS in their version message (Bitcoin addrRecv). Empty when the peer
    // is an older node that zero-fills addrRecv. Fed into self-address voting so
    // a reachable node can auto-discover + advertise its own external IP.
    std::string reported_local_addr;
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
    // A RELAY_REGISTER is out and its relayrok not yet back. is_our_relay
    // becomes true only on the ack (an old build or a relay with the role
    // off never acks, and must not be counted).
    bool relay_register_pending{false};
    std::chrono::steady_clock::time_point relay_register_pending_since{};
    // Liveness of a confirmed relay: one outstanding circuit-0 relay ping.
    uint64_t relay_ping_nonce{0};
    uint32_t relay_pings_missed{0};
    std::chrono::steady_clock::time_point last_relay_ping_at{};

    // NAT traversal slice 4c: synthetic peer carried over a relay circuit
    // instead of a direct TCP socket. address is a stable virtual key:
    // relay:<target_node_id_hex>:<circuit_id_hex>.
    std::optional<ViaRelayInfo> via_relay;
    std::mutex relay_inbox_mutex;
    std::condition_variable relay_inbox_cv;
    std::deque<std::vector<uint8_t>> relay_inbox_frames;
    std::shared_ptr<dinero::network::QuicSession> relay_quic_session;
    std::optional<dinero::network::QuicSessionOptions> relay_quic_options;
    // Direct QUIC peer: no relay and no TCP socket. Its session (in
    // relay_quic_session) carries the same framed stream as an encrypted relay
    // circuit, so both share the inbox, reader loop and send path below.
    bool quic_direct{false};
    dinero::network::UdpAddr quic_remote{};
    // Hole-punched direct QUIC peer (Plan 4): dialed from a relayed link, so
    // like a relayed peer it draws on the home-peer budget rather than the
    // direct outbound slots. punch_expected_id is the node id the dial must
    // authenticate as.
    bool punched{false};
    std::optional<std::array<uint8_t, 20>> punch_expected_id;
    // Outbound relayed and punched peers share kHomePeerSlots instead of
    // MAX_OUTBOUND_CONNECTIONS.
    bool is_home_peer() const { return via_relay.has_value() || punched; }
    bool uses_quic_stream() const {
        return quic_direct || (via_relay && via_relay->encrypted_quic);
    }
    // getpeerinfo "transport".
    const char* transport_name() const {
        return quic_direct ? "quic" : (via_relay ? "relay" : "tcp");
    }
    // getpeerinfo "path": how this peer is reached.
    const char* path_name() const { return punched ? "punched" : (via_relay ? "relayed" : "direct"); }

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
          is_feeler(other.is_feeler),
          is_connected(other.is_connected),
          socket_fd(other.socket_fd),
          consecutive_send_failures(other.consecutive_send_failures.load()),
          last_seen_unix(other.last_seen_unix),
          avg_latency_ms(other.avg_latency_ms),
          last_getaddr_sent(other.last_getaddr_sent),
          their_pubkey(other.their_pubkey),
          their_node_id(other.their_node_id),
          identity_proven(other.identity_proven),
          is_our_relay(other.is_our_relay),
          via_relay(std::move(other.via_relay)),
          relay_quic_session(std::move(other.relay_quic_session)),
          relay_quic_options(std::move(other.relay_quic_options)),
          quic_direct(other.quic_direct),
          quic_remote(other.quic_remote),
          punched(other.punched),
          punch_expected_id(other.punch_expected_id),
          lifetime_state(other.lifetime_state.load()),
          compact_timing_capable(other.compact_timing_capable.load()),
          release_handshake_complete(other.release_handshake_complete.load()) {}

    // Published by the handshake; sweeps never inspect partially parsed fields.
    std::atomic<bool> compact_timing_capable{false};
    std::atomic<bool> release_handshake_complete{false};

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
        return dinero::network::FormatEndpoint(address, port);
    }
};

struct P2PMessage {
    std::string command;
    std::vector<uint8_t> payload;
    uint32_t checksum;
    
    // Standard Bitcoin-like message types
    // addr_recv_ip/port: the address of the peer we're sending this version TO
    // (IPv4, or IPv6 in v6 form), written into addrRecv so the recipient can
    // learn its own external address (Bitcoin addrRecv semantics). Empty =>
    // 0.0.0.0 (legacy behavior). addr_from_port: our listen port.
    static P2PMessage create_version(uint32_t protocol_version, uint32_t best_height,
                                     uint64_t services = 0,
                                     const std::string& user_agent = "",
                                     uint64_t explicit_nonce = 0,
                                     const std::string& addr_recv_ip = "",
                                     uint16_t addr_recv_port = 20999,
                                     uint16_t addr_from_port = 20999);
    static P2PMessage create_verack();

    // NAT traversal Phase 1A: post-verack node-identity exchange.
    // Builds payload {pubkey_33, sig_len_1, sig_DER}. `nonce_to_sign` is the
    // REMOTE peer's version nonce — signing it binds this dineroid to the
    // specific TCP handshake and prevents cross-connection replay.
    // Returns empty payload if the identity isn't initialized; caller MUST
    // handle that as a non-fatal handshake skip.
    // With `binding` (a QUIC session's ExportBinding) it builds dineroid v2:
    // the signature covers nonce_LE8 || binding32 and the payload ends with
    // version byte 0x02.
    static P2PMessage create_dineroid(const dinero::daemon::NodeIdentity& identity,
                                      uint64_t nonce_to_sign,
                                      const std::array<uint8_t, 32>* binding = nullptr);
    static constexpr uint8_t kDineroIdV2 = 0x02;

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
    // Registration pong: circuit 0 | nonce | kRelayPongFlag.
    static P2PMessage create_relay_pong(uint64_t nonce);
    static constexpr uint8_t kRelayPongFlag = 0x01;
    // relayrok: a relay accepted our RELAY_REGISTER. Payload: ttl_u32 LE.
    static P2PMessage create_relay_register_ok(uint32_t ttl_seconds);

    // relay_hints: target_node_id is usually our own node_id; relay_addr
    // is one of the relays we've registered with. Multiple entries per
    // message — speaker can advertise itself via several relays to give
    // dialers a choice if one relay is overloaded.
    struct RelayHint {
        std::array<uint8_t, 20> target_node_id{};
        dinero::p2p::NetworkType relay_net{dinero::p2p::NetworkType::IPV4};
        std::vector<uint8_t> relay_addr;
        uint16_t relay_port{0};
    };
    static P2PMessage create_relay_hints(const std::vector<RelayHint>& hints);

    // Relay-coordinated hole punching (p2p/hole_punch.h for the payload).
    static P2PMessage create_punch_offer(uint64_t nonce, const dinero::p2p::PunchCandidates& c);
    static P2PMessage create_punch_answer(uint64_t nonce, const dinero::p2p::PunchCandidates& c);
    static P2PMessage create_punch_go(uint64_t nonce);

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

// Gap 2 (Stage A / A.1): a node is DIRECTLY reachable iff peers have been
// observed connecting inbound, an EXPLICIT/trusted address is advertised
// (operator externalip or a successful UPnP/NAT-PMP mapping), or a port
// mapping is currently active. A Gap-1-LEARNED address (the NAT gateway's
// public IP — seen by peers, but not necessarily dialable) does NOT count.
// This single predicate gates BOTH the getnetworkinfo relay-fallback report
// AND the MaybeAutoRegisterWithRelays behavioral gate so the two can never
// diverge — the divergence that left Stage A reporting-complete but
// behavior-incomplete (the RPC said relay_fallback_eligible=true while the
// auto-register gate still suppressed registration on a learned address).
//
// Since dial-back: reachable means another node proved it (an address of ours
// verified by dial-back, which covers a port mapping only once its address
// verified) or the operator asserted it (externalip). An inbound connection
// alone is not proof -- it may be a relay, a LAN peer, or a mapping that only
// works from inside a double NAT.
inline bool IsDirectlyReachable(bool proven_reachable) {
    return proven_reachable;
}

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
    using PeerHeightUpdatedHandler = std::function<void(const std::string& peer_address, uint32_t height)>;
    using HeightProvider = std::function<uint32_t()>;  // P2P sync fix: Callback to get current chain height
    using ServiceFlagsProvider = std::function<uint64_t()>;  // Returns advertised service flags

    P2PManager(uint16_t listen_port = 20999, const std::string& external_ip = "");

    // Idle-peer reaping policy, evaluated by keepalive_loop() on its 30s
    // cadence. Pure and public so the 90-second boundary is pinned by a
    // fake-clock regression test (tests/p2p/test_p2p_idle_peer_reap.cpp)
    // instead of being re-derived by every reader of the loop.
    //
    // PINGs go to ALL connected peers every 30s and last_message_at
    // refreshes on ANY received message (PONGs included), so a live peer
    // cannot trip the threshold: reaping requires 3 consecutive missed
    // PING/PONG round-trips. The boundary is exclusive — a peer idle for
    // EXACTLY kPeerIdleTimeout is retained; reaping starts strictly after.
    //
    // Silent direct OUTBOUND peers are deliberately exempt under current
    // policy (we chose them; churning them recreates the same connection),
    // but that exemption is policy, not accident — revisit alongside the
    // outbound liveness work rather than silently widening this reap.
    enum class IdlePeerAction {
        kKeep,               // healthy, not yet past the threshold, or exempt
        kReapRelayZombie,    // relay-virtual peer idle past the threshold
        kReapDirectInbound,  // direct inbound peer idle past the threshold
    };
    static constexpr std::chrono::seconds kPeerIdleTimeout{90};
    static IdlePeerAction classify_idle_peer(
        bool is_connected,
        bool is_relay_virtual,
        bool is_outbound,
        std::chrono::system_clock::time_point last_message_at,
        std::chrono::system_clock::time_point now,
        std::chrono::seconds idle_timeout = kPeerIdleTimeout);

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
    // Quiesce outbound work before a service performs potentially blocking
    // teardown. Socket close, peer joins, and final persistence remain in stop().
    void begin_shutdown();
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
    void add_anchor_node(const std::string& address, uint16_t port);
    bool remove_seed_node(const std::string& address, uint16_t port);
    std::vector<std::pair<std::string, uint16_t>> get_seed_nodes() const;
    std::vector<std::pair<std::string, uint16_t>> get_anchor_nodes() const;
    bool connect_to_peer(const std::string& address, uint16_t port);
    void disconnect_peer(const std::string& peer_address);
    bool ban_peer(const std::string& target, std::chrono::seconds duration);
    bool unban_peer(const std::string& target);
    void clear_banned_peers();
    std::vector<BanEntry> list_banned_peers() const;
    bool is_peer_banned(const std::string& address, uint16_t port = 0) const;
    void set_network_active(bool active);
    bool is_network_active() const { return network_active_.load(std::memory_order_acquire); }
    // Defaults are the owner's caps for a home relay: 8 circuits, 200 KB/s,
    // 20 GB per calendar month.
    struct RelayServiceLimits {
        size_t max_circuits{8};
        uint64_t bandwidth_bytes_per_second{200ULL * 1024};
        size_t max_circuits_per_peer{2};
        uint32_t circuit_lifetime_seconds{1800};
        uint32_t requests_per_peer_per_minute{6};
        uint64_t monthly_quota_bytes{20ULL * 1024 * 1024 * 1024};
    };
    // Turning the role off also drops every registration we hold.
    void set_relay_service_enabled(bool enabled);
    // Enabled, not syncing, and quota left: what acks and pongs require.
    bool relay_serving() const;
    void set_relay_service_limits(const RelayServiceLimits& limits);
    // <datadir>/relay_quota: this calendar month's (UTC) relayed bytes, so a
    // restart does not reset the monthly cap. Loads it now; written at most
    // once a minute and at shutdown.
    void set_relay_quota_path(const std::string& path);
    uint64_t relay_quota_month_bytes() const;
    uint64_t relay_quota_month_limit() const;
    // Why new circuits are refused right now: "" (not paused), "syncing"
    // (still at genesis, or more than 6 blocks behind the best validated
    // header) or "quota".
    std::string relay_paused_reason() const;
    void set_best_header_height_provider(HeightProvider provider) {
        best_header_height_provider_ = std::move(provider);
    }
    void set_address_manager(dinero::p2p::AddressManager* address_manager);

    // NAT traversal Phase 1A: caller wires in the daemon-wide NodeIdentity
    // (initialized in DaemonApp from node_identity.dat). When set AND the
    // remote peer advertises NODE_DINERO_V2, perform_handshake will exchange
    // `dineroid` messages between version and verack. Safe to call with
    // nullptr: identity is treated as "not available", dineroid is skipped,
    // and the handshake completes legacy-style.
    void set_node_identity(std::shared_ptr<dinero::daemon::NodeIdentity> identity);

    // Returns the local node's identity as a 40-character lowercase hex
    // string (20 bytes encoded). Empty string if node_identity_ has not
    // yet been set. Safe to call before set_node_identity().
    std::string get_local_node_id_hex() const;

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
    // learned=true marks a peer-vote-discovered address (Gap 1) that is NOT
    // proof of direct inbound reachability; learned=false (default) is an
    // explicit/trusted source (operator externalip, UPnP/NAT-PMP) and sets the
    // has_explicit_advertised_ flag used by the relay-fallback gate.
    // Explicit addresses (learned=false) are dial-back checked and reported
    // too; `source` says where they came from. Operator externalip addresses
    // are always gossiped and count as reachable; port-mapped ones only once
    // verified (a mapping on an inner router of a double NAT dials nowhere).
    void add_advertised_address(const std::string& address, uint16_t port, bool learned = false,
                                dinero::p2p::CandidateSource source =
                                    dinero::p2p::CandidateSource::ExternalIp);
    void remove_explicit_advertised_address(const std::string& address, uint16_t port);
    std::vector<std::pair<std::string, uint16_t>> get_advertised_addresses() const;
    // Dial-back verification of our own addresses. min_groups is 2 by default;
    // 1 is only for single-host (regtest) setups.
    void set_dialback_min_groups(uint32_t min_groups);
    // Regtest/tests only: answer reachchk from loopback/private requesters.
    // Off, they get result 4 without a dial (every Tor inbound peer is
    // 127.0.0.1, and a verifier must not probe its own localhost).
    void set_dialback_private_targets(bool allow) { dialback_private_targets_.store(allow); }
    std::vector<dinero::p2p::CandidateStatus> self_reachability_status() const;
    // Directly reachable as far as we can prove: an explicit address (operator
    // externalip / port mapping) or one that peers dialed back successfully.
    bool has_proven_reachable() const;
    // Called (outside every P2PManager lock) when has_proven_reachable()
    // flips. Verification also lapses by time, so the reachability tick
    // re-checks too.
    void set_reachability_changed_handler(std::function<void(bool)> handler);
    // Relays that confirmed our registration (relayrok), as host:port.
    std::vector<std::string> confirmed_relay_endpoints() const;
    // This round's STUN reflexive addresses (ip, port) on the P2P UDP port,
    // fed in as QUIC candidates. A different set of IPs from the last round
    // means our public address moved: every candidate goes back to unverified.
    void update_stun_candidates(const std::vector<std::pair<std::string, uint16_t>>& reflexive);
    // The last NAT mapping classification (P2PService sets it after each STUN round).
    void set_nat_mapping(dinero::p2p::NatMappingType mapping);
    // Addresses a remote peer can try for a hole punch: this host's global-unicast
    // IPv6 interface addresses on the QUIC port, plus the first routable STUN
    // reflexive IPv4. Empty when the QUIC endpoint is not listening.
    dinero::p2p::PunchCandidates LocalPunchCandidates() const;
    // Relay-coordinated hole punching: what the punch exchange has done so far.
    struct PunchStats {
        uint64_t attempts{0};          // Started, either role
        uint64_t offers_sent{0};
        uint64_t answers_sent{0};
        uint64_t gos_sent{0};
        uint64_t dials{0};             // punched QUIC dials started
        uint64_t probes_sent{0};
        size_t max_probe_targets{0};   // most distinct targets one attempt probed
        uint64_t succeeded{0};
        uint64_t timeout{0};
        uint64_t mismatch{0};          // a punched session proved another node id
        uint64_t refused{0};           // a punched session's dineroid was refused
                                       // (missing, not session-bound, or invalid)
        uint64_t skipped{0};           // no usable target pair
        uint64_t ignored{0};           // punch messages dropped (unsolicited, refused, stale)
        uint64_t received{0};          // punch messages received, any outcome
        // attempts: attempts with a target of that family; succeeded: by
        // the family of the punched session's address.
        struct Family { uint64_t attempts{0}; uint64_t succeeded{0}; };
        Family ipv4, ipv6;
    };
    PunchStats punch_stats() const;
    // True iff we have an EXPLICIT/confirmed reachable advertised address
    // (operator externalip or port-mapping) — not merely a Gap-1-learned one.
    // Used so a NAT'd node (only a learned, dead address) stays relay-fallback
    // eligible instead of wrongly classifying itself as directly reachable.
    bool has_explicit_advertised() const { return has_explicit_advertised_.load(std::memory_order_acquire); }
    // Gap 2 (A.1): P2PService pushes its port-mapping state here so the relay
    // auto-register gate (MaybeAutoRegisterWithRelays) can apply the SAME
    // reachability predicate as getnetworkinfo. P2PService owns the mapping
    // lifecycle; this is a lock-free mirror it updates on every state change.
    void set_port_mapping_active(bool active) {
        port_mapping_active_.store(active, std::memory_order_release);
    }

    // Gap 1 (peer-reported external-IP learning): record that `reporter_ip` told
    // us (in their version addrRecv) that our external address is `reported_addr`.
    // Once enough DISTINCT peers agree, the address is promoted to an advertised
    // address (with our listen port) via add_advertised_address — so a reachable
    // node auto-discovers + announces its own IP with zero config. Anti-spoof:
    // counts distinct reporter IPs and requires a threshold before trusting.
    void record_self_address_report(const std::string& reported_addr,
                                    const std::string& reporter_ip);

    // Message handling
    void set_message_handler(MessageHandler handler) { message_handler_ = handler; }
    void set_peer_connected_handler(PeerConnectedHandler handler) { peer_connected_handler_ = handler; }
    void set_peer_disconnected_handler(PeerDisconnectedHandler handler) { peer_disconnected_handler_ = handler; }
    void set_peer_height_updated_handler(PeerHeightUpdatedHandler handler) { peer_height_updated_handler_ = handler; }

    // P2P sync fix: Set height provider for version handshake
    void set_height_provider(HeightProvider provider) { height_provider_ = provider; }

    // Set before start(). Provider must use the locally validated chain tip.
    void set_release_cutoff_provider(std::function<bool()> provider) {
        release_cutoff_provider_ = std::move(provider);
    }

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

    // Phase 2b — rolling 24h total bytes carried over relay-virtual peers
    // (sent + received via the relay-virtual data funnels). Surfaced via
    // getnetworkinfo.relay.bytes_relayed_24h.
    uint64_t BytesRelayed24h() const {
        if (!bytes_relayed_24h_) return 0;
        return bytes_relayed_24h_->Total24h();
    }

    // ========================================================================
    // PHASE C: Persistent Peer Database & Adaptive Keepalive
    // ========================================================================
    void load_peers(const std::string& peers_file_path);
    void save_peers(const std::string& peers_file_path);
    void save_peers_with_seeds(const std::string& peers_file_path);
    // Durable relay reachability cache. Fresh relay hints survive an ordinary
    // restart, but are still bounded by the same 15-minute TTL and failure
    // policy as live gossip. The file contains discovery metadata only; relay
    // registrations and circuits are always re-authenticated after reconnect.
    void load_relay_hints(const std::string& relay_hints_file_path);
    void save_relay_hints(const std::string& relay_hints_file_path);

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
    // success. On a TCP peer failures are non-fatal — the handshake
    // continues legacy-style. On a QUIC peer (relayed or direct) the
    // signature must cover nonce || this session's TLS exporter value
    // (dineroid v2), so a relay that terminates both TLS legs cannot pass
    // one side's proof to the other; any failure returns false and the
    // handshake fails. An outbound relayed peer must also turn out to be
    // the node we dialed; otherwise that hint is dropped.
    bool ExchangeDineroId(PeerInfo* peer);

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
    void handle_relay_register_ok(const std::string& peer_address,
                                  const P2PMessage& message);
    // Logs the reason, then acks the RELAY_CONNECT with `status`.
    void refuse_relay_connect(const std::string& peer_address, uint64_t request_id,
                              P2PMessage::RelayConnectStatus status, const std::string& why);

    // Periodic maintenance: drop circuits whose last_data_at is older
    // than kCircuitIdleTimeout. Called from keepalive_loop.
    void SweepIdleCircuits();

    // Relay-coordinated hole punching (Plan 4). PunchTick starts offers to
    // relayed peers we initiate for, fires due dials and probe rounds, and
    // ends expired attempts. Called from keepalive_loop and, while an attempt
    // is active, from punch_loop every kPunchProbeInterval.
    void PunchTick();
    void punch_loop();
    void handle_punch_offer(const std::string& peer_address, const P2PMessage& message);
    void handle_punch_answer(const std::string& peer_address, const P2PMessage& message);
    void handle_punch_go(const std::string& peer_address, const P2PMessage& message);
    // The punched session at peer_key failed its dineroid: count it and end
    // the initiator attempt that dialed it (the relayed peer stays).
    void EndPunchOnFailedProof(const std::array<uint8_t, 20>& expected_id,
                               const std::string& peer_key, bool mismatch);

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
    // relayhn2: verify (only the target can sign its own hint), store, then
    // forward once with hop+1 to NODE_DIALBACK peers while hop < 3, at most
    // 50 forwards a minute.
    void handle_relay_hint_v2(const std::string& peer_address, const P2PMessage& message);

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
    // Direct QUIC peers on the P2P port (UDP). start() calls
    // start_quic_endpoint(listen_port_) when enabled; tests call it with 0.
    void set_quic_enabled(bool enabled) { quic_enabled_ = enabled; }
    bool start_quic_endpoint(uint16_t port);
    dinero::network::QuicEndpoint* quic_endpoint() const { return quic_endpoint_.get(); }
    // Dial ip:port over QUIC. False if not listening, banned, self, or a peer
    // (either transport) already exists at that address.
    bool connect_to_peer_quic(
        const std::string& ip, uint16_t port,
        std::optional<std::array<uint8_t, 20>> punch_expected_id = std::nullopt);
    // NODE_P2P_QUIC while the endpoint is listening, else 0.
    uint64_t local_quic_service_bits() const;
    // p2p.quic_connect: "ip:port" peers to keep connected over QUIC. Dialed by
    // the connection loop (dial_quic_connect_targets) whenever not connected.
    void set_quic_connect_targets(std::vector<std::string> targets);
    void dial_quic_connect_targets();

    // Direct QUIC: wrap an already-started session (from QuicEndpoint) in a
    // PeerInfo keyed "ip:port", insert it and start its handler thread.
    // Returns the peer key.
    std::string install_direct_quic_peer(std::shared_ptr<dinero::network::QuicSession> session,
                                         const dinero::network::UdpAddr& remote,
                                         bool outbound,
                                         std::optional<std::array<uint8_t, 20>> punch_expected_id =
                                             std::nullopt);
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
    // Keepalive tick: registrations with no relayrok within 30 s, and
    // confirmed relays that missed 3 relay pings (one every 30 s), are
    // dropped and the relay is skipped for 30 min; then re-plan.
    void RelayRegistrationTick();

    // Phase 2b — value-type snapshot for the relay_hints.list RPC.
    // Built under relay_hints_mutex_; JSON construction happens with
    // the lock released to keep the critical section short.
    struct RelayHintRpcEndpoint {
        dinero::p2p::NetworkType net;
        std::vector<uint8_t>     addr;
        uint16_t                 port{0};
        uint64_t                 age_seconds{0};
        int                      dial_failures{0};
        bool                     near_eviction{false};
    };
    struct RelayHintRpcTarget {
        std::string                          target_hex;
        std::vector<RelayHintRpcEndpoint>    endpoints;
    };
    struct RelayHintRpcSnapshot {
        std::vector<RelayHintRpcTarget>      entries;
        uint64_t                             ttl_seconds{0};
        int                                  max_failures{0};
    };
    RelayHintRpcSnapshot SnapshotRelayHintsForRpc() const;

    // Phase 3: operator-initiated relay dial for MyNodeDashboard peer actions.
    // This is intentionally constrained to cached RELAY_HINTS entries: callers
    // may ask us to dial a target_node_id through one of the relays we already
    // learned from the network, but cannot use the RPC as an arbitrary relay
    // connect primitive.
    struct ManualRelayDialResult {
        enum class Status {
            Submitted,
            DryRunOk,
            AlreadyConnected,
            NoHint,
            RelayNotConnected,
            InvalidTarget,
        };

        Status status{Status::NoHint};
        std::string target_node_id_hex;
        std::string relay_endpoint;
        uint64_t request_id{0};
    };

    ManualRelayDialResult TryDialRelayHint(
        const std::string& target_node_id_hex,
        const std::optional<std::string>& relay_endpoint,
        bool dry_run);

#ifdef DINERO_TEST_BUILD
    bool test_dineroid_v1_only_{false};
    bool test_shutdown_requested() const {
        return shutdown_requested_.load(std::memory_order_acquire);
    }
    // Exercise the real connection-manager pass and connect timeout without
    // depending on host routing or a reachable external peer.
    void test_set_connection_manager_dial(
        std::function<bool(const std::string&, uint16_t)> dial) {
        test_connection_manager_dial_ = std::move(dial);
    }
    void test_set_outbound_connect_poll(
        std::function<int(int, std::chrono::milliseconds)> poll) {
        test_outbound_connect_poll_ = std::move(poll);
    }
    void test_set_ipv4_resolver(
        std::function<std::optional<std::string>(const std::string&)> resolver) {
        test_ipv4_resolver_ = std::move(resolver);
    }
    void test_set_before_final_peers_save(std::function<void()> before_save) {
        test_before_final_peers_save_ = std::move(before_save);
    }
    void test_set_before_peer_handler_start(std::function<void(int)> before_start) {
        test_before_peer_handler_start_ = std::move(before_start);
    }
    bool test_is_connecting_peer(const std::string& address, uint16_t port) const {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        return connecting_peers_.count(dinero::network::FormatEndpoint(address, port)) != 0;
    }
    void test_handle_incoming_connection(int socket_fd, const std::string& address) {
        handle_incoming_connection(socket_fd, address);
    }
    std::optional<std::string> test_resolve_ipv4_for_dial(
        const std::string& host, std::chrono::milliseconds timeout) const {
        return resolve_ipv4_for_dial(host, timeout);
    }
    static unsigned test_resolver_in_flight();
    bool test_socks5_transfer_all(int socket_fd, uint8_t* data, size_t len,
                                  bool sending, std::chrono::milliseconds timeout) {
        return socks5_transfer_all(socket_fd, data, len, sending,
            std::chrono::steady_clock::now() + timeout);
    }
    int test_wait_for_outbound_connect() { return wait_for_outbound_connect(-1); }

    // #373 fd-hygiene hooks: drive the private cleanup path and observe the
    // stored fd so tests can prove close-and-invalidate (no double-close).
    bool test_send_peer_message(PeerInfo* peer, const P2PMessage& message) { return send_peer_message(peer, message); }
    bool test_perform_handshake(PeerInfo* peer) { return perform_handshake(peer); }
    // An encrypted relay virtual peer whose QuicSession never started (so is
    // never active), connected via `relay_endpoint`. Returns its key.
    std::string test_install_dead_quic_relay_peer(const std::string& relay_endpoint);
    void test_run_quic_reader_loop(const std::string& key);
    size_t test_peer_thread_count() {
        std::lock_guard<std::mutex> lock(peer_threads_mutex_);
        return peer_threads_.size();
    }
    void test_add_self_candidate(const std::string& ip, uint16_t port);
    void test_run_reachability_tick() { run_reachability_tick(); }
    uint64_t test_send_reachchk(const std::string& peer_key, uint16_t port,
                                const std::vector<uint8_t>& trailing = {});
    std::optional<uint8_t> test_reach_result(uint64_t nonce) const;
    std::vector<std::string> test_dialback_targets() const;
    void test_mark_self_verified(const std::string& ip, uint16_t port);
    size_t test_reach_checks_sent() const;
    std::vector<std::pair<std::string, uint16_t>> test_local_advertised_addresses() const {
        return get_local_advertised_addresses();
    }
    bool test_inbound_slot_available(const std::string& ip) const {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        return inbound_slot_available_locked(ip, nullptr);
    }
    void test_set_inbound_limits(size_t total, size_t per_ip) {
        max_inbound_ = total;
        max_inbound_per_ip_ = per_ip;
    }
    void test_sweep_release_compatibility() { sweep_release_compatibility(); }
    void test_process_message(const std::string& key, const P2PMessage& message) { process_message(key, message); }
    void test_set_release_capability(const std::string& key, bool capable, bool complete = true);
    void test_run_outbox() { outbox_loop(); }
    void test_stop_outbox() { shutdown_requested_.store(true); outbox_cv_.notify_all(); }
    void test_cleanup_peer(const std::string& peer_address);
    int test_peer_socket_fd(const std::string& peer_address) const;  // -2 if absent
    void test_set_running(bool running);  // lets tests reach stop()'s close loop
    void set_plaintext_relay_dev_override_for_tests(bool allowed);
    void set_encrypted_relay_dev_override_for_tests(bool allowed);
    bool test_plaintext_relay_transport_allowed() const;
    void test_install_connected_direct_peer(
        const std::string& peer_address,
        int socket_fd,
        bool is_outbound,
        bool identity_proven,
        const std::array<uint8_t, 20>& node_id);
    void test_set_peer_connected(const std::string& peer_address,
                                 bool connected);
    void test_maybe_auto_register_with_relays();
    void test_relay_registration_tick() { RelayRegistrationTick(); }
    void test_refresh_relay_registrations() { RefreshRelayRegistrations(); }
    void test_signed_relay_hints_tick() { SignedRelayHintsTick(/*force=*/true); }
    void test_orchestrate_relay_dials() { OrchestrateRelayDials(); }
    size_t test_relay_hint_count(const std::array<uint8_t, 20>& target) const;
    size_t test_signed_relay_hint_count(const std::array<uint8_t, 20>& target) const;
    size_t test_relay_hint_target_count() const;
    size_t test_relay_circuit_count() const;
    void test_set_peer_services(const std::string& peer_key, uint64_t services);
    void test_set_peer_best_height(const std::string& peer_key, uint32_t height);
    // A relayed (via_relay, encrypted QUIC) peer on a caller-owned session;
    // `expected_target` set means an outbound dial of that node id.
    std::string test_install_quic_relay_peer(
        std::shared_ptr<dinero::network::QuicSession> session, const std::string& relay_address,
        uint64_t circuit_id, std::optional<std::array<uint8_t, 20>> expected_target);
    // Behave like a pre-v8.2.0 build: send only dineroid v1.
    void test_set_dineroid_v1_only(bool v1_only) { test_dineroid_v1_only_ = v1_only; }
    // Stands in for a listening QUIC endpoint (NGTCP2-off builds cannot start
    // one). Interface addresses are injected through
    // network::test_set_interface_enumerator_override (local_interfaces.h).
    void test_set_quic_port(uint16_t port);
    // Hole punching: drive the punch state machine, replace its clock (set it
    // before any punch traffic), override LocalPunchCandidates (loopback
    // candidates are never routable), read the counters.
    void test_punch_tick() { PunchTick(); }
    void test_set_punch_clock(std::function<std::chrono::steady_clock::time_point()> clock);
    void test_set_local_punch_candidates(std::optional<dinero::p2p::PunchCandidates> c);
    PunchStats test_punch_stats() const;
    void test_handle_getaddr(const std::string& peer_key) { handle_getaddr(peer_key, P2PMessage{}); }
    // Gives a test-installed peer (keyed "host:port", port 0) its real
    // endpoint, so to_string() matches relay endpoint keys.
    void test_set_peer_endpoint(const std::string& peer_key, const std::string& host,
                                uint16_t port);
    void test_set_peer_proven_identity(const std::string& peer_key,
                                       const std::array<uint8_t, 20>& node_id,
                                       const std::array<uint8_t, 33>& pubkey, uint64_t our_nonce);
    size_t test_confirmed_relay_count() const;
    std::vector<std::string> test_configured_relay_endpoints() const;
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
    // Force/read the relay behind-tip throttle (normally set by
    // RecomputeRelayBehindThrottle when this relay is >kRelayMaxBlocksBehind
    // behind its peers). Lets tests verify that a behind relay refuses new
    // circuits cleanly without black-holing already-open circuit data.
    void test_set_relay_behind_throttle(bool behind);
    uint64_t test_relay_drops_behind_count() const;
    void test_install_relay_circuit(uint64_t circuit_id,
                                    const std::string& requester_addr,
                                    const std::string& target_addr);
    void test_register_relay_target(const std::array<uint8_t, 20>& node_id,
                                    const std::string& peer_address);
    // Frames dropped by the per-circuit, global or monthly cap.
    uint64_t test_relay_cap_drop_frames() const;
    void test_recompute_relay_behind_throttle() { RecomputeRelayBehindThrottle(); }
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
    static constexpr size_t MAX_OUTBOUND_CONNECTIONS =
        dinero::p2p::kTargetDurableOutbound;
    // Outbound slots for relayed/punched ("home") peers, counted apart from
    // MAX_OUTBOUND_CONNECTIONS so a full direct set still gets a relayed link.
    static constexpr size_t kHomePeerSlots = 2;
    static constexpr std::chrono::minutes FEELER_INTERVAL{2};
    static constexpr size_t MAX_INBOUND_CONNECTIONS = 125;
    // Two full nodes behind one household/office NAT can each maintain several
    // direct and relay connections. Keep the cap bounded, but high enough that
    // one healthy node does not starve the second (#279).
    static constexpr size_t MAX_INBOUND_PER_IP = 12;
    // Serialize writes per-socket (not globally) to avoid interleaved frames
    // while preventing one stalled peer from blocking all other peers.
    mutable std::mutex socket_send_mutexes_guard_;
    mutable std::unordered_map<int, std::shared_ptr<std::mutex>> socket_send_mutexes_;

    // Ring 3 Phase 4e: TS3 Liveness - Interruptible waits for event loops
    std::mutex keepalive_mutex_;
    std::condition_variable keepalive_cv_;
    std::mutex connection_manager_mutex_;
    std::condition_variable connection_manager_cv_;
    // Serializes the full peers.dat.tmp -> peers.dat transaction. Without it,
    // concurrent save triggers race on the shared temporary path (especially
    // visible as ERROR_SHARING_VIOLATION on Windows).
    std::mutex peers_file_mutex_;

    uint16_t listen_port_;
    std::string user_agent_;
    std::string external_ip_;  // Node's external IP (for self-loop detection)
    dinero::network::VersionNonceTracker version_nonces_;
    uint32_t protocol_version_;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> network_active_{true};
    // Direct QUIC transport. Lock order: the endpoint calls AcceptFn under its
    // own mutex and AcceptFn takes peers_mutex_, so never call into the
    // endpoint while holding peers_mutex_.
    std::unique_ptr<dinero::network::QuicEndpoint> quic_endpoint_;
    dinero::network::QuicSessionOptions quic_p2p_options_;
    bool quic_enabled_{true};
    std::vector<std::string> quic_connect_targets_;

    // ---- dial-back verification ----
    // Lock order: peers_mutex_ before reach_mutex_; reach_mutex_ is a leaf.
    mutable std::mutex reach_mutex_;
    std::unique_ptr<dinero::p2p::SelfReachability> self_reach_ =
        std::make_unique<dinero::p2p::SelfReachability>(
            dinero::p2p::SelfReachability::Params{}, [] { return std::chrono::steady_clock::now(); });
    std::set<std::pair<std::string, uint16_t>> stun_reflexive_;  // last STUN round's answers
    dinero::p2p::NatMappingType nat_mapping_type_{dinero::p2p::NatMappingType::Unknown};
#ifdef DINERO_TEST_BUILD
    uint16_t test_quic_port_{0};
#endif
    struct PendingReach {
        dinero::p2p::Candidate candidate;
        dinero::p2p::Verifier verifier;
        std::chrono::steady_clock::time_point sent_at;
    };
    std::map<uint64_t, PendingReach> pending_reach_;          // our requests by nonce
    std::map<uint64_t, uint8_t> reach_results_;               // last results (bounded)
    size_t reach_checks_sent_{0};
    // Verifier side: request windows and the targets we dialed (bounded).
    // Keyed by requester address (IPv4, or IPv6 /64), not by connection.
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> reach_window_by_peer_;
    std::atomic<bool> dialback_private_targets_{false};
    std::deque<std::chrono::steady_clock::time_point> reach_window_global_;
    size_t dialbacks_in_flight_{0};
    std::deque<std::string> dialback_targets_;
    void run_reachability_tick();
    void handle_reach_check(const std::string& peer_address, const P2PMessage& message);
    void handle_reach_result(const std::string& peer_address, const P2PMessage& message);
    uint64_t send_reach_check(const std::string& peer_key, const dinero::p2p::Candidate& c,
                              const dinero::p2p::Verifier& v, const std::vector<uint8_t>& trailing);
    // Dial ip:port, run a version handshake, report whether it is `expected`:
    // 0 ok, 2 connect/handshake failed, 3 a different node answered.
    uint8_t dialback_probe(const std::string& ip, uint16_t port,
                           const std::array<uint8_t, 20>& expected);
    // Caller holds peers_mutex_: explicit, or dial-back verified.
    bool gossipable_locked(const std::string& address, uint16_t port) const;
    std::atomic<bool> quic_connect_warned_{false};
    size_t max_inbound_ = MAX_INBOUND_CONNECTIONS;
    size_t max_inbound_per_ip_ = MAX_INBOUND_PER_IP;

    // Socket readiness observability (for deterministic tests)
    std::atomic<bool> socket_listening_{false};
    mutable std::mutex ready_mutex_;
    std::condition_variable ready_cv_;

    // Threading
    std::unique_ptr<std::thread> listen_thread_;
    std::unique_ptr<std::thread> connection_manager_thread_;
    std::unique_ptr<std::thread> keepalive_thread_;  // Phase C: Adaptive keepalive
    std::unique_ptr<std::thread> punch_thread_;      // drives active hole punches

    // ---- hole punching ----
    // One attempt per remote node id, either role. Lock order: punch_mutex_
    // is a leaf; nothing else is taken while it is held, and sends, dials and
    // probes happen with no lock held. punch_tick_mutex_ serializes PunchTick
    // (keepalive, punch_loop, tests) and is taken before any other lock.
    struct PunchAttempt {
        enum class Phase { Offered, AwaitGo, Dialing, Probing };
        dinero::p2p::PunchRole role{dinero::p2p::PunchRole::None};
        Phase phase{Phase::Offered};
        uint64_t nonce{0};
        std::string relay_key;  // the relayed peer the exchange runs on
        dinero::p2p::PunchCandidates local;  // what we sent
        std::chrono::steady_clock::time_point offer_sent_at{};
        std::chrono::steady_clock::time_point deadline{};
        std::vector<dinero::network::UdpAddr> targets;
        // Initiator: when to dial, the next target to try, the dialed peer key
        // and when that dial gives way to the next target (each target gets an
        // equal share of what is left of the window).
        std::chrono::steady_clock::time_point dial_at{};
        size_t next_target{0};
        std::string dialed_key;
        std::chrono::steady_clock::time_point dial_expires{};
        // Acceptor: probe rounds sent and when the next one is due.
        size_t probe_rounds{0};
        std::chrono::steady_clock::time_point next_probe_at{};
    };
    std::mutex punch_tick_mutex_;
    mutable std::mutex punch_mutex_;
    std::condition_variable punch_cv_;
    bool punch_wake_{false};  // guarded by punch_mutex_
    std::map<std::array<uint8_t, 20>, PunchAttempt> punch_attempts_;
    std::function<std::chrono::steady_clock::time_point()> punch_clock_;
    std::optional<dinero::p2p::PunchScheduler> punch_scheduler_;
    PunchStats punch_stats_;
    std::chrono::steady_clock::time_point PunchNow() const;
    // Under punch_mutex_: the next probe round's targets, or empty when none is due.
    std::vector<dinero::network::UdpAddr> TakeProbeRoundLocked(
        PunchAttempt& a, std::chrono::steady_clock::time_point now);
    void SendPunchProbes(const std::vector<dinero::network::UdpAddr>& targets);
#ifdef DINERO_TEST_BUILD
    std::optional<dinero::p2p::PunchCandidates> test_local_punch_candidates_;  // reach_mutex_
#endif
    std::mutex peer_threads_mutex_;
    // Per-peer threads (handlers, QUIC readers). `done` is set when the
    // thread's function returns so spawn_peer_thread_locked can join and drop
    // it; stop() joins whatever is left.
    struct PeerThread {
        std::unique_ptr<std::thread> thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<PeerThread> peer_threads_;
    // Caller holds peer_threads_mutex_. Reaps finished threads, then starts
    // `fn`. False if the thread could not be created (never throws).
    bool spawn_peer_thread_locked(std::function<void()> fn);

    // Peer management
    mutable std::mutex peers_mutex_;
    // Ring 3 Phase 4c: Changed unique_ptr → shared_ptr for TS1 compliance
    // Allows worker threads to hold weak_ptr, preventing use-after-free
    std::unordered_map<std::string, std::shared_ptr<PeerInfo>> connected_peers_;
    std::unordered_set<std::string> connecting_peers_;  // Guards against duplicate connection attempts
    std::atomic<bool> feeler_in_progress_{false};
    std::chrono::steady_clock::time_point last_feeler_attempt_{
        std::chrono::steady_clock::now()};
    std::vector<std::pair<std::string, uint16_t>> seed_nodes_;
    std::vector<std::pair<std::string, uint16_t>> discovered_nodes_;
    // Mandatory recovery peers use the normal transport, but are tracked
    // separately so dynamic discovery can never crowd them out.
    std::vector<std::pair<std::string, uint16_t>> anchor_nodes_;
    std::vector<std::pair<std::string, uint16_t>> advertised_addresses_;
    std::unordered_set<std::string> explicit_advertised_addresses_;
    // Operator externalip subset: always gossiped, always reachable.
    std::unordered_set<std::string> operator_addresses_;
    // True once an EXPLICIT/trusted reachable address (operator externalip or
    // port-mapping) has been advertised — distinct from Gap-1-learned addresses.
    // Drives the relay-fallback gate so NAT'd nodes (learned-only) stay eligible.
    std::atomic<bool> has_explicit_advertised_{false};
    std::atomic<bool> has_operator_address_{false};
    void notify_reachability_if_changed();
    std::mutex reachability_handler_mutex_;
    std::function<void(bool)> reachability_changed_handler_;
    bool last_reported_reachable_{false};  // guarded by reachability_handler_mutex_
    // Gap 2 (A.1): lock-free mirror of P2PService::port_mapping_active_, pushed
    // via set_port_mapping_active(). Lets the relay auto-register gate apply the
    // same reachability predicate (IsDirectlyReachable) as the getnetworkinfo RPC.
    std::atomic<bool> port_mapping_active_{false};

    // Gap 1 (peer-reported external-IP learning): votes for our own external
    // address. Key = address a peer reported as ours (addrRecv); value = set of
    // distinct reporter IPs. Promoted to advertised once a distinct-peer
    // threshold is met. Guarded by self_addr_mutex_.
    mutable std::mutex self_addr_mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> self_addr_votes_;
    std::string onion_proxy_host_;
    uint16_t onion_proxy_port_{0};
    dinero::p2p::AddressManager* address_manager_{nullptr};  // Owned by AddressManagerService
    std::string peers_file_path_;  // Phase C: Persistent peer database path
    std::string relay_hints_file_path_;
    std::atomic<bool> relay_hints_dirty_{false};
    mutable std::mutex bans_mutex_;
    std::unordered_map<std::string, BanEntry> banned_peers_;
    
    // Message handling
    MessageHandler message_handler_;
    PeerConnectedHandler peer_connected_handler_;
    PeerDisconnectedHandler peer_disconnected_handler_;
    PeerHeightUpdatedHandler peer_height_updated_handler_;
    std::function<bool()> release_cutoff_provider_;
    bool release_peer_allowed(const PeerInfo& peer) const;
    bool check_release_handshake(PeerInfo* peer);
    bool enforce_release_compatibility(const std::string& key);
    void sweep_release_compatibility();
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
    // but N is small (typically up to five relays per node).
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
    static constexpr size_t kAutoRelayTargetCount = 2;  // on distinct netgroups
    static constexpr std::chrono::seconds kRelayAckTimeout{30};
    static constexpr std::chrono::seconds kRelayPingInterval{30};
    static constexpr uint32_t kRelayMaxMissedPings = 3;
    static constexpr std::chrono::minutes kRelaySkipAfterFailure{30};
    // How long a node still counts as dual-stack after its last direct
    // outbound IPv4 connection (see MaybeAutoRegisterWithRelays); guarded
    // by peers_mutex_.
    static constexpr std::chrono::hours kDualStackMemory{1};
    std::chrono::steady_clock::time_point last_direct_ipv4_at_{};
    // Relays that never acked or stopped answering pings; guarded by
    // relay_endpoints_mutex_.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> relay_skip_until_;
    // Relay candidates we dialed and are waiting to connect; guarded by
    // relay_endpoints_mutex_.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> relay_dialed_at_;
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

    // Relay bandwidth caps — global egress token bucket, a calendar-month
    // (UTC) quota persisted across restarts, and the syncing auto-suspend
    // gate. relay_global_bucket_ / relay_quota_* are touched only under
    // circuits_mutex_; the atomics need no lock.
    static constexpr double kRelayGlobalRateBps = 200.0 * 1024.0;     // 200 KB/s
    static constexpr double kRelayGlobalBurstBytes = 200.0 * 1024.0;  // 1s burst
    static constexpr uint32_t kRelayMaxBlocksBehind = 6;
    dinero::network::TokenBucket relay_global_bucket_{kRelayGlobalRateBps,
                                                      kRelayGlobalBurstBytes};
    std::atomic<bool> relay_service_enabled_{true};
    RelayServiceLimits relay_service_limits_{};
    struct RelayRequestWindow {
        std::chrono::steady_clock::time_point started{};
        uint32_t requests{0};
    };
    std::unordered_map<std::string, RelayRequestWindow> relay_request_windows_;
    uint64_t relay_quota_bytes_ = 0;
    std::string relay_quota_month_;  // "YYYY-MM" the bytes belong to
    std::string relay_quota_path_;
    bool relay_quota_dirty_ = false;
    std::chrono::steady_clock::time_point relay_quota_saved_at_{};
    std::atomic<bool> relay_quota_exhausted_{false};
    HeightProvider best_header_height_provider_;
    std::atomic<bool> relay_behind_throttle_{false};
    std::atomic<uint64_t> relay_drops_circuit_{0};
    std::atomic<uint64_t> relay_drops_global_{0};
    std::atomic<uint64_t> relay_drops_quota_{0};
    std::atomic<uint64_t> relay_drops_behind_{0};
    // Monthly quota check; caller holds circuits_mutex_.
    bool RelayQuotaAllows(size_t bytes);
    std::string RelayQuotaMonthNow() const;  // "YYYY-MM", UTC, from clock_
    // Writes relay_quota_path_ when dirty: always if `force`, else at most
    // once a minute.
    void save_relay_quota(bool force);
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
    static constexpr std::chrono::seconds kRelayConnectSweepPeriod{2};
    static constexpr size_t kRelayMaxCircuitsPerNetGroup = 4;

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
        // relayhn2 only: the target's own signed hint (empty for legacy
        // relayhnt) and its expiry, unix seconds.
        std::vector<uint8_t> v2_payload;
        uint32_t v2_expiry{0};
    };
    // Time source for TTL/expiry logic in the hints subsystem.
    // Defaults to SystemClockSource; tests inject a FakeClockSource.
    std::unique_ptr<dinero::network::ClockSource> clock_;

    // Phase 2b: rolling 24h byte counter for bytes carried over relay-virtual
    // peers (both directions). Declared after clock_ — clock_ is always set
    // in the constructor body before bytes_relayed_24h_.emplace() is called.
    // std::optional defers construction until clock_ is live.
    std::optional<dinero::network::Rolling24hCounter> bytes_relayed_24h_;

    // Increment bytes_relayed_24h_ from the relay-virtual data funnels.
    // Safe to call on any thread; Rolling24hCounter::Add is lock-free in
    // the hot path.
    void RecordRelayBytes(uint64_t bytes) {
        if (bytes_relayed_24h_) bytes_relayed_24h_->Add(bytes);
    }

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
    // Relays named only by a signed hint, dialed so RELAY_CONNECT can follow:
    // relay host:port -> not before. Failed ones wait kHintRelaySkipAfterFailure.
    std::unordered_map<std::string /*relay host:port*/, std::chrono::steady_clock::time_point>
        last_hint_relay_dial_;
    static constexpr std::chrono::hours kHintRelaySkipAfterFailure{6};
    // At most 2 such cold dials per 10 minutes.
    dinero::network::TokenBucket hint_cold_dial_budget_{2.0 / 600.0, 2.0};
    void drop_all_relay_hints_via(const std::string& relay_address);
    // relayhn2: every 10 min a NAT'd target re-signs its hints (30 min
    // expiry) and a relay re-advertises the signed hints of its registrants.
    static constexpr std::chrono::minutes kSignedHintPeriod{10};
    static constexpr uint32_t kSignedHintLifetimeSeconds = 30 * 60;
    std::chrono::steady_clock::time_point last_signed_hint_round_{};
    std::mutex hint_forwarder_mutex_;
    dinero::p2p::HintForwarder hint_forwarder_;
    std::atomic<uint64_t> hints_v2_rejected_{0};
    // An outbound relayed dial reached someone other than the target: drop
    // the target's hints through that relay.
    // Caller holds relay_hints_mutex_. False: no room for this hint.
    bool MakeRoomForHint(std::vector<RelayHintRecord>& bucket, bool incoming_signed);
    // Caller holds relay_hints_mutex_. Keeps the table at kMaxHintTargets.
    void EnforceHintTargetCap();
    static constexpr size_t kMaxHintTargets = 1024;
    static constexpr uint32_t kHintIngestPerPeerPerMinute = 30;
    // peer -> (window start, relayhn2 seen); guarded by hint_forwarder_mutex_.
    std::unordered_map<std::string, std::pair<std::chrono::steady_clock::time_point, uint32_t>>
        hint_ingest_windows_;
    void drop_relay_hints_via(const std::array<uint8_t, 20>& target, const std::string& relay_address);
    void SignedRelayHintsTick(bool force);
    void SendSignedRelayHint(const std::vector<uint8_t>& payload, const std::string& except_peer);

public:
    // ── Relay health scoring (auto-registration SET) ────────────────────
    // Per-relay health for the relays we REGISTER WITH to stay reachable
    // (distinct from RelayHintRecord, which is dial-side hint health).
    // Updated by real outcomes — RELAYREG confirmation (a relay advertising us
    // back via RELAYHINTS received_self), inbound relay-circuit handshake
    // success, and failures (circuit idle-close, relay connection drop). The
    // score is an EWMA in [0,1] so recent outcomes dominate (failure decay);
    // MaybeAutoRegisterWithRelays prefers high-score relays and replaces bad
    // ones, while preserving >=1 known-good bootstrap relay.
    void note_relay_outcome(const std::string& relay_endpoint, bool success);
    double relay_health_score(const std::string& relay_endpoint) const;  // 0.5 if unknown
    bool relay_health_replace_eligible(const std::string& relay_endpoint) const;

private:
    struct RelayHealth {
        double   score{0.5};                 // EWMA, neutral prior
        uint32_t successes{0};
        uint32_t failures{0};
        int      consecutive_failures{0};
        std::chrono::steady_clock::time_point last_update{};
        std::chrono::steady_clock::time_point last_success{};
    };
    mutable std::mutex relay_health_mutex_;
    std::unordered_map<std::string /*endpoint_key host:port*/, RelayHealth> relay_health_;
    static constexpr double kRelayHealthAlpha = 0.3;            // EWMA weight on new sample
    static constexpr int    kRelayMaxConsecutiveFailures = 4;   // replace at/after this many
    static constexpr double kRelayHealthReplaceFloor = 0.25;    // score below + tried → replace
    static constexpr uint32_t kRelayHealthMinSamples = 3;       // need this many before score-based eviction

#ifdef DINERO_TEST_BUILD
    std::atomic<bool> plaintext_relay_dev_override_for_tests_{false};
    std::atomic<bool> encrypted_relay_dev_override_for_tests_{false};
    std::function<bool(const std::string&, uint16_t)> test_connection_manager_dial_;
    std::function<int(int, std::chrono::milliseconds)> test_outbound_connect_poll_;
    std::function<std::optional<std::string>(const std::string&)> test_ipv4_resolver_;
    std::function<void()> test_before_final_peers_save_;
    std::function<void(int)> test_before_peer_handler_start_;
#endif

    // Network threads
    void listen_loop();
    void connection_manager_loop();
    int wait_for_outbound_connect(int socket_fd);
    std::optional<std::string> resolve_ipv4_for_dial(
        const std::string& host,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;
    bool socks5_transfer_all(int socket_fd, uint8_t* data, size_t len, bool sending,
                             std::chrono::steady_clock::time_point deadline);
    bool connect_to_peer_impl(const std::string& address, uint16_t port,
                              bool is_feeler,
                              const std::optional<std::string>& pre_resolved_ip = std::nullopt);
    // Ring 3 Phase 4c: Changed to shared_ptr for TS1 compliance
    bool start_peer_handler_thread(std::shared_ptr<PeerInfo> peer);
    // Inbound caps shared by TCP and QUIC. Caller holds peers_mutex_.
    bool inbound_slot_available_locked(const std::string& ip, std::string* why) const;
    // Frame one message onto a QUIC peer's stream (relay circuit or direct).
    bool send_quic_stream_message(PeerInfo& peer, const P2PMessage& message);
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
    // Best-effort IPv6 listener on listen_port_ (IPV6_V6ONLY); -1 if the host
    // has no usable IPv6, and the node stays v4-only.
    int create_listen_socket_v6();
    int create_client_socket(const std::string& address, uint16_t port);
    int create_socks5_client_socket(const std::string& proxy_host,
                                    uint16_t proxy_port,
                                    const std::string& target_host,
                                    uint16_t target_port);
    void close_socket(int socket_fd);
    void set_socket_nonblocking(int socket_fd);
    void set_socket_send_timeout(int socket_fd, int seconds);
    void set_socket_keepalive(int socket_fd);
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
