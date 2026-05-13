#pragma once

#include "primitives/uint256.h"
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstdint>

namespace dinero {

/**
 * @brief Template delta tracker for incremental block template refresh
 *
 * Phase W.1.4: Tracks mempool changes since last template generation to enable
 * incremental refresh instead of full rebuild.
 *
 * Design Principles:
 * - Lightweight delta tracking (only changed txids)
 * - Fast refresh decision (O(1) checks)
 * - Automatic cleanup (stale entries removed)
 * - Thread-safe (can be called from mining threads)
 *
 * Usage:
 * 1. Snapshot current mempool state: `SnapshotMempool()`
 * 2. Track changes as they occur: `OnTransactionAdded()`, `OnTransactionRemoved()`
 * 3. Check if refresh needed: `ShouldRefreshTemplate()`
 * 4. Get delta statistics: `GetDeltaCount()`, `GetHighestAddedFeeRate()`
 */
class TemplateDeltaTracker {
public:
    /**
     * @brief Transaction entry with timestamp
     */
    struct TxEntry {
        uint256 txid;
        uint64_t fee_rate;         // sat/byte
        uint64_t entry_time_ms;    // When tx entered mempool
        bool in_template;          // Was in last template
    };

    TemplateDeltaTracker();

    /**
     * @brief Snapshot current mempool state
     *
     * Records the current mempool as the baseline for delta tracking.
     * Called after a template is successfully generated.
     *
     * @param txids Set of transaction IDs currently in template
     * @param snapshot_time_ms Timestamp of snapshot (milliseconds)
     */
    void SnapshotMempool(
        const std::unordered_set<uint256>& txids,
        uint64_t snapshot_time_ms
    );

    /**
     * @brief Record transaction added to mempool
     *
     * @param txid Transaction ID
     * @param fee_rate Fee rate in sat/byte
     * @param entry_time_ms When transaction entered mempool
     */
    void OnTransactionAdded(
        const uint256& txid,
        uint64_t fee_rate,
        uint64_t entry_time_ms
    );

    /**
     * @brief Record transaction removed from mempool
     *
     * @param txid Transaction ID
     */
    void OnTransactionRemoved(const uint256& txid);

    /**
     * @brief Should template be refreshed?
     *
     * Decision criteria:
     * - Delta count > threshold (default: 10 transactions)
     * - High-fee transaction added (fee_rate > 2× median)
     * - Time since last template > max_age (default: 30 seconds)
     * - Significant template transaction removed
     *
     * @param current_time_ms Current timestamp (milliseconds)
     * @param delta_threshold Minimum delta count to trigger refresh (default: 10)
     * @param max_age_ms Maximum template age before refresh (default: 30000ms)
     * @return True if template should be refreshed
     */
    bool ShouldRefreshTemplate(
        uint64_t current_time_ms,
        size_t delta_threshold = 10,
        uint64_t max_age_ms = 30000
    ) const;

    /**
     * @brief Get count of mempool changes since last template
     *
     * @return Number of added + removed transactions
     */
    size_t GetDeltaCount() const {
        return added_txs_.size() + removed_txs_.size();
    }

    /**
     * @brief Get highest fee rate among added transactions
     *
     * Used to detect high-priority transactions that warrant immediate refresh.
     *
     * @return Highest fee rate (sat/byte), or 0 if no additions
     */
    uint64_t GetHighestAddedFeeRate() const;

    /**
     * @brief Get refresh urgency score
     *
     * Combines multiple factors into single urgency metric:
     * - Delta count (10 txs = 0.2, 50 txs = 1.0)
     * - High-fee additions (multiplier up to 2×)
     * - Template age (30s = 0.5, 60s = 1.0)
     * - Removed template txs (penalty)
     *
     * Range: [0.0, 2.0+]
     * - 0.0: No urgency (template is fresh)
     * - 1.0: Moderate urgency (refresh recommended)
     * - 2.0+: High urgency (refresh strongly recommended)
     *
     * @param current_time_ms Current timestamp (milliseconds)
     * @param median_fee_rate Current mempool median fee rate (for high-fee detection)
     * @return Urgency score [0.0, 2.0+]
     */
    double GetRefreshUrgency(
        uint64_t current_time_ms,
        uint64_t median_fee_rate = 10
    ) const;

    /**
     * @brief Get added transactions since last template
     *
     * @return Set of added transaction IDs
     */
    const std::unordered_set<uint256>& GetAddedTransactions() const {
        return added_txs_;
    }

    /**
     * @brief Get removed transactions since last template
     *
     * @return Set of removed transaction IDs
     */
    const std::unordered_set<uint256>& GetRemovedTransactions() const {
        return removed_txs_;
    }

    /**
     * @brief Get timestamp of last template snapshot
     *
     * @return Timestamp in milliseconds, or 0 if never snapshotted
     */
    uint64_t GetLastSnapshotTime() const {
        return last_snapshot_time_ms_;
    }

    /**
     * @brief Clear all delta tracking data
     *
     * Resets to initial state (no snapshot, no deltas).
     */
    void Clear();

private:
    // Last template snapshot
    std::unordered_set<uint256> template_txids_;  // Txs in last template
    uint64_t last_snapshot_time_ms_;              // When snapshot was taken

    // Delta tracking
    std::unordered_set<uint256> added_txs_;       // Txs added since snapshot
    std::unordered_set<uint256> removed_txs_;     // Txs removed since snapshot

    // Metadata for added transactions
    std::unordered_map<uint256, uint64_t> added_fee_rates_;  // fee_rate per added tx

    // Statistics
    size_t removed_template_txs_;  // Count of removed txs that were in template
};

} // namespace dinero
