#pragma once

#include "lightning/lightning_types.h"
#include "din_json.h"
#include "result.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <functional>
#include <optional>

// Forward declarations
struct DaemonContext;
namespace CTransaction {};
namespace CBlock {};

namespace dinero {
namespace lightning {

// Forward declarations
class ChannelManager;
class LightningEventManager;

/**
 * @class WatchtowerClient
 * @brief Client-side watchtower for breach detection and penalty enforcement
 *
 * Phase 10: Watchtowers & Penalty Logic
 *
 * Responsibilities:
 * - Track revoked commitment transaction states
 * - Monitor blockchain for breach attempts (old commitment broadcast)
 * - Construct and broadcast justice transactions
 * - CPFP fee bumping for penalty enforcement
 * - Remote watchtower coordination (optional)
 *
 * BOLT #13 Features:
 * - Breach remedy database
 * - Justice transaction construction
 * - To-local output recovery (CSV timelock bypass via revocation key)
 * - To-remote output recovery (sweeping counterparty outputs)
 * - HTLC output recovery (sweeping with preimages or revocation keys)
 *
 * Taproot Integration:
 * - Revocation keypath spending (instant penalty, no script reveal)
 * - Keypath = P + hash(P || revocation_basepoint) * G
 * - Justice transactions spend via taproot keypath signature
 *
 * Thread Safety: All public methods are thread-safe
 */
class WatchtowerClient {
public:
    /**
     * @brief Construct WatchtowerClient
     * @param ctx Reference to DaemonContext for blockchain access
     * @param channel_mgr Reference to ChannelManager
     */
    WatchtowerClient(
        DaemonContext& ctx,
        ChannelManager& channel_mgr
    );
    ~WatchtowerClient();

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 10.1: Commitment State Tracking
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @struct RevokedCommitmentState
     * @brief Data needed to construct justice transaction for a revoked state
     */
    struct RevokedCommitmentState {
        std::string channel_id;
        uint64_t commitment_number;

        // Revoked commitment transaction (signed by counterparty)
        std::string commitment_txid;
        std::vector<uint8_t> commitment_tx;

        // Revocation secrets (received via revoke_and_ack)
        std::vector<uint8_t> per_commitment_secret;
        std::vector<uint8_t> revocation_pubkey;

        // Our keys for this commitment
        std::vector<uint8_t> delayed_payment_basepoint;
        std::vector<uint8_t> to_self_delay_pubkey;

        // Output indices in commitment tx
        uint32_t to_local_output_index;   // Counterparty's CSV-locked output
        uint32_t to_remote_output_index;  // Our immediate output
        std::vector<uint32_t> htlc_output_indices;

        // Amounts
        uint64_t to_local_amount_sat;
        uint64_t to_remote_amount_sat;
        std::map<uint32_t, uint64_t> htlc_amounts;  // output_index → amount

        // CSV delay (for to_local output)
        uint16_t to_self_delay;

        // Timestamp when revoked
        uint64_t revoked_at;
    };

    /**
     * @brief Store revoked commitment state
     *
     * Called when we send revoke_and_ack, making a previous commitment revoked.
     *
     * @param state Revoked commitment data
     * @return Result<void> Success or error
     */
    Result<void> addRevokedCommitment(const RevokedCommitmentState& state);

    /**
     * @brief Get all revoked states for a channel
     *
     * @param channel_id Channel to query
     * @return std::vector<RevokedCommitmentState> All revoked commitments
     */
    std::vector<RevokedCommitmentState> getRevokedCommitments(const std::string& channel_id) const;

    /**
     * @brief Check if a txid is a revoked commitment
     *
     * @param txid Transaction ID to check
     * @return std::optional<RevokedCommitmentState> State if revoked, nullopt otherwise
     */
    std::optional<RevokedCommitmentState> isRevokedCommitment(const std::string& txid) const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 10.2: Breach Detection
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Monitor new block for breach attempts
     *
     * Scans block for any revoked commitment transactions.
     *
     * @param block_hash Block to scan
     * @return Result<size_t> Number of breaches detected
     */
    Result<size_t> scanBlockForBreaches(const std::string& block_hash);

    /**
     * @brief Check if specific transaction is a breach
     *
     * @param txid Transaction ID to check
     * @return std::optional<RevokedCommitmentState> State if breach, nullopt otherwise
     */
    std::optional<RevokedCommitmentState> detectBreach(const std::string& txid);

    /**
     * @brief Register breach callback
     *
     * Called when a breach is detected.
     *
     * @param callback Function(channel_id, revoked_state)
     */
    using BreachCallback = std::function<void(const std::string&, const RevokedCommitmentState&)>;
    void registerBreachCallback(BreachCallback callback);

    /**
     * @brief Set event manager for real-time event streaming (Phase 14)
     * @param event_mgr Pointer to event manager (nullable)
     */
    void setEventManager(LightningEventManager* event_mgr);

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 10.3: Justice Transaction Construction
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @struct JusticeTransaction
     * @brief Penalty transaction that sweeps all outputs from revoked commitment
     */
    struct JusticeTransaction {
        std::vector<uint8_t> tx_bytes;
        std::string txid;

        // Inputs (from revoked commitment)
        struct Input {
            std::string prev_txid;
            uint32_t prev_vout;
            uint64_t amount_sat;
            std::string output_type;  // "to_local", "to_remote", "htlc"
        };
        std::vector<Input> inputs;

        // Single output (sweep all to our address)
        std::string destination_address;
        uint64_t total_recovered_sat;
        uint64_t fee_sat;

        // Fee bumping
        bool supports_cpfp;
        std::string cpfp_anchor_txid;  // If we add CPFP output
    };

    /**
     * @brief Construct justice transaction for breach
     *
     * Builds penalty tx that:
     * - Spends to_local output via revocation keypath
     * - Spends to_remote output (if exists)
     * - Spends all HTLC outputs via revocation keypaths
     * - Sweeps all funds to our recovery address
     *
     * @param revoked_state Revoked commitment state
     * @param destination_address Where to send recovered funds
     * @param fee_rate Fee rate in sat/vB
     * @return Result<JusticeTransaction> Constructed justice tx or error
     */
    Result<JusticeTransaction> constructJusticeTx(
        const RevokedCommitmentState& revoked_state,
        const std::string& destination_address,
        uint64_t fee_rate
    );

    /**
     * @brief Broadcast justice transaction
     *
     * @param justice_tx Justice transaction to broadcast
     * @return Result<std::string> Transaction ID if successful
     */
    Result<std::string> broadcastJusticeTx(const JusticeTransaction& justice_tx);

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 10.4: CPFP Fee Bumping
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Add CPFP fee bump to justice transaction
     *
     * If justice tx is not confirming fast enough, create child transaction
     * that spends its output with higher fee to pull parent into block.
     *
     * @param parent_txid Justice transaction to bump
     * @param additional_fee_rate Additional fee in sat/vB
     * @return Result<std::string> CPFP child txid or error
     */
    Result<std::string> cpfpBumpJusticeTx(
        const std::string& parent_txid,
        uint64_t additional_fee_rate
    );

    /**
     * @brief Check if justice transaction needs fee bumping
     *
     * Monitors mempool/confirmation status and recommends fee bump.
     *
     * @param justice_txid Justice transaction to monitor
     * @return bool True if CPFP bump is recommended
     */
    bool needsFeeBump(const std::string& justice_txid) const;

    // ═══════════════════════════════════════════════════════════════════════════
    // Phase 10.5: Watchtower Statistics & Management
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get watchtower statistics
     *
     * @return din::Json Stats (tracked_commitments, breaches_detected, etc.)
     */
    din::Json getWatchtowerStats() const;

    /**
     * @brief Start watchtower monitoring
     *
     * Begins blockchain monitoring for breaches.
     *
     * @return Result<void> Success or error
     */
    Result<void> startMonitoring();

    /**
     * @brief Stop watchtower monitoring
     */
    void stopMonitoring();

    /**
     * @brief Check if watchtower is running
     *
     * @return bool True if monitoring
     */
    bool isMonitoring() const;

    /**
     * @brief Prune old revoked states
     *
     * Remove revoked commitments older than retention_seconds.
     * Only prune if commitment tx is confirmed and buried deep enough.
     *
     * @param retention_seconds Age threshold (default 90 days)
     * @return size_t Number of states pruned
     */
    size_t pruneOldStates(uint64_t retention_seconds = 7776000);

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext& m_daemon_ctx;
    ChannelManager& m_channel_mgr;

    // Revoked commitment states (channel_id → list of revoked states)
    mutable std::mutex m_states_mutex;
    std::map<std::string, std::vector<RevokedCommitmentState>> m_revoked_states;

    // Fast lookup: txid → revoked state
    mutable std::mutex m_txid_index_mutex;
    std::map<std::string, RevokedCommitmentState> m_txid_to_state;

    // Justice transactions we've broadcast (txid → justice_tx)
    mutable std::mutex m_justice_mutex;
    std::map<std::string, JusticeTransaction> m_justice_txs;

    // Breach callbacks
    std::vector<BreachCallback> m_breach_callbacks;
    mutable std::mutex m_callbacks_mutex;

    // Event manager for Phase 14
    LightningEventManager* m_event_mgr;

    // Monitoring state
    std::atomic<bool> m_monitoring;

    // Statistics
    struct Stats {
        uint64_t revoked_commitments_tracked = 0;
        uint64_t breaches_detected = 0;
        uint64_t justice_txs_broadcast = 0;
        uint64_t total_recovered_sat = 0;
        uint64_t cpfp_bumps_performed = 0;
    };
    mutable Stats m_stats;

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Derive revocation private key from per_commitment_secret
     *
     * revocation_privkey = revocation_basepoint_secret +
     *                      SHA256(revocation_basepoint || per_commitment_point) * G
     *
     * @param revocation_basepoint_secret Our revocation base secret
     * @param per_commitment_point Counterparty's per-commitment point
     * @return std::vector<uint8_t> Revocation private key
     */
    std::vector<uint8_t> deriveRevocationPrivkey(
        const std::vector<uint8_t>& revocation_basepoint_secret,
        const std::vector<uint8_t>& per_commitment_point
    ) const;

    /**
     * @brief Construct taproot keypath spend for revocation
     *
     * Uses revocation_privkey to sign input spending to_local output.
     *
     * @param input_txid Input transaction ID
     * @param input_vout Input vout index
     * @param amount_sat Input amount
     * @param revocation_privkey Revocation private key
     * @return std::vector<uint8_t> Witness data for input
     */
    std::vector<uint8_t> constructTaprootRevocationSpend(
        const std::string& input_txid,
        uint32_t input_vout,
        uint64_t amount_sat,
        const std::vector<uint8_t>& revocation_privkey
    ) const;

    /**
     * @brief Calculate recommended fee for justice transaction
     *
     * Justice transactions should confirm ASAP, so use aggressive fee rate.
     *
     * @return uint64_t Recommended fee rate in sat/vB
     */
    uint64_t calculateJusticeFeeRate() const;

    /**
     * @brief Validate revoked commitment state
     *
     * @param state State to validate
     * @return bool True if valid
     */
    bool validateRevokedState(const RevokedCommitmentState& state) const;
};

} // namespace lightning
} // namespace dinero
