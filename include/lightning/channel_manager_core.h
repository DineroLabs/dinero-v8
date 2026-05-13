#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning Channel Manager Core (Pure L2 State Machine)
// ═══════════════════════════════════════════════════════════════════════════
// Pure L2 logic - answers "what can happen" without knowing "how it happens on this node".
//
// ARCHITECTURE - ZERO L1 DEPENDENCIES:
// - NO daemon/chainstate/mempool/wallet/RocksDB headers
// - Only includes: STL + lightning interfaces (oracles, DB interface, types)
// - Communicates with L1 ONLY through oracle interfaces
// - NO logging (pure state machine)
//
// This file must compile with ONLY:
// - Standard library
// - lightning/lightning_types.h (enums, constants)
// - lightning/lightning_db_types.h (plain structs)
// - lightning/lightning_db_interface.h (ILightningDB)
// - lightning/chain_oracle.h (IChainOracle)
// - lightning/wallet_oracle.h (IWalletOracle)
// - lightning/funding_service.h (IFundingService)
// - common/status.h (Status enum)
//
// PROOF OF L2 SEPARATION: This compiles WITHOUT linking RocksDB, wallet, daemon.
// ═══════════════════════════════════════════════════════════════════════════

#include "lightning/lightning_types.h"
#include "lightning/lightning_db_types.h"
#include "lightning/lightning_db_interface.h"
#include "lightning/db_transaction.h"  // Full IDBTransaction definition
#include "lightning/chain_oracle.h"
#include "lightning/wallet_oracle.h"
#include "lightning/funding_service.h"
#include "lightning/htlc_sweep_oracle.h"
#include "lightning/justice_oracle.h"
#include "lightning/time_oracle.h"  // Phase 8.5: Deterministic time
#include "common/status.h"
#include <memory>
#include <string>
#include <vector>
#include <optional>

namespace dinero {
namespace lightning {

// Note: Result<T> is defined in lightning_db_interface.h (avoid duplication)

/**
 * Result of opening a channel (includes both L2 state and L1 TX data)
 */
struct OpenChannelResult {
    ChannelRecord channel;        // L2: Channel state (persisted)
    std::string funding_tx_hex;   // L1: Funding transaction (for broadcasting)
};

/**
 * @class ChannelManagerCore
 * @brief Pure L2 state machine for Lightning channel management
 *
 * This is the CORE - the "what is allowed to happen" logic.
 * Runtime glue (the "how it happens on this node") lives elsewhere.
 *
 * Key principles:
 * - Stateless computation (all state in database)
 * - Deterministic logic (same inputs → same outputs)
 * - No side effects (no logging, no I/O except through oracles)
 * - Oracle-driven (queries L1 through interfaces, never directly)
 */
class ChannelManagerCore {
public:
    /**
     * Constructor
     * @param chain_oracle Interface for blockchain queries
     * @param wallet_oracle Interface for wallet queries
     * @param funding_service Interface for funding transaction creation
     * @param sweep_oracle Interface for HTLC sweep transaction building (Phase 7B)
     * @param justice_oracle Interface for justice transaction building (Phase 7C)
     * @param db Interface for state persistence
     * @param node_pubkey Node's public key for channel operations
     * @param time_oracle Interface for deterministic time (Phase 8.5)
     */
    ChannelManagerCore(
        std::shared_ptr<::lightning::IChainOracle> chain_oracle,
        std::shared_ptr<::lightning::IWalletOracle> wallet_oracle,
        std::shared_ptr<::lightning::IFundingService> funding_service,
        std::shared_ptr<::lightning::IHTLCSweepOracle> sweep_oracle,
        std::shared_ptr<::lightning::IJusticeOracle> justice_oracle,
        std::shared_ptr<ILightningDB> db,
        const std::string& node_pubkey,  // Node's public key for channel operations
        ::lightning::ITimeOracle* time_oracle  // Phase 8.5: Deterministic time
    );

    ~ChannelManagerCore() = default;

    // Disable copy and move
    ChannelManagerCore(const ChannelManagerCore&) = delete;
    ChannelManagerCore& operator=(const ChannelManagerCore&) = delete;
    ChannelManagerCore(ChannelManagerCore&&) = delete;
    ChannelManagerCore& operator=(ChannelManagerCore&&) = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // Channel Lifecycle Operations
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Open a new Lightning channel
     * @param peer_node_id Remote peer's node pubkey (33-byte hex)
     * @param local_amount_sats Our funding amount in una
     * @param push_amount_sats Amount to push to remote (0 for no push)
     * @return OpenChannelResult (channel state + funding TX hex) if successful, error otherwise
     */
    Result<OpenChannelResult> openChannel(
        const std::string& peer_node_id,
        uint64_t local_amount_sats,
        uint64_t push_amount_sats = 0
    );

    /**
     * Close a channel (cooperative or force)
     * @param channel_id Channel ID (32-byte hex)
     * @param force true for unilateral close, false for cooperative
     * @return Result indicating success or failure
     */
    Result<void> closeChannel(
        const std::string& channel_id,
        bool force = false
    );

    /**
     * List all channels
     * @return Vector of all channel records
     */
    std::vector<ChannelRecord> listChannels();

    /**
     * Get specific channel by ID
     * @param channel_id Channel ID (32-byte hex)
     * @return Channel record if found, std::nullopt otherwise
     */
    std::optional<ChannelRecord> getChannel(const std::string& channel_id);

    // ═══════════════════════════════════════════════════════════════════════
    // Crash Recovery (Phase 6: Restart Safety)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Recovery result statistics
     */
    struct RecoveryResult {
        uint64_t channels_loaded;      // Successfully loaded channels
        uint64_t channels_corrupted;   // Channels with validation errors
        std::vector<std::string> errors; // Detailed error messages
    };

    /**
     * Restore state from database after restart/crash
     * Loads all channels, validates state consistency, reports corruption
     * @return RecoveryResult with statistics
     */
    RecoveryResult restoreFromDB();

    // ═══════════════════════════════════════════════════════════════════════
    // Block Processing (L1 → L2 Notifications)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Process new blockchain block
     * Updates channel states based on on-chain confirmations
     * @param block_height Height of the new block
     * @param block_hash Hash of the new block (hex)
     * @return Result indicating success or failure
     */
    Result<void> onNewBlock(uint64_t block_height, const std::string& block_hash);

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 9: Watchtower / Chain Event Integration
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Process transaction confirmation event from watchtower or chain observer
     *
     * Called when a transaction is confirmed on-chain at a specific height.
     * This is the ONLY entry point through which Lightning learns:
     * "A transaction with txid X was confirmed at height H."
     *
     * Phase 9 Architecture:
     * - WatchtowerService scans blocks → emits TransactionConfirmedEvent
     * - IPC delivers event → lightningd → ChannelManagerCore
     * - This method decides what the transaction means
     *
     * Phase 7C Integration:
     * - Checks if txid is a revoked commitment (breach)
     * - Creates justice transaction if appropriate
     *
     * Phase 7B Integration:
     * - Checks if txid is a pending sweep transaction
     * - Updates sweep status to confirmed
     *
     * Invariants:
     * - Idempotent: Same txid processed twice → same state
     * - Deterministic: Same inputs → same outputs
     * - Atomic: All state changes committed together
     *
     * @param txid Transaction ID (hex string)
     * @param confirmed_height Block height where transaction was confirmed
     */
    void onTransactionConfirmed(
        const std::string& txid,
        uint64_t confirmed_height
    );

    // ═══════════════════════════════════════════════════════════════════════
    // On-Chain Event Processing (Phase 7A: Force-Close Detection)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Process on-chain event notification from L1
     *
     * Called when L1 detects:
     * - An outpoint is spent (funding output, HTLC output, etc.)
     * - A transaction is confirmed in a block
     *
     * Lightning L2 does NOT scan chainstate. Instead, L1 delivers events.
     * L2 reacts to classify channel closes and trigger recovery.
     *
     * Phase 7A Responsibilities:
     * - Detect force-close by matching commitment txids
     * - Classify close type (LOCAL/REMOTE/UNKNOWN)
     * - Update terminal state
     * - Freeze HTLC set
     *
     * Invariants:
     * - Idempotent: Same event processed twice → same state
     * - Order-independent: Event arrival order doesn't affect final state
     * - No resurrection: Terminal states are immutable
     *
     * @param event On-chain event from L1 oracle
     * @return Result indicating success or failure
     */
    Result<void> onChainEvent(const OnChainEvent& event);

    // ═══════════════════════════════════════════════════════════════════════
    // HTLC Sweep Management (Phase 7B: Recovery After Force-Close)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Identify HTLCs that need sweeping after force-close
     *
     * Called after a channel transitions to terminal state (Phase 7A).
     * Analyzes HTLCs and creates sweep records for recoverable funds.
     *
     * Sweep candidates:
     * - Outgoing HTLCs (is_incoming=false) → TIMEOUT sweep after CLTV expiry
     * - Incoming HTLCs (is_incoming=true) with preimage → SUCCESS sweep after CSV delay
     *
     * @param channel Channel that was force-closed (with terminal state set)
     * @return Vector of sweep records created
     */
    std::vector<HTLCSweepRecord> identifySweepCandidates(const ChannelRecord& channel);

    /**
     * Get all pending sweeps that are ready to execute
     *
     * Returns sweeps where timing constraints (CSV/CLTV) are satisfied
     * at the current block height.
     *
     * @param current_height Current blockchain height
     * @return Vector of sweep records ready for execution
     */
    std::vector<HTLCSweepRecord> getReadySweeps(uint64_t current_height);

    /**
     * Update sweep status after broadcast/confirmation
     *
     * Called by L1 layer after sweep transaction is broadcast or confirmed.
     *
     * @param sweep_id Sweep identifier
     * @param status New status
     * @param sweep_txid Transaction ID (for BROADCAST/CONFIRMED status)
     * @param confirmed_height Block height (for CONFIRMED status, 0 otherwise)
     * @return Result indicating success or failure
     */
    Result<void> updateSweepStatus(
        const std::string& sweep_id,
        HTLCSweepStatus status,
        const std::string& sweep_txid = "",
        uint64_t confirmed_height = 0
    );

    // ═══════════════════════════════════════════════════════════════════════
    // Justice Management (Phase 7C: Breach Detection & Punishment)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Check if a commitment transaction is revoked
     *
     * Called during force-close detection (Phase 7A) to determine if the
     * remote commitment that closed the channel was a breach.
     *
     * @param channel Channel being checked
     * @param commitment_txid Commitment transaction ID that was broadcast
     * @return true if commitment is revoked (breach detected), false otherwise
     */
    bool isRevokedCommitment(
        const ChannelRecord& channel,
        const std::string& commitment_txid
    ) const;

    /**
     * Create justice record after breach detection
     *
     * Called when a revoked commitment is detected on-chain.
     * Creates justice intent with CSV maturity constraints.
     *
     * @param channel Channel where breach occurred
     * @param commitment_txid Revoked commitment transaction ID
     * @param breach_height Block height where breach was detected
     * @return Justice record
     */
    JusticeRecord createJusticeRecord(
        const ChannelRecord& channel,
        const std::string& commitment_txid,
        uint64_t breach_height
    ) const;

    /**
     * Get all pending justice actions that are ready to execute
     *
     * Returns justice records where CSV delay has expired at the current
     * block height.
     *
     * @param current_height Current blockchain height
     * @return Vector of justice records ready for execution
     */
    std::vector<JusticeRecord> getReadyJusticeActions(uint64_t current_height);

    /**
     * Update justice status after broadcast/confirmation
     *
     * Called by L1 layer after justice transaction is broadcast or confirmed.
     *
     * @param justice_id Justice identifier
     * @param status New status
     * @param justice_txid Transaction ID (for BROADCAST/CONFIRMED status)
     * @param confirmed_height Block height (for CONFIRMED status, 0 otherwise)
     * @return Result indicating success or failure
     */
    Result<void> updateJusticeStatus(
        const std::string& justice_id,
        JusticeStatus status,
        const std::string& justice_txid = "",
        uint64_t confirmed_height = 0
    );

private:
    // Oracle interfaces (L1 → L2 communication)
    std::shared_ptr<::lightning::IChainOracle> m_chain_oracle;
    std::shared_ptr<::lightning::IWalletOracle> m_wallet_oracle;
    std::shared_ptr<::lightning::IFundingService> m_funding_service;
    std::shared_ptr<::lightning::IHTLCSweepOracle> m_sweep_oracle; // Phase 7B
    std::shared_ptr<::lightning::IJusticeOracle> m_justice_oracle; // Phase 7C
    ::lightning::ITimeOracle* m_time_oracle;  // Phase 8.5: Deterministic time (NOT owned)

    // Database interface (L2 state persistence)
    std::shared_ptr<ILightningDB> m_db;

    // Node identity
    std::string m_node_pubkey;  // This node's public key (33-byte hex)

    // ═══════════════════════════════════════════════════════════════════════
    // Persistence Helpers (Phase 6: Atomic State Persistence)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Atomically persist channel state
     * Uses transactions to ensure all-or-nothing persistence
     * @param channel Channel record to persist
     * @param htlcs Associated HTLCs (optional, for future use)
     * @return Status::Ok if persisted successfully
     */
    Status persistChannelState(
        const ChannelRecord& channel,
        const std::vector<HTLCRecord>& htlcs = {}
    );

    /**
     * Validation result for recovery
     */
    struct ValidationResult {
        bool is_valid;
        std::string error;
    };

    /**
     * Validate channel state consistency
     * Checks: required fields, balance invariants, state transitions
     * @param channel Channel to validate
     * @return ValidationResult with status and error message (if invalid)
     */
    ValidationResult validateChannel(const ChannelRecord& channel) const;

    // ═══════════════════════════════════════════════════════════════════════
    // Force-Close Detection Helpers (Phase 7A)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Classify channel close based on spending transaction
     * Matches spending txid against known commitment transactions
     * @param channel Channel being closed
     * @param spending_txid Transaction that spent funding output
     * @return Terminal state classification
     */
    uint32_t classifyChannelClose(
        const ChannelRecord& channel,
        const std::string& spending_txid
    ) const;

    /**
     * Handle funding outpoint spent event
     * Detects channel close and classifies type
     * @param channel Channel whose funding was spent
     * @param spending_txid Transaction that spent funding output
     * @param block_height Block where spend occurred
     * @return Status::Ok if handled successfully
     */
    Status handleFundingSpent(
        ChannelRecord& channel,
        const std::string& spending_txid,
        uint64_t block_height
    );

    // ═══════════════════════════════════════════════════════════════════════
    // HTLC Sweep Helpers (Phase 7B)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Create sweep record for an HTLC
     * Determines sweep type and calculates timing constraints
     * @param channel Channel containing the HTLC
     * @param htlc HTLC to create sweep for
     * @param preimage Payment preimage (for SUCCESS sweeps, empty for TIMEOUT)
     * @return Sweep record
     */
    HTLCSweepRecord createSweepRecord(
        const ChannelRecord& channel,
        const HTLCRecord& htlc,
        const std::string& preimage = ""
    ) const;

    /**
     * Calculate earliest height when sweep is valid
     * Considers CSV delay and CLTV expiry constraints
     * @param channel Channel containing the HTLC
     * @param htlc HTLC being swept
     * @param sweep_type Sweep type (TIMEOUT/SUCCESS)
     * @return Earliest block height when sweep TX can be broadcast
     */
    uint64_t calculateEarliestSweepHeight(
        const ChannelRecord& channel,
        const HTLCRecord& htlc,
        HTLCSweepType sweep_type
    ) const;

    // Internal helpers
    std::string generateChannelId(const std::string& funding_txid, uint32_t vout) const;
    uint64_t getCurrentTimestamp() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 9: Transaction Confirmation Helpers
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * @brief Create justice transaction for revoked commitment (Phase 7C)
     *
     * Called when a revoked commitment is detected on-chain.
     * Attempts to create and broadcast justice transaction to claim all funds.
     *
     * @param revoked Revoked commitment record
     * @param confirmed_height Block height where breach was confirmed
     */
    void maybeCreateJustice(
        const RevokedCommitmentRecord& revoked,
        uint64_t confirmed_height
    );

    /**
     * @brief Update sweep status to confirmed (Phase 7B)
     *
     * Called when a sweep transaction is confirmed on-chain.
     * Updates sweep record from BROADCAST → CONFIRMED.
     *
     * @param sweep Sweep record to update
     * @param confirmed_height Block height where sweep was confirmed
     */
    void updateSweepConfirmed(
        const HTLCSweepRecord& sweep,
        uint64_t confirmed_height
    );

    /**
     * @brief Update channel state based on funding/commitment tx confirmation
     *
     * Called when a channel's funding or commitment transaction is confirmed.
     * Handles state transitions (PENDING_OPEN → OPEN, etc.).
     *
     * @param channel Channel to update
     * @param confirmed_height Block height where transaction was confirmed
     */
    void updateChannelConfirmation(
        const ChannelRecord& channel,
        uint64_t confirmed_height
    );
};

} // namespace lightning
} // namespace dinero
