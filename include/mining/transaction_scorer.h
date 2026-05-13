#pragma once

#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "mining/block_assembly_context.h"
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace dinero {

// Forward declarations
class Mempool;

/**
 * @brief Fee histogram for mining probability calculation
 *
 * Phase W.1.2: Tracks fee rate distribution across mempool to calculate
 * percentile rankings for mining probability estimation.
 *
 * Design:
 * - Buckets: 0-1, 1-5, 5-10, 10-50, 50-100, 100-500, 500+ sat/byte
 * - Fast percentile calculation (no sorting)
 * - Lightweight update (O(1) per transaction)
 */
class FeeHistogram {
public:
    FeeHistogram();

    /**
     * @brief Add a transaction to the histogram
     *
     * @param fee_rate Fee rate in una per byte
     */
    void AddTransaction(uint64_t fee_rate);

    /**
     * @brief Remove a transaction from the histogram
     *
     * @param fee_rate Fee rate in una per byte
     */
    void RemoveTransaction(uint64_t fee_rate);

    /**
     * @brief Get percentile rank for a fee rate
     *
     * Returns the percentile (0.0-1.0) of this fee rate in the distribution.
     * - 0.0 = lowest fee
     * - 0.5 = median fee
     * - 1.0 = highest fee
     *
     * @param fee_rate Fee rate in una per vbyte
     * @return Percentile rank [0.0, 1.0]
     */
    double GetPercentile(uint64_t fee_rate) const;

    /**
     * @brief Get fee rate at a given percentile
     *
     * Returns the fee rate (una/vB) at the specified percentile.
     * - 0.0 = lowest fee in mempool
     * - 0.15 = 15th percentile (backlog threshold)
     * - 0.5 = median fee
     * - 1.0 = highest fee
     *
     * @param percentile Target percentile [0.0, 1.0]
     * @return Fee rate at that percentile (una/vB)
     */
    uint64_t GetFeeRateAtPercentile(double percentile) const;

    /**
     * @brief Get total transaction count
     */
    size_t GetTotalCount() const { return total_count_; }

    /**
     * @brief Alias for GetTotalCount (for compatibility)
     */
    size_t Count() const { return total_count_; }

    /**
     * @brief Clear all data
     */
    void Clear();

    /**
     * @brief Build histogram from mempool (one-time initialization)
     *
     * @param mempool Mempool to analyze
     */
    void BuildFromMempool(const Mempool* mempool);

private:
    // Fee rate buckets (sat/byte): [0-1), [1-5), [5-10), [10-50), [50-100), [100-500), [500+)
    static constexpr size_t NUM_BUCKETS = 7;
    static constexpr uint64_t BUCKET_BOUNDARIES[NUM_BUCKETS] = {1, 5, 10, 50, 100, 500, UINT64_MAX};

    size_t buckets_[NUM_BUCKETS];  // Count of txs in each bucket
    size_t total_count_;           // Total transactions

    /**
     * @brief Get bucket index for a fee rate
     */
    size_t GetBucketIndex(uint64_t fee_rate) const;
};

/**
 * @brief Transaction scorer for intelligent block assembly
 *
 * Phase W.1.2: Combines fee rate, mining probability, and compact
 * reconstructability into a single score for transaction prioritization.
 *
 * Scoring Formula:
 *   score = base_fee_rate × mining_probability × compact_reconstructability
 *
 * Where:
 * - base_fee_rate: Transaction fee per byte (sat/byte)
 * - mining_probability: Likelihood of being mined soon (0.0-1.0)
 *   - Calculated as: 0.5 × age_factor + 0.5 × fee_percentile
 * - compact_reconstructability: Likelihood peers have this tx (0.0-1.0)
 *   - Based on: propagation age + compact_friendly_bias
 *
 * Design Principles:
 * - Fee dominance preserved (high fee always beats low fee)
 * - Age bonus for well-propagated transactions (compact optimization)
 * - Adaptive bias based on compact success rate (from context)
 * - Deterministic (same inputs = same output)
 */
class TransactionScorer {
public:
    /**
     * @brief Scored transaction entry
     */
    struct ScoredTransaction {
        uint256 txid;
        double score;              // Combined score
        uint64_t fee_rate;         // Base fee rate (sat/byte)
        double mining_probability; // Mining likelihood [0.0, 1.0]
        double compact_recon;      // Compact reconstructability [0.0, 1.0]
        uint64_t entry_time_ms;    // When tx entered mempool

        // For sorting (descending by score)
        bool operator<(const ScoredTransaction& other) const {
            return score > other.score;  // Higher score = higher priority
        }
    };

    explicit TransactionScorer(const BlockAssemblyContext& context);

    /**
     * @brief Score a single transaction
     *
     * @param txid Transaction ID
     * @param fee_rate Fee rate in sat/byte
     * @param entry_time_ms When transaction entered mempool (milliseconds)
     * @param current_time_ms Current time (milliseconds)
     * @return Scored transaction
     */
    ScoredTransaction ScoreTransaction(
        const uint256& txid,
        uint64_t fee_rate,
        uint64_t entry_time_ms,
        uint64_t current_time_ms
    ) const;

    /**
     * @brief Score multiple transactions
     *
     * @param transactions Vector of (txid, fee_rate, entry_time) tuples
     * @param current_time_ms Current time (milliseconds)
     * @return Vector of scored transactions (sorted by score descending)
     */
    std::vector<ScoredTransaction> ScoreTransactions(
        const std::vector<std::tuple<uint256, uint64_t, uint64_t>>& transactions,
        uint64_t current_time_ms
    ) const;

    /**
     * @brief Set fee histogram for percentile calculation
     *
     * @param histogram Fee histogram (shared, non-owning)
     */
    void SetFeeHistogram(const FeeHistogram* histogram) {
        fee_histogram_ = histogram;
    }

private:
    const BlockAssemblyContext& context_;  // Network context (sync phase, compact success, etc.)
    const FeeHistogram* fee_histogram_;    // Fee distribution (optional)

    /**
     * @brief Calculate mining probability
     *
     * Hybrid formula: 0.5 × age_factor + 0.5 × fee_percentile
     *
     * Age factor:
     * - 0-60s:   age_factor = age_seconds / 300.0 (ramp up to 20% over 5 minutes)
     * - 60s-5m:  age_factor = 0.2 + (age_seconds - 60) / 240.0 × 0.8 (ramp to 100%)
     * - >5m:     age_factor = 1.0 (fully propagated)
     *
     * @param fee_rate Fee rate in sat/byte
     * @param age_seconds Age in seconds
     * @return Mining probability [0.0, 1.0]
     */
    double CalculateMiningProbability(uint64_t fee_rate, uint64_t age_seconds) const;

    /**
     * @brief Calculate compact reconstructability
     *
     * Factors:
     * - Propagation age (older = more peers have it)
     * - Compact friendly bias from context (adaptive)
     *
     * Formula:
     *   base = min(1.0, age_seconds / 300.0)  // 5 minutes = fully propagated
     *   bonus = context.GetCompactFriendlyBias() × base
     *   reconstructability = base + bonus
     *
     * @param age_seconds Age in seconds
     * @return Compact reconstructability [0.0, 1.0+]
     */
    double CalculateCompactReconstructability(uint64_t age_seconds) const;
};

} // namespace dinero
