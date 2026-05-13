#pragma once

#include "lightning/lightning_types.h"
#include "lightning/lightning_db_interface.h"
#include "lightning/lightning_db_types.h"
#include "din_json.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <queue>
// Phase 8.5: NO <chrono> include - wall time is FORBIDDEN
#include <condition_variable>
#include <map>

// Forward declaration for time oracle
namespace lightning {
    class ITimeOracle;  // Phase 8.5
}

namespace dinero {
namespace lightning {

/**
 * @file lightning_peer.h
 * @brief Lightning Network peer connection and BOLT protocol implementation
 *
 * Phase 7.5: BOLT Protocol & Peer Networking
 *
 * This module implements:
 * - BOLT #1: Base Protocol (init, ping/pong, error)
 * - BOLT #2: Peer Protocol (channel lifecycle messages)
 * - BOLT #8: Encrypted Transport (Noise_XK handshake)
 * - BOLT #9: Feature Bits (capability negotiation)
 *
 * Architecture:
 * ┌─────────────────┐      ┌─────────────────┐
 * │  PeerManager    │◄────►│ LightningPeer   │
 * │  (orchestrator) │      │ (connection)    │
 * └─────────────────┘      └─────────────────┘
 *         │                        │
 *         │                        ▼
 *         │                ┌──────────────────┐
 *         │                │ BOLTMessage      │
 *         │                │ (wire protocol)  │
 *         │                └──────────────────┘
 *         │
 *         ▼
 * ┌─────────────────┐
 * │ ChannelManager  │
 * │ HTLCManager     │
 * └─────────────────┘
 */

// ═══════════════════════════════════════════════════════════════════════════
// BOLT Message Types (BOLT #1 and BOLT #2)
// ═══════════════════════════════════════════════════════════════════════════

// Windows wingdi.h defines ERROR as a macro
#ifdef ERROR
#undef ERROR
#endif

enum class BOLTMessageType : uint16_t {
    // BOLT #1: Base Protocol
    INIT             = 16,      // Feature negotiation
    ERROR            = 17,      // Error notification
    PING             = 18,      // Keepalive request
    PONG             = 19,      // Keepalive response
    WARNING          = 1,       // Non-fatal warning

    // BOLT #2: Peer Protocol - Channel Lifecycle
    OPEN_CHANNEL     = 32,      // Initiate channel open
    ACCEPT_CHANNEL   = 33,      // Accept channel open
    FUNDING_CREATED  = 34,      // Funding tx created
    FUNDING_SIGNED   = 35,      // Funding tx signed
    FUNDING_LOCKED   = 36,      // Channel ready (old name)
    CHANNEL_READY    = 36,      // Channel ready (new name, same value)
    SHUTDOWN         = 38,      // Initiate cooperative close
    CLOSING_SIGNED   = 39,      // Closing tx signature

    // BOLT #2: Peer Protocol - HTLCs
    UPDATE_ADD_HTLC  = 128,     // Add HTLC to channel
    UPDATE_FULFILL_HTLC = 130,  // Settle HTLC with preimage
    UPDATE_FAIL_HTLC = 131,     // Fail HTLC
    UPDATE_FAIL_MALFORMED_HTLC = 135, // Malformed HTLC

    // BOLT #2: Peer Protocol - Commitment Updates
    COMMITMENT_SIGNED = 132,    // Sign new commitment
    REVOKE_AND_ACK   = 133,     // Revoke old commitment
    UPDATE_FEE       = 134,     // Update fee rate

    // BOLT #7: P2P Node and Channel Discovery
    CHANNEL_ANNOUNCEMENT = 256, // Announce new channel
    NODE_ANNOUNCEMENT = 257,    // Announce node info
    CHANNEL_UPDATE   = 258,     // Update channel parameters
    ANNOUNCEMENT_SIGNATURES = 259, // Exchange channel announcement sigs

    // BOLT #9: Query Channel Range (for gossip sync)
    QUERY_SHORT_CHANNEL_IDS = 261,
    REPLY_SHORT_CHANNEL_IDS = 262,
    QUERY_CHANNEL_RANGE = 263,
    REPLY_CHANNEL_RANGE = 264,
    GOSSIP_TIMESTAMP_FILTER = 265,

    UNKNOWN          = 0        // Unknown message type
};

/**
 * @struct BOLTMessage
 * @brief Lightning Network wire protocol message
 *
 * Format (BOLT #1):
 *   2-byte: type (big-endian)
 *   2-byte: payload length (big-endian)
 *   N-byte: payload
 */
struct BOLTMessage {
    BOLTMessageType type;               // Message type
    std::vector<uint8_t> payload;       // Message payload
    uint64_t timestamp;                 // Received timestamp

    BOLTMessage()
        : type(BOLTMessageType::UNKNOWN),
          timestamp(0) {}

    BOLTMessage(BOLTMessageType t, const std::vector<uint8_t>& p)
        : type(t), payload(p), timestamp(0) {}

    // Serialize to wire format
    std::vector<uint8_t> serialize() const;

    // Deserialize from wire format
    static Result<BOLTMessage> deserialize(const std::vector<uint8_t>& data);
};

// ═══════════════════════════════════════════════════════════════════════════
// Feature Bits (BOLT #9)
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @struct FeatureBits
 * @brief Lightning feature flags for capability negotiation
 *
 * Bit positions (odd = optional, even = required):
 * - 0/1:   option_data_loss_protect
 * - 4/5:   option_upfront_shutdown_script
 * - 8/9:   var_onion_optin
 * - 12/13: option_static_remotekey
 * - 14/15: payment_secret
 * - 16/17: basic_mpp
 * - 18/19: option_support_large_channel
 * - 22/23: option_anchors
 * - 26/27: option_shutdown_anysegwit
 */
struct FeatureBits {
    std::vector<uint8_t> bits;  // Feature bit vector

    FeatureBits();  // Initialize with default features

    bool supports(uint32_t bit_position) const;
    void set(uint32_t bit_position);
    void clear(uint32_t bit_position);

    // Serialize to TLV format
    std::vector<uint8_t> serialize() const;
    static FeatureBits deserialize(const std::vector<uint8_t>& data);
};

// ═══════════════════════════════════════════════════════════════════════════
// Lightning Peer Connection State
// ═══════════════════════════════════════════════════════════════════════════

enum class PeerState {
    DISCONNECTED,       // Not connected
    CONNECTING,         // TCP connection in progress
    HANDSHAKING,        // BOLT #8 Noise handshake
    AWAITING_INIT,      // Waiting for INIT message
    READY,              // Ready for channel operations
    ERROR_STATE         // Fatal error occurred
};

inline std::string peerStateToString(PeerState state) {
    switch (state) {
        case PeerState::DISCONNECTED: return "DISCONNECTED";
        case PeerState::CONNECTING: return "CONNECTING";
        case PeerState::HANDSHAKING: return "HANDSHAKING";
        case PeerState::AWAITING_INIT: return "AWAITING_INIT";
        case PeerState::READY: return "READY";
        case PeerState::ERROR_STATE: return "ERROR";
        default: return "UNKNOWN";
    }
}

/**
 * @struct PeerStats
 * @brief Statistics for a Lightning peer connection
 */
struct PeerStats {
    uint64_t messages_sent = 0;
    uint64_t messages_received = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t ping_time_ms = 0;
    uint64_t connected_at = 0;
    uint64_t last_message_at = 0;

    din::Json toJson() const;
};

// ═══════════════════════════════════════════════════════════════════════════
// LightningPeer - Single peer connection
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @class LightningPeer
 * @brief Single Lightning Network peer connection
 *
 * Lifecycle:
 *   1. Connect to peer (TCP)
 *   2. Perform Noise_XK handshake (BOLT #8) [TODO: Phase 7.6]
 *   3. Exchange INIT messages (feature negotiation)
 *   4. Ready for channel operations
 *
 * Thread Safety: All public methods are thread-safe
 */
class LightningPeer {
public:
    /**
     * @brief Construct LightningPeer
     * @param node_id Remote peer's 33-byte compressed pubkey (hex)
     * @param address IP:port address (e.g., "192.168.1.1:9735")
     * @param inbound True if peer connected to us
     * @param time_oracle Time oracle for deterministic timestamps (Phase 8.5)
     */
    LightningPeer(
        const std::string& node_id,
        const std::string& address,
        bool inbound = false,
        ::lightning::ITimeOracle* time_oracle = nullptr
    );
    ~LightningPeer();

    // ═══════════════════════════════════════════════════════════════════════════
    // Connection Lifecycle
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Connect to remote peer
     * @return Result<void> Success or connection error
     */
    Result<void> connect();

    /**
     * @brief Disconnect from peer
     */
    void disconnect();

    /**
     * @brief Check if connected
     */
    bool isConnected() const;

    /**
     * @brief Get current peer state
     */
    PeerState getState() const { return m_state; }

    // ═══════════════════════════════════════════════════════════════════════════
    // Message Handling
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Send BOLT message to peer
     * @param message Message to send
     * @return Result<void> Success or send error
     */
    Result<void> sendMessage(const BOLTMessage& message);

    /**
     * @brief Register message handler callback
     * @param type Message type to handle
     * @param handler Callback function
     */
    using MessageHandler = std::function<void(const BOLTMessage&)>;
    void registerMessageHandler(BOLTMessageType type, MessageHandler handler);

    // ═══════════════════════════════════════════════════════════════════════════
    // BOLT #1: Base Protocol Messages
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Send INIT message with feature bits
     * @param features Local feature bits
     * @return Result<void> Success or error
     */
    Result<void> sendInit(const FeatureBits& features);

    /**
     * @brief Send PING message
     * @param num_pong_bytes Number of bytes for PONG reply
     * @return Result<void> Success or error
     */
    Result<void> sendPing(uint16_t num_pong_bytes = 0);

    /**
     * @brief Send PONG message in response to PING
     * @param ignored Bytes to echo back (from PING)
     * @return Result<void> Success or error
     */
    Result<void> sendPong(const std::vector<uint8_t>& ignored);

    /**
     * @brief Send ERROR message
     * @param channel_id Channel ID (or zeros for general error)
     * @param data Error message
     * @return Result<void> Success or error
     */
    Result<void> sendError(const std::string& channel_id, const std::string& data);

    // ═══════════════════════════════════════════════════════════════════════════
    // BOLT #2: HTLC Messages
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Send UPDATE_ADD_HTLC message
     *
     * Adds an HTLC to a channel. This is the wire protocol message for initiating
     * a Lightning payment.
     *
     * @param channel_id Channel to add HTLC to (32 bytes)
     * @param htlc_id Unique HTLC identifier
     * @param amount_msats Payment amount in milliuna
     * @param payment_hash SHA256 hash of payment preimage (32 bytes)
     * @param cltv_expiry Absolute block height for HTLC timeout
     * @param onion_routing_packet Sphinx onion packet (1366 bytes)
     * @return Result<void> Success or send error
     */
    Result<void> sendUpdateAddHTLC(
        const std::vector<uint8_t>& channel_id,
        uint64_t htlc_id,
        uint64_t amount_msats,
        const std::vector<uint8_t>& payment_hash,
        uint32_t cltv_expiry,
        const std::vector<uint8_t>& onion_routing_packet
    );

    /**
     * @brief Send UPDATE_FULFILL_HTLC message
     *
     * Settles an HTLC by revealing the payment preimage.
     *
     * @param channel_id Channel containing HTLC (32 bytes)
     * @param htlc_id HTLC to settle
     * @param payment_preimage Payment preimage (32 bytes)
     * @return Result<void> Success or send error
     */
    Result<void> sendUpdateFulfillHTLC(
        const std::vector<uint8_t>& channel_id,
        uint64_t htlc_id,
        const std::vector<uint8_t>& payment_preimage
    );

    /**
     * @brief Send UPDATE_FAIL_HTLC message
     *
     * Fails an HTLC due to routing error or other issue.
     *
     * @param channel_id Channel containing HTLC (32 bytes)
     * @param htlc_id HTLC to fail
     * @param reason Failure reason (encrypted for return path)
     * @return Result<void> Success or send error
     */
    Result<void> sendUpdateFailHTLC(
        const std::vector<uint8_t>& channel_id,
        uint64_t htlc_id,
        const std::vector<uint8_t>& reason
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Peer Information
    // ═══════════════════════════════════════════════════════════════════════════

    std::string getNodeId() const { return m_node_id; }
    std::string getAddress() const { return m_address; }
    bool isInbound() const { return m_inbound; }
    FeatureBits getRemoteFeatures() const { return m_remote_features; }
    PeerStats getStats() const;

    /**
     * @brief Get peer information as JSON
     */
    din::Json toJson() const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    std::string m_node_id;          // Remote peer's public key (33-byte hex)
    std::string m_address;          // IP:port address
    bool m_inbound;                 // True if peer connected to us
    PeerState m_state;              // Current connection state
    ::lightning::ITimeOracle* m_time_oracle;  // Phase 8.5: Deterministic time (NOT owned)

    // Socket and networking
    int m_socket_fd;                // TCP socket file descriptor
    std::atomic<bool> m_running;    // Reader thread running flag
    std::thread m_reader_thread;    // Message reader thread
    std::thread m_writer_thread;    // Message writer thread

    // Feature negotiation
    FeatureBits m_local_features;   // Our advertised features
    FeatureBits m_remote_features;  // Peer's advertised features

    // Message handling
    mutable std::mutex m_handlers_mutex;
    std::map<BOLTMessageType, MessageHandler> m_message_handlers;

    // Message write queue
    mutable std::mutex m_write_mutex;
    std::condition_variable m_write_cv;
    std::queue<BOLTMessage> m_write_queue;

    // Statistics
    mutable std::mutex m_stats_mutex;
    PeerStats m_stats;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Reader thread - receives messages from peer
     */
    void readerLoop();

    /**
     * @brief Writer thread - sends queued messages to peer
     */
    void writerLoop();

    /**
     * @brief Handle received INIT message
     */
    void handleInit(const BOLTMessage& message);

    /**
     * @brief Handle received PING message
     */
    void handlePing(const BOLTMessage& message);

    /**
     * @brief Handle received PONG message
     */
    void handlePong(const BOLTMessage& message);

    /**
     * @brief Handle received ERROR message
     */
    void handleError(const BOLTMessage& message);

    /**
     * @brief Parse IP:port address
     * @return std::pair<std::string, uint16_t> (ip, port)
     */
    std::pair<std::string, uint16_t> parseAddress(const std::string& address) const;

    /**
     * @brief Transition to new state
     */
    void transitionState(PeerState new_state);
};

// ═══════════════════════════════════════════════════════════════════════════
// PeerManager - Manages all Lightning peer connections
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @class PeerManager
 * @brief Manages all Lightning Network peer connections
 *
 * Responsibilities:
 * - Accept incoming peer connections (listen on port 9735)
 * - Initiate outgoing connections to known peers
 * - Maintain peer connection health (ping/pong)
 * - Route messages to appropriate handlers (ChannelManager, etc.)
 * - Persist peer information to database
 *
 * Thread Safety: All public methods are thread-safe
 */
class PeerManager {
public:
    /**
     * @brief Construct PeerManager
     * @param db Database for peer persistence (ILightningDB interface)
     * @param listen_port Port to listen on (default: 9735)
     */
    PeerManager(
        std::shared_ptr<ILightningDB> db,
        uint16_t listen_port = 9735
    );
    ~PeerManager();

    // ═══════════════════════════════════════════════════════════════════════════
    // Lifecycle
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Start peer manager (listen for connections, load peers)
     * @return Result<void> Success or error
     */
    Result<void> start();

    /**
     * @brief Stop peer manager
     */
    void stop();

    /**
     * @brief Check if running
     */
    bool isRunning() const { return m_running; }

    // ═══════════════════════════════════════════════════════════════════════════
    // Peer Management
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Connect to peer
     * @param node_id Peer's public key (33-byte hex)
     * @param address IP:port address
     * @return Result<void> Success or connection error
     */
    Result<void> connectToPeer(const std::string& node_id, const std::string& address);

    /**
     * @brief Disconnect from peer
     * @param node_id Peer's public key
     */
    void disconnectPeer(const std::string& node_id);

    /**
     * @brief Get peer by node ID
     * @param node_id Peer's public key
     * @return std::shared_ptr<LightningPeer> Peer or nullptr if not found
     */
    std::shared_ptr<LightningPeer> getPeer(const std::string& node_id) const;

    /**
     * @brief List all connected peers
     * @return std::vector<std::string> Node IDs of connected peers
     */
    std::vector<std::string> listPeers() const;

    /**
     * @brief Get peer count
     */
    uint32_t getPeerCount() const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Message Broadcasting
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Broadcast message to all peers
     * @param message Message to broadcast
     */
    void broadcastMessage(const BOLTMessage& message);

    /**
     * @brief Send message to specific peer
     * @param node_id Peer's public key
     * @param message Message to send
     * @return Result<void> Success or error
     */
    Result<void> sendMessageToPeer(const std::string& node_id, const BOLTMessage& message);

    // ═══════════════════════════════════════════════════════════════════════════
    // Message Handler Registration
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Register global message handler
     *
     * Called for all messages of the specified type from any peer.
     * Used by ChannelManager, HTLCManager, etc. to handle channel operations.
     *
     * @param type Message type
     * @param handler Callback(peer_node_id, message)
     */
    using GlobalMessageHandler = std::function<void(const std::string&, const BOLTMessage&)>;
    void registerGlobalHandler(BOLTMessageType type, GlobalMessageHandler handler);

    // ═══════════════════════════════════════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get aggregated statistics
     * @return din::Json Statistics (peer count, messages, bytes, etc.)
     */
    din::Json getStats() const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    std::shared_ptr<ILightningDB> m_db;     // Peer persistence
    uint16_t m_listen_port;                 // TCP listen port (9735)
    int m_listen_socket;                    // TCP listen socket
    std::atomic<bool> m_running;            // Running flag

    // Peer storage (node_id → LightningPeer)
    mutable std::mutex m_peers_mutex;
    std::map<std::string, std::shared_ptr<LightningPeer>> m_peers;

    // Global message handlers
    mutable std::mutex m_global_handlers_mutex;
    std::map<BOLTMessageType, GlobalMessageHandler> m_global_handlers;

    // Background threads
    std::thread m_listener_thread;          // Accept incoming connections
    std::thread m_maintenance_thread;       // Peer health checks (ping/pong)

    // Statistics
    struct Stats {
        uint64_t total_peers_connected = 0;
        uint64_t total_messages_sent = 0;
        uint64_t total_messages_received = 0;
        uint64_t total_bytes_sent = 0;
        uint64_t total_bytes_received = 0;
    };
    mutable std::mutex m_stats_mutex;
    Stats m_stats;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Listener thread - accept incoming connections
     */
    void listenerLoop();

    /**
     * @brief Maintenance thread - health checks, reconnection
     */
    void maintenanceLoop();

    /**
     * @brief Handle incoming connection
     * @param client_socket Accepted socket
     * @param client_address Client address string
     */
    void handleIncomingConnection(int client_socket, const std::string& client_address);

    /**
     * @brief Dispatch message to registered handlers
     * @param peer_node_id Source peer node ID
     * @param message Received message
     */
    void dispatchMessage(const std::string& peer_node_id, const BOLTMessage& message);

    /**
     * @brief Load peers from database
     */
    void loadPeers();

    /**
     * @brief Save peer to database
     */
    void savePeer(const std::string& node_id, const std::string& address);
};

} // namespace lightning
} // namespace dinero
