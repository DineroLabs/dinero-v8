#pragma once

#include "economic_types.h"
#include <vector>
#include <map>
#include <optional>
#include <cstdint>

namespace dinero {
namespace economic {
namespace test {

/**
 * FeeEstimator - Policy-driven fee estimation for economic simulation
 *
 * Provides fee rate estimates for different confirmation targets based on
 * recent transaction history and policy configuration.
 *
 * This is a SIMPLIFIED estimator for Phase 6a. Unlike production fee estimators
 * (which use complex statistical models), this uses simple heuristics:
 * - Track recent confirmed transactions
 * - Compute median/percentile fee rates
 * - Return estimates for configured confirmation targets
 *
 * Pattern: Observable-facts-only
 * - Uses only confirmed transaction data (observable)
 * - No market prediction or miner behavior modeling
 * - Deterministic (same history → same estimates)
 */
class FeeEstimator {
public:
    /**
     * Create fee estimator with policy configuration
     */
    explicit FeeEstimator(const EconomicPolicy& policy);

    /**
     * Reset estimator to initial state
     */
    void reset();

    /**
     * Update estimator with newly confirmed block
     *
     * @param block_height Block height
     * @param confirmed_txs Transactions confirmed in block
     * @param timestamp Block timestamp
     */
    void processBlock(
        uint32_t block_height,
        const std::vector<MempoolEntry>& confirmed_txs,
        uint64_t timestamp
    );

    /**
     * Get fee estimate for confirmation target
     *
     * @param confirmation_target Number of blocks for confirmation
     * @return Estimated fee rate (sat/byte), or nullopt if insufficient data
     */
    std::optional<double> estimateFeeRate(uint32_t confirmation_target) const;

    /**
     * Get all current fee estimates for configured targets
     */
    std::vector<FeeEstimate> getAllEstimates() const;

    /**
     * Get minimum relay fee (policy-based, not estimated)
     */
    uint64_t getMinRelayFee() const {
        return policy_.min_relay_fee_una;
    }

    /**
     * Get dust threshold (policy-based, not estimated)
     */
    uint64_t getDustThreshold() const {
        return policy_.dust_threshold_una;
    }

    /**
     * Check if estimator has sufficient data for estimates
     */
    bool hasSufficientData() const;

    /**
     * Get number of blocks processed
     */
    uint32_t getBlockCount() const {
        return processed_blocks_;
    }

    /**
     * Get total transactions tracked
     */
    size_t getTxCount() const {
        return tracked_txs_.size();
    }

private:
    struct ConfirmedTx {
        TxID tx_id;
        uint32_t block_height;
        uint64_t fee_una;
        uint32_t tx_size_bytes;
        double fee_rate;  // sat/byte
        uint64_t timestamp;
    };

    EconomicPolicy policy_;

    // Confirmed transaction history (limited window)
    std::vector<ConfirmedTx> tracked_txs_;
    uint32_t processed_blocks_;

    // Maximum history to keep (blocks)
    static constexpr uint32_t MAX_HISTORY_BLOCKS = 1008;  // ~1 week at 10 min/block

    // Minimum transactions required for estimates
    static constexpr size_t MIN_TXS_FOR_ESTIMATE = 10;

    /**
     * Compute fee estimate for target using median fee rate
     */
    std::optional<double> computeMedianFeeRate(uint32_t confirmation_target) const;

    /**
     * Get transactions confirmed in recent N blocks
     */
    std::vector<ConfirmedTx> getRecentTxs(uint32_t block_count) const;

    /**
     * Prune old transaction history
     */
    void pruneOldHistory(uint32_t current_height);
};

} // namespace test
} // namespace economic
} // namespace dinero
