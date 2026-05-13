#pragma once

#include "lightning/lightning_types.h"
#include "lightning/lightning_db_interface.h"
#include "lightning/lightning_db_types.h"
#include "lightning/onion_error.h"
#include "din_json.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>

// Forward declaration (DaemonContext is in global namespace)
struct DaemonContext;

// Forward declaration for time oracle (::lightning namespace)
namespace lightning {
    class ITimeOracle;
}

namespace dinero {
namespace lightning {

// Forward declarations
class ChannelManager;

/**
 * @class HTLCManager
 * @brief Manages Hashed Time-Locked Contracts for Lightning payments
 *
 * Phase 7.4: HTLC lifecycle and payment flow management
 *
 * Responsibilities:
 * - Create HTLCs for outgoing payments
 * - Accept HTLCs for incoming payments
 * - Settle HTLCs with preimages
 * - Timeout expired HTLCs
 * - Persist HTLC state to database
 * - Coordinate with ChannelManager for balance updates
 *
 * HTLC Lifecycle:
 *   PENDING → SETTLED (preimage revealed)
 *   PENDING → FAILED (routing error)
 *   PENDING → TIMED_OUT (CLTV expiry reached)
 *
 * Thread Safety: All public methods are thread-safe
 */
class HTLCManager {
public:
    /**
     * @brief Construct HTLCManager
     * @param ctx Reference to DaemonContext for block height access
     * @param db Database for HTLC persistence
     * @param channel_mgr Reference to ChannelManager for coordination
     * @param time_oracle Time oracle for deterministic timestamps (Phase 8.5)
     */
    HTLCManager(
        DaemonContext& ctx,
        std::shared_ptr<ILightningDB> db,
        ChannelManager& channel_mgr,
        ::lightning::ITimeOracle* time_oracle
    );
    ~HTLCManager();

    // ═══════════════════════════════════════════════════════════════════════════
    // HTLC Creation and Acceptance
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Create HTLC for outgoing payment
     *
     * Creates an outgoing HTLC and adds it to the specified channel.
     * The HTLC will be included in the next commitment transaction.
     *
     * @param channel_id Channel to add HTLC to
     * @param amount_msats Payment amount in milliuna
     * @param payment_hash SHA256 hash of preimage (32 bytes)
     * @param cltv_expiry Absolute block height for timeout
     * @param next_hop Next channel in payment route (or empty if final hop)
     * @return ::Result<HTLC> Created HTLC or error
     */
    ::Result<HTLC> createOutgoingHTLC(
        const std::string& channel_id,
        uint64_t amount_msats,
        const std::vector<uint8_t>& payment_hash,
        uint32_t cltv_expiry,
        const std::string& next_hop = ""
    );

    /**
     * @brief Accept incoming HTLC from peer
     *
     * Called when we receive an HTLC from a peer. Validates the HTLC
     * and adds it to the channel if acceptable.
     *
     * @param channel_id Channel receiving the HTLC
     * @param htlc_id Unique HTLC identifier from peer
     * @param amount_msats Payment amount
     * @param payment_hash Payment hash (32 bytes)
     * @param cltv_expiry Timeout block height
     * @param prev_hop Previous channel in route (or empty if we're the recipient)
     * @return ::Result<void> Success or error with rejection reason
     */
    ::Result<void> acceptIncomingHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        uint64_t amount_msats,
        const std::vector<uint8_t>& payment_hash,
        uint32_t cltv_expiry,
        const std::string& prev_hop = ""
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // HTLC Settlement
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Settle HTLC with preimage
     *
     * Settles an HTLC by revealing the payment preimage. This:
     * - Validates preimage matches payment_hash
     * - Updates channel balances
     * - Marks HTLC as SETTLED
     * - Propagates preimage back along route (if forwarding)
     *
     * @param channel_id Channel containing HTLC
     * @param htlc_id HTLC to settle
     * @param preimage Payment preimage (32 bytes)
     * @return ::Result<void> Success or error
     */
    ::Result<void> settleHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        const std::vector<uint8_t>& preimage
    );

    /**
     * @brief Fail HTLC (payment failed)
     *
     * Fails an HTLC due to routing error or other issue. This:
     * - Marks HTLC as FAILED
     * - Restores channel balances
     * - Propagates failure back along route
     *
     * @param channel_id Channel containing HTLC
     * @param htlc_id HTLC to fail
     * @param reason Failure reason (for routing feedback)
     * @return ::Result<void> Success or error
     */
    ::Result<void> failHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        const std::string& reason
    );

    /**
     * @brief Fail HTLC with BOLT #4 failure code
     *
     * Fails an HTLC using standardized BOLT #4 failure codes. Creates
     * an onion error packet that is propagated back along the route.
     *
     * @param channel_id Channel containing HTLC
     * @param htlc_id HTLC to fail
     * @param code BOLT #4 failure code
     * @param shared_secret Shared secret for error encryption (from ECDH)
     * @param channel_update Optional channel_update for UPDATE failures
     * @return ::Result<void> Success or error
     */
    ::Result<void> failHTLCWithCode(
        const std::string& channel_id,
        const std::string& htlc_id,
        FailureCode code,
        const std::array<uint8_t, 32>& shared_secret,
        const std::optional<std::vector<uint8_t>>& channel_update = std::nullopt
    );

    /**
     * @brief Timeout expired HTLCs
     *
     * Called on new blocks to check for HTLCs that have reached their
     * CLTV expiry without being settled. Timed-out HTLCs are failed
     * and balances are restored.
     *
     * @param block_height Current block height
     * @return ::Result<uint64_t> Number of HTLCs timed out, or error
     */
    ::Result<uint64_t> timeoutExpiredHTLCs(uint64_t block_height);

    // ═══════════════════════════════════════════════════════════════════════════
    // HTLC Queries
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get HTLC by ID
     * @param htlc_id HTLC identifier
     * @return std::optional<HTLC> HTLC if found, nullopt otherwise
     */
    std::optional<HTLC> getHTLC(const std::string& htlc_id) const;

    /**
     * @brief List all HTLCs for a channel
     * @param channel_id Channel ID
     * @param state_filter Optional state filter (e.g., only PENDING HTLCs)
     * @return std::vector<HTLC> HTLCs matching filter
     */
    std::vector<HTLC> listHTLCs(
        const std::string& channel_id,
        std::optional<HTLC::State> state_filter = std::nullopt
    ) const;

    /**
     * @brief Get HTLC statistics
     * @return din::Json Statistics (total HTLCs, amounts, states, etc.)
     */
    din::Json getStats() const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Payment Hash Tracking
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Register payment preimage
     *
     * Stores a payment preimage so we can claim HTLCs when they arrive.
     * Used for receiving payments.
     *
     * @param preimage Payment preimage (32 bytes)
     * @return ::Result<std::vector<uint8_t>> Payment hash (SHA256 of preimage) or error
     */
    ::Result<std::vector<uint8_t>> registerPreimage(const std::vector<uint8_t>& preimage);

    /**
     * @brief Check if we know the preimage for a payment hash
     *
     * @param payment_hash Payment hash (32 bytes)
     * @return std::optional<std::vector<uint8_t>> Preimage if known, nullopt otherwise
     */
    std::optional<std::vector<uint8_t>> getPreimage(const std::vector<uint8_t>& payment_hash) const;

    /**
     * @brief Get error packet for a failed HTLC
     * @param htlc_id HTLC ID to look up
     * @return OnionErrorPacket if found, empty optional otherwise
     */
    std::optional<OnionErrorPacket> getErrorPacket(const std::string& htlc_id) const;

    /**
     * @brief Store shared secrets for error decryption (Phase 5.1)
     *
     * Stores the SharedSecrets structure for an outgoing HTLC. These secrets
     * are needed to decrypt onion error packets if the payment fails.
     *
     * @param htlc_id HTLC ID to associate secrets with
     * @param secrets SharedSecrets structure (all hop keys)
     */
    void storeSharedSecrets(const std::string& htlc_id, const SharedSecrets& secrets);

    /**
     * @brief Get shared secrets for error decryption (Phase 5.1)
     * @param htlc_id HTLC ID to look up
     * @return SharedSecrets if found, empty optional otherwise
     */
    std::optional<SharedSecrets> getSharedSecrets(const std::string& htlc_id) const;

    /**
     * @brief Wait for HTLC settlement
     *
     * Blocks until the specified HTLC is settled, failed, or timed out.
     * Used for payment synchronization.
     *
     * @param htlc_id HTLC to wait for
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return ::Result<HTLC::State> Final state or error if timeout
     */
    ::Result<HTLC::State> waitForHTLCSettlement(
        const std::string& htlc_id,
        uint64_t timeout_ms = 0
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Onion Routing (BOLT #4 Integration)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Send payment with onion routing
     *
     * Initiates a payment using an onion packet. Creates an HTLC to the first hop
     * with the onion packet attached.
     *
     * @param channel_id Channel to first hop in route
     * @param amount_msats Payment amount in milliuna
     * @param payment_hash Payment hash (32 bytes)
     * @param cltv_expiry CLTV expiry for first hop
     * @param onion_packet Encrypted onion packet for the route
     * @return ::Result<std::string> HTLC ID or error
     */
    ::Result<std::string> sendPaymentWithOnion(
        const std::string& channel_id,
        uint64_t amount_msats,
        const std::vector<uint8_t>& payment_hash,
        uint32_t cltv_expiry,
        const std::vector<uint8_t>& onion_packet
    );

    /**
     * @brief Forward HTLC after peeling onion layer
     *
     * Called when we receive an HTLC with onion packet. Peels one layer,
     * extracts routing instructions, and forwards to next hop (or settles if final).
     *
     * @param channel_id Channel that received the HTLC
     * @param htlc_id Incoming HTLC ID
     * @param onion_packet Onion packet to peel
     * @param our_privkey Our node's private key for ECDH (32 bytes)
     * @return ::Result<void> Success or error
     */
    ::Result<void> forwardHTLC(
        const std::string& channel_id,
        const std::string& htlc_id,
        const std::vector<uint8_t>& onion_packet,
        const std::vector<uint8_t>& our_privkey
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Message Handlers (called by PeerManager on incoming BOLT messages)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Handle incoming UPDATE_ADD_HTLC message from peer
     *
     * Called when a peer sends us an HTLC. This method:
     * - Parses the BOLT #2 message payload
     * - Validates the HTLC parameters
     * - Calls acceptIncomingHTLC() to add it to the channel
     * - Calls forwardHTLC() to process the onion packet
     *
     * @param peer_node_id Node ID of sending peer
     * @param payload BOLT #2 message payload (32+8+8+32+4+1366 bytes)
     */
    void handleUpdateAddHTLC(const std::string& peer_node_id, const std::vector<uint8_t>& payload);

    // ═══════════════════════════════════════════════════════════════════════════
    // Callbacks
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief HTLC event callback type
     *
     * Called when HTLC state changes:
     * - SETTLED: Payment completed
     * - FAILED: Payment failed
     * - TIMED_OUT: HTLC expired
     */
    using HTLCEventCallback = std::function<void(const HTLC& htlc, HTLC::State old_state, HTLC::State new_state)>;

    /**
     * @brief Register callback for HTLC events
     * @param callback Function to call on HTLC state changes
     */
    void registerHTLCEventCallback(HTLCEventCallback callback);

    // ═══════════════════════════════════════════════════════════════════════════
    // Persistence
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Load HTLCs from database
     * @return ::Result<void> Success or error
     */
    ::Result<void> loadHTLCs();

    /**
     * @brief Save HTLC to database
     * @param htlc HTLC to persist
     * @return ::Result<void> Success or error
     */
    ::Result<void> saveHTLC(const HTLC& htlc);

    /**
     * @brief Delete HTLC from database
     * @param htlc_id HTLC to delete
     * @return ::Result<void> Success or error
     */
    ::Result<void> deleteHTLC(const std::string& htlc_id);

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext& m_daemon_ctx;                           // Block height access
    std::shared_ptr<ILightningDB> m_db;      // Persistence
    ChannelManager& m_channel_mgr;                         // Channel coordination
    ::lightning::ITimeOracle* m_time_oracle;               // Phase 8.5: Deterministic time (NOT owned)

    // HTLC storage (in-memory cache, backed by RocksDB cf_htlcs)
    mutable std::mutex m_htlcs_mutex;
    std::map<std::string, HTLC> m_htlcs;                   // htlc_id → HTLC

    // Preimage storage (for receiving payments)
    mutable std::mutex m_preimages_mutex;
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> m_preimages;  // payment_hash → preimage

    // Phase 8: Payment secret validation (BOLT #11 receiver-side security)
    mutable std::mutex m_payment_secrets_mutex;
    std::map<std::vector<uint8_t>, std::array<uint8_t, 32>> m_payment_secrets;  // payment_hash → payment_secret

    // Phase 8.4: MPP (Multi-Path Payment) tracking
    struct MPPPart {
        std::string htlc_id;
        uint64_t amount_muna;
        uint64_t received_at;
        bool settled;
    };
    struct MPPState {
        uint64_t total_amount_muna;     // Expected total from total_amount_muna TLV
        uint64_t received_amount_muna;  // Sum of parts received so far
        std::vector<MPPPart> parts;      // Individual HTLC parts
        uint64_t first_part_at;          // Timestamp of first part
        uint64_t timeout_seconds = 60;   // MPP assembly timeout (default: 60s)
    };
    mutable std::mutex m_mpp_state_mutex;
    std::map<std::vector<uint8_t>, MPPState> m_mpp_state;  // payment_hash → MPP state

    // Phase 8.5: Probing attack detection
    struct ProbeAttempt {
        std::string channel_id;
        uint64_t timestamp;
        bool had_payment_secret;
    };
    struct PaymentHashProbeHistory {
        std::vector<ProbeAttempt> attempts;
        uint64_t first_probe_at;
        uint32_t failure_count = 0;
    };
    mutable std::mutex m_probe_history_mutex;
    std::map<std::vector<uint8_t>, PaymentHashProbeHistory> m_probe_history;  // payment_hash → probe attempts

    // Rate limiting thresholds
    static constexpr uint32_t MAX_PROBE_ATTEMPTS_PER_HASH = 5;  // Max failed attempts per payment_hash
    static constexpr uint64_t PROBE_HISTORY_WINDOW_SECONDS = 300;  // 5 minute rolling window

    // HTLC linking for settlement propagation (forwarded payments)
    mutable std::mutex m_htlc_links_mutex;
    std::map<std::string, std::string> m_htlc_links;  // outgoing_htlc_id → incoming_htlc_id

    // Shared secrets for onion error packet decryption (BOLT #4 Phase 5.1)
    // Stores the full SharedSecrets structure (all hop keys) for sender-side error decryption
    mutable std::mutex m_shared_secrets_mutex;
    std::map<std::string, SharedSecrets> m_shared_secrets;  // htlc_id → SharedSecrets (all hops)

    // Onion error packets for failure propagation (BOLT #4)
    mutable std::mutex m_error_packets_mutex;
    std::map<std::string, OnionErrorPacket> m_error_packets;  // htlc_id → error_packet

    // Event callbacks
    std::vector<HTLCEventCallback> m_event_callbacks;
    mutable std::mutex m_callbacks_mutex;

    // Statistics
    struct Stats {
        uint64_t total_htlcs = 0;
        uint64_t pending_htlcs = 0;
        uint64_t settled_htlcs = 0;
        uint64_t failed_htlcs = 0;
        uint64_t timed_out_htlcs = 0;
        uint64_t total_amount_msats = 0;
        uint64_t settled_amount_msats = 0;
    };
    mutable Stats m_stats;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Generate unique HTLC ID
     * @return std::string 32-byte hex HTLC ID
     */
    std::string generateHTLCId() const;

    /**
     * @brief Validate HTLC parameters
     * @param htlc HTLC to validate
     * @return ::Result<void> Success or error with reason
     */
    ::Result<void> validateHTLC(const HTLC& htlc) const;

    /**
     * @brief Verify payment hash matches preimage
     * @param payment_hash Payment hash (32 bytes)
     * @param preimage Preimage (32 bytes)
     * @return bool True if SHA256(preimage) == payment_hash
     */
    bool verifyPreimage(
        const std::vector<uint8_t>& payment_hash,
        const std::vector<uint8_t>& preimage
    ) const;

    /**
     * @brief Update HTLC state and trigger callbacks
     * @param htlc_id HTLC to update
     * @param new_state New state
     */
    void updateHTLCState(const std::string& htlc_id, HTLC::State new_state);

    /**
     * @brief Propagate HTLC settlement upstream (for forwarded payments)
     * @param htlc Settled HTLC
     * @param preimage Payment preimage
     */
    void propagateSettlement(const HTLC& htlc, const std::vector<uint8_t>& preimage);

    /**
     * @brief Propagate HTLC failure upstream (for forwarded payments)
     * @param htlc Failed HTLC
     * @param reason Failure reason
     */
    void propagateFailure(const HTLC& htlc, const std::string& reason);

    /**
     * @brief Propagate HTLC failure upstream with onion error packet
     * @param htlc Failed HTLC
     * @param error_packet Onion error packet to propagate
     * @param shared_secret Our shared secret for wrapping the error
     */
    void propagateOnionFailure(
        const HTLC& htlc,
        const OnionErrorPacket& error_packet,
        const std::array<uint8_t, 32>& shared_secret
    );

    /**
     * @brief Update internal statistics
     */
    void updateStats();

    /**
     * @brief Convert HTLC to HTLCRecord for database storage
     * @param htlc HTLC to convert
     * @return HTLCRecord Typed record for MessagePack serialization
     */
    HTLCRecord htlcToHTLCRecord(const HTLC& htlc) const;

    /**
     * @brief Convert HTLCRecord back to HTLC
     * @param record HTLCRecord from database
     * @return HTLC Runtime HTLC object
     */
    HTLC htlcRecordToHTLC(const HTLCRecord& record) const;

    /**
     * @brief Compute SHA256 hash
     * @param data Input data
     * @return std::vector<uint8_t> 32-byte hash
     */
    std::vector<uint8_t> sha256(const std::vector<uint8_t>& data) const;
};

} // namespace lightning
} // namespace dinero
