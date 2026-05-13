#pragma once

#include "policy/fee_estimator.h"
#include "mempool/mempool.h"
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <chrono>
#include <string>

namespace dinero {
namespace policy {

/**
 * @file hybrid_fee_estimator.h
 * @brief Hybrid ML Fee Estimator (Phase 32)
 *
 * Combines EWMA-based historical tracking with ML trend prediction and
 * real-time mempool analysis for accurate fee estimation on small chains.
 *
 * Architecture:
 * 1. EWMA Base Layer - Existing fee estimator (exponentially weighted moving average)
 * 2. ML Trend Predictor - Simple linear regression for fee trends
 * 3. Mempool Analyzer - Real-time congestion and fee pressure analysis
 * 4. Historical Persistence - Store fee history to disk for better estimates
 * 5. Adaptive Fallbacks - Dynamic fallback rates based on recent activity
 *
 * This hybrid approach mitigates noise in small chains while providing
 * responsive estimates during network congestion.
 */

// ═══════════════════════════════════════════════════════════════════════════
// ML Trend Predictor
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Historical fee data point
 *
 * Represents a single observation of network fee rates at a point in time.
 */
struct FeeDataPoint {
    uint64_t timestamp;           // Unix timestamp (seconds)
    uint32_t block_height;        // Block height when observed
    double fee_rate;              // Fee rate (sat/vB)
    size_t confirmation_blocks;   // Target confirmation time
    double confidence;            // Estimate confidence [0.0, 1.0]

    FeeDataPoint()
        : timestamp(0), block_height(0), fee_rate(0.0)
        , confirmation_blocks(0), confidence(0.0)
    {}

    FeeDataPoint(uint64_t ts, uint32_t height, double rate, size_t conf, double conf_val)
        : timestamp(ts), block_height(height), fee_rate(rate)
        , confirmation_blocks(conf), confidence(conf_val)
    {}
};

/**
 * @brief Simple ML trend predictor using linear regression
 *
 * Tracks recent fee trends and predicts future fee rates.
 * Uses a sliding window of recent observations to fit a linear model.
 *
 * Model: fee_rate = intercept + slope * time
 *
 * This simple approach is sufficient for small chains and avoids
 * overfitting that complex ML models might suffer from with limited data.
 */
class MLTrendPredictor {
public:
    explicit MLTrendPredictor(size_t window_size = 100);

    /**
     * @brief Add new fee observation
     *
     * @param data_point Fee observation
     */
    void addObservation(const FeeDataPoint& data_point);

    /**
     * @brief Predict fee rate for future time
     *
     * Uses linear regression on recent observations to predict future fees.
     *
     * @param target_blocks Target confirmation time (blocks)
     * @param current_time Current timestamp (seconds)
     * @return Predicted fee rate (sat/vB), or 0.0 if insufficient data
     */
    double predictFeeRate(size_t target_blocks, uint64_t current_time) const;

    /**
     * @brief Get trend direction (upward, downward, stable)
     *
     * @return Slope of linear regression (positive = rising fees, negative = falling fees)
     */
    double getTrendSlope() const { return slope_; }

    /**
     * @brief Check if predictor has sufficient data
     */
    bool hasSufficientData() const { return observations_.size() >= min_observations_; }

    /**
     * @brief Clear all observations
     */
    void clear();

private:
    size_t window_size_;           // Max observations to keep
    size_t min_observations_;      // Min observations for prediction (default: 10)

    // Historical observations (sliding window)
    std::deque<FeeDataPoint> observations_;

    // Linear regression coefficients
    mutable double slope_;         // Trend slope (sat/vB per second)
    mutable double intercept_;     // Y-intercept
    mutable uint64_t last_update_; // When regression was last computed

    // Helper: Compute linear regression
    void computeRegression() const;

    // Thread safety
    mutable std::mutex mutex_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Mempool Analyzer
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Real-time mempool state analysis
 *
 * Analyzes current mempool to determine:
 * - Congestion level (mempool fullness)
 * - Fee pressure (demand for block space)
 * - Percentile-based fee recommendations
 */
class MempoolAnalyzer {
public:
    MempoolAnalyzer();

    /**
     * @brief Analyze current mempool state
     *
     * Extracts real-time fee metrics from mempool:
     * - P25, P50, P75, P90 fee rates
     * - Mempool congestion ratio
     * - Fee pressure indicator
     *
     * @param mempool Mempool to analyze
     * @param target_blocks Target confirmation time
     * @return Recommended fee rate (sat/vB)
     */
    double analyzeMempoolFees(const mempool::Mempool& mempool, size_t target_blocks) const;

    /**
     * @brief Calculate mempool congestion ratio
     *
     * @param mempool Mempool to analyze
     * @return Congestion [0.0, 1.0+] (0 = empty, 1.0 = full, >1.0 = over capacity)
     */
    double calculateCongestion(const mempool::Mempool& mempool) const;

    /**
     * @brief Get fee rate at percentile
     *
     * @param mempool Mempool to analyze
     * @param percentile Percentile [0.0, 1.0] (e.g., 0.5 = median)
     * @return Fee rate at percentile (sat/vB)
     */
    double getFeeAtPercentile(const mempool::Mempool& mempool, double percentile) const;

private:
    // Mempool capacity threshold (bytes)
    size_t capacity_mb_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Historical Data Persistence
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Persistent fee history storage
 *
 * Stores fee observations to disk for:
 * - Better estimates after restart
 * - Long-term trend analysis
 * - Model training data
 *
 * Format: CSV file with: timestamp,height,fee_rate,target_blocks,confidence
 */
class FeeHistoryPersistence {
public:
    explicit FeeHistoryPersistence(const std::string& data_dir);

    /**
     * @brief Load historical fee data from disk
     *
     * @return Vector of historical data points
     */
    std::vector<FeeDataPoint> loadHistory() const;

    /**
     * @brief Save fee data point to disk (append mode)
     *
     * @param data_point Fee observation to save
     * @return True if saved successfully
     */
    bool saveDataPoint(const FeeDataPoint& data_point);

    /**
     * @brief Prune old data (keep last N days)
     *
     * @param max_age_days Maximum age to keep (default: 30 days)
     * @return Number of entries pruned
     */
    size_t pruneOldData(uint32_t max_age_days = 30);

private:
    std::string data_dir_;         // Data directory
    std::string history_file_;     // Path to fee_history.csv

    mutable std::mutex mutex_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Adaptive Fallback Rates
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Adaptive fallback rate system
 *
 * Dynamically adjusts fallback rates based on recent network activity.
 * Prevents static fallbacks from being too high or too low.
 */
class AdaptiveFallbackRates {
public:
    AdaptiveFallbackRates();

    /**
     * @brief Update fallback rates based on recent observations
     *
     * @param recent_fees Recent confirmed transaction fees
     */
    void updateFromRecentActivity(const std::vector<double>& recent_fees);

    /**
     * @brief Get adaptive fallback rate for target
     *
     * @param target Fee target (IMMEDIATE, FAST, NORMAL, etc.)
     * @return Adaptive fallback rate (sat/vB)
     */
    double getFallbackRate(FeeTarget target) const;

    /**
     * @brief Reset to default static fallbacks
     */
    void reset();

private:
    // Current adaptive fallback rates (sat/KB)
    std::unordered_map<FeeTarget, double> fallback_rates_;

    // Decay factor for EWMA updates
    double decay_factor_;

    mutable std::mutex mutex_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Hybrid Fee Estimator
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Hybrid ML Fee Estimator
 *
 * Combines multiple estimation techniques for robust fee predictions:
 * 1. EWMA base layer (existing FeeEstimator)
 * 2. ML trend prediction (linear regression on recent history)
 * 3. Mempool-aware real-time adjustments
 * 4. Historical persistence for cold starts
 * 5. Adaptive fallback rates
 *
 * Decision Logic:
 * - If mempool is congested (>70% full) → use mempool analysis (real-time)
 * - Else if EWMA has sufficient data → use EWMA estimate
 * - Else if ML predictor has data → use ML prediction
 * - Else → use adaptive fallback
 *
 * Weight adjustment:
 * - ML weight increases with trend confidence
 * - Mempool weight increases with congestion
 * - EWMA weight is baseline
 */
class HybridFeeEstimator {
public:
    explicit HybridFeeEstimator(const std::string& data_dir);
    ~HybridFeeEstimator();

    /**
     * @brief Initialize estimator
     *
     * Loads historical data and initializes sub-components.
     *
     * @param base_estimator Existing EWMA-based fee estimator (optional)
     * @return True if initialized successfully
     */
    bool initialize(std::shared_ptr<FeeEstimator> base_estimator = nullptr);

    /**
     * @brief Set mempool for real-time analysis
     *
     * @param mempool Mempool instance
     */
    void setMempool(mempool::Mempool* mempool) { mempool_ = mempool; }

    /**
     * @brief Record new transaction confirmation
     *
     * Called when a transaction gets confirmed to update models.
     *
     * @param fee_rate Fee rate of confirmed transaction (sat/vB)
     * @param confirmation_blocks Blocks until confirmation
     * @param height Block height
     */
    void recordConfirmation(double fee_rate, size_t confirmation_blocks, uint32_t height);

    /**
     * @brief Estimate fee rate (hybrid method)
     *
     * Combines EWMA + ML + Mempool analysis for robust estimate.
     *
     * @param target Fee target (IMMEDIATE, FAST, NORMAL, etc.)
     * @return Hybrid fee estimate
     */
    FeeEstimate estimateFee(FeeTarget target);

    /**
     * @brief Estimate fee rate for confirmation in N blocks
     *
     * Compatibility method for existing interfaces.
     *
     * @param target_blocks Number of blocks for confirmation
     * @return Estimated fee rate (sat/vB)
     */
    double estimateFeeRate(size_t target_blocks);

    /**
     * @brief Get estimate breakdown (for debugging/monitoring)
     *
     * Returns individual estimates from each component:
     * - EWMA estimate
     * - ML prediction
     * - Mempool analysis
     * - Final hybrid estimate
     */
    struct EstimateBreakdown {
        double ewma_estimate;
        double ml_prediction;
        double mempool_estimate;
        double hybrid_final;
        double congestion_ratio;
        double trend_slope;
        std::string decision_reason;
    };

    EstimateBreakdown getBreakdown(FeeTarget target) const;

    /**
     * @brief Periodic maintenance (pruning, persistence)
     *
     * Should be called periodically (e.g., every hour).
     */
    void performMaintenance();

private:
    // Sub-components
    std::shared_ptr<FeeEstimator> base_estimator_;        // EWMA base layer
    std::unique_ptr<MLTrendPredictor> ml_predictor_;      // ML trend prediction
    std::unique_ptr<MempoolAnalyzer> mempool_analyzer_;   // Real-time mempool analysis
    std::unique_ptr<FeeHistoryPersistence> persistence_;  // Historical data storage
    std::unique_ptr<AdaptiveFallbackRates> adaptive_fallbacks_;  // Dynamic fallbacks

    // Mempool reference
    mempool::Mempool* mempool_{nullptr};

    // Data directory
    std::string data_dir_;

    // Hybrid decision weights
    struct Weights {
        double ewma_weight;
        double ml_weight;
        double mempool_weight;
    };

    Weights calculateWeights(FeeTarget target) const;

    // Helper: Combine estimates using weighted average
    double combineEstimates(double ewma, double ml, double mempool, const Weights& weights) const;

    // Thread safety
    mutable std::mutex mutex_;
};

} // namespace policy
} // namespace dinero
