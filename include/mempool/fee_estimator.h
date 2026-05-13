#pragma once

#include "primitives/uint256.h"  // Phase M.1.A: uint256-based transaction identity
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <cstdint>

namespace dinero {

/**
 * @brief Fee Estimation - Bitcoin Core Conservative Approach
 *
 * v0.13.0.3 - Minimal but real fee estimation
 *
 * Design Constraints:
 * - No ML, no magic, no guesses
 * - Track real confirmation outcomes
 * - Bucket-based aggregation
 * - Return "insufficient data" when honest
 *
 * Tracks per transaction:
 * - txid (identifier)
 * - feerate (una per byte)
 * - entry_height (when entered mempool)
 * - confirmation_height (when confirmed, or 0 if evicted)
 *
 * Buckets by confirmation target:
 * - Fast: 1-2 blocks
 * - Medium: 3-6 blocks
 * - Slow: 6-12 blocks
 *
 * RPC Interface:
 * - estimatesmartfee <nblocks>
 * - Returns feerate or "insufficient data"
 */
class FeeEstimator {
public:
    FeeEstimator();
    ~FeeEstimator() = default;

    // ═══════════════════════════════════════════════════════════════════════
    // Event Tracking (called by mempool)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Record transaction entry into mempool
     * @param txid Transaction ID (Phase M.1.A: uint256)
     * @param feerate Fee rate in una per byte
     * @param entry_height Block height when entered mempool
     */
    void recordTxEntry(const uint256& txid, double feerate, uint32_t entry_height);

    /**
     * Record transaction confirmation
     * @param txid Transaction ID (Phase M.1.A: uint256)
     * @param confirmation_height Block height when confirmed
     */
    void recordTxConfirmation(const uint256& txid, uint32_t confirmation_height);

    /**
     * Record transaction eviction (dropped from mempool)
     * @param txid Transaction ID (Phase M.1.A: uint256)
     */
    void recordTxEviction(const uint256& txid);

    // ═══════════════════════════════════════════════════════════════════════
    // Fee Estimation (RPC interface)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Estimate feerate for confirmation within target blocks
     *
     * @param target_blocks Desired confirmation time (1-12 blocks supported)
     * @return Estimated feerate in una/byte, or nullopt if insufficient data
     *
     * Targets:
     * - 1-2 blocks: Fast confirmation
     * - 3-6 blocks: Medium confirmation
     * - 6-12 blocks: Slow confirmation
     *
     * Returns nullopt if:
     * - Insufficient historical data
     * - Target blocks out of range
     * - No confirmations observed yet
     */
    std::optional<double> estimateFee(uint32_t target_blocks) const;

    /**
     * Get statistics for debugging/monitoring
     */
    struct Stats {
        size_t tracked_txs;           // Currently tracked transactions
        size_t confirmed_txs;          // Total confirmed transactions
        size_t evicted_txs;            // Total evicted transactions
        size_t fast_samples;           // Samples in fast bucket (1-2 blocks)
        size_t medium_samples;         // Samples in medium bucket (3-6 blocks)
        size_t slow_samples;           // Samples in slow bucket (6-12 blocks)
    };
    Stats getStats() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Persistence (optional - for future milestone)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Clear all tracked data (for testing or reset)
     */
    void clear();

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Internal Data Structures
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Tracked transaction entry (Phase M.1.A: uint256-based identity)
     */
    struct TrackedTx {
        uint256 txid;
        double feerate;              // Una per byte
        uint32_t entry_height;       // Block height when entered mempool
        uint32_t confirmation_height; // Block height when confirmed (0 = pending)

        TrackedTx() : feerate(0.0), entry_height(0), confirmation_height(0) {}
        TrackedTx(const uint256& id, double rate, uint32_t height)
            : txid(id), feerate(rate), entry_height(height), confirmation_height(0) {}
    };

    /**
     * Confirmation bucket
     */
    struct ConfirmationBucket {
        std::vector<double> feerates;  // All observed feerates in this bucket

        // Get median feerate (conservative estimate)
        std::optional<double> getMedianFeerate() const;

        // Get 75th percentile (more conservative)
        std::optional<double> get75thPercentileFeerate() const;
    };

    // Thread-safe access
    mutable std::mutex mutex_;

    // Currently tracked transactions (txid -> TrackedTx) - Phase M.1.A: uint256 keys
    std::unordered_map<uint256, TrackedTx> tracked_txs_;

    // Confirmation buckets by target
    // Key: blocks to confirmation
    // Value: bucket of feerates that confirmed in that time
    std::map<uint32_t, ConfirmationBucket> confirmation_buckets_;

    // Statistics
    size_t total_confirmed_ = 0;
    size_t total_evicted_ = 0;

    // Constants
    static constexpr uint32_t MIN_TARGET_BLOCKS = 1;
    static constexpr uint32_t MAX_TARGET_BLOCKS = 12;
    static constexpr size_t MIN_SAMPLES_FOR_ESTIMATE = 10;  // Need at least 10 samples

    // ═══════════════════════════════════════════════════════════════════════
    // Helper Methods
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Determine confirmation bucket based on blocks to confirmation
     * Returns bucket key (1, 2, 3, 6, or 12)
     */
    uint32_t getConfirmationBucket(uint32_t blocks_to_confirmation) const;

    /**
     * Get feerate estimate for a specific bucket
     */
    std::optional<double> getFeerateForBucket(uint32_t bucket_key) const;
};

} // namespace dinero
