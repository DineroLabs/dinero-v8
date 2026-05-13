#pragma once

#include "lightning/lightning_types.h"
#include "wallet/transaction.h"
#include <vector>
#include <string>
#include <mutex>
#include <map>

// Forward declare DaemonContext
struct DaemonContext;

namespace dinero {
namespace lightning {

// Forward declaration
class LightningEventManager;

/**
 * @class LightningSweepManager
 * @brief Manages sweeping to_self outputs after force close CSV timelocks expire
 *
 * Phase 13.4: CSV Timelock Output Sweep
 *
 * Responsibilities:
 * - Track pending sweeps after force close
 * - Monitor block height for CSV expiry
 * - Build and broadcast sweep transactions
 * - Handle CSV-locked to_self outputs
 *
 * CSV Timelock Flow:
 * 1. Force close broadcasts commitment tx
 * 2. to_self output is CSV-locked for `to_self_delay` blocks
 * 3. After N blocks, output becomes spendable
 * 4. LightningSweepManager builds sweep tx and broadcasts
 * 5. Funds return to wallet
 *
 * Thread Safety: All public methods are thread-safe
 */
class LightningSweepManager {
public:
    /**
     * @brief Pending sweep information
     */
    struct PendingSweep {
        std::string channel_id;          // Channel ID
        std::string commitment_txid;     // Commitment tx ID
        uint32_t to_self_vout;           // Output index of to_self
        uint64_t amount_una;            // Amount in una
        uint32_t csv_delay;              // CSV delay in blocks
        uint64_t scheduled_height;       // Block height when sweep scheduled
        uint64_t expiry_height;          // Block height when CSV expires
        std::string sweep_destination;   // Wallet address to sweep to
        bool swept;                      // true if already swept

        PendingSweep()
            : to_self_vout(0),
              amount_una(0),
              csv_delay(0),
              scheduled_height(0),
              expiry_height(0),
              swept(false) {}
    };

    /**
     * @brief Construct LightningSweepManager
     * @param daemon_ctx Daemon context for blockchain access
     */
    explicit LightningSweepManager(DaemonContext& daemon_ctx);
    ~LightningSweepManager();

    /**
     * @brief Set event manager for real-time event streaming (Phase 14)
     * @param event_mgr Pointer to event manager (nullable)
     */
    void setEventManager(LightningEventManager* event_mgr);

    // ═══════════════════════════════════════════════════════════════════════════
    // Sweep Scheduling
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Schedule a sweep for a force-closed channel's to_self output
     *
     * Called by ChannelManager after force close broadcast.
     * Tracks the commitment txid, output index, amount, and CSV expiry height.
     *
     * @param channel_id Channel ID that was force-closed
     * @param commitment_txid Transaction ID of broadcast commitment tx
     * @param to_self_vout Output index of the to_self output
     * @param amount_una Amount in the to_self output (una)
     * @param csv_delay CSV delay in blocks
     * @param current_block_height Current blockchain height
     * @param sweep_destination Wallet address to send swept funds
     * @return Result<void> Success or error
     */
    Result<void> scheduleSweep(
        const std::string& channel_id,
        const std::string& commitment_txid,
        uint32_t to_self_vout,
        uint64_t amount_una,
        uint32_t csv_delay,
        uint64_t current_block_height,
        const std::string& sweep_destination
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Block Processing
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Process new block and check for CSV expiry
     *
     * Called by LightningService when a new block arrives.
     * Checks all pending sweeps to see if CSV timelock has expired.
     * If expired, builds and broadcasts sweep transaction.
     *
     * @param block_height New block height
     * @return Result<void> Success or error
     */
    Result<void> onNewBlock(uint64_t block_height);

    // ═══════════════════════════════════════════════════════════════════════════
    // Sweep Transaction Building
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Build sweep transaction for expired CSV output
     *
     * Creates a transaction spending the to_self output:
     * - Input: to_self output from commitment tx (CSV-locked)
     * - Output: sweep_destination address (minus fees)
     * - Sequence: Set to satisfy CSV requirement
     *
     * @param sweep Pending sweep to build transaction for
     * @return Result<Transaction> Sweep transaction or error
     */
    Result<Transaction> buildSweepTransaction(const PendingSweep& sweep);

    /**
     * @brief Broadcast sweep transaction to mempool
     *
     * @param sweep_tx Transaction to broadcast
     * @param channel_id Channel ID (for logging)
     * @return Result<void> Success or error
     */
    Result<void> broadcastSweepTransaction(
        const Transaction& sweep_tx,
        const std::string& channel_id
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Query Methods
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Get all pending sweeps
     * @return std::vector<PendingSweep> List of pending sweeps
     */
    std::vector<PendingSweep> getPendingSweeps() const;

    /**
     * @brief Check if a channel has a pending sweep
     * @param channel_id Channel ID to check
     * @return bool true if sweep is pending
     */
    bool hasPendingSweep(const std::string& channel_id) const;

private:
    // ═══════════════════════════════════════════════════════════════════════════
    // Internal State
    // ═══════════════════════════════════════════════════════════════════════════

    DaemonContext& m_daemon_ctx;    // Blockchain context
    LightningEventManager* m_event_mgr;  // Event manager for Phase 14

    // Map: channel_id -> PendingSweep
    std::map<std::string, PendingSweep> m_pending_sweeps;

    mutable std::mutex m_mutex;   // Protects m_pending_sweeps

    // ═══════════════════════════════════════════════════════════════════════════
    // Internal Helpers
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Calculate fee for sweep transaction
     * @param input_amount Input amount in una
     * @return uint64_t Fee in una (0.1% of input)
     */
    uint64_t calculateSweepFee(uint64_t input_amount) const;

    /**
     * @brief Check if commitment tx is confirmed on-chain
     * @param txid Transaction ID to check
     * @param min_confirmations Minimum confirmations required
     * @return bool true if confirmed
     */
    bool isCommitmentTxConfirmed(
        const std::string& txid,
        uint32_t min_confirmations = 1
    ) const;
};

} // namespace lightning
} // namespace dinero
