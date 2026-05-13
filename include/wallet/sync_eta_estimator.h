#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <cstdint>

namespace dinero {

/**
 * @brief Phase W.2.2: ETA Estimator (Honest & Stable)
 *
 * Tracks sync progress over time and estimates completion time.
 * Never lies - only shows ETA when data is stable and reliable.
 *
 * Design Principles:
 * - Require ≥30s window before showing ETA
 * - Use rolling average for stability
 * - Freeze during reorgs
 * - Show "Estimating..." when unstable
 * - Handle rate changes gracefully
 */
class SyncETAEstimator {
public:
    /**
     * @brief Progress sample (timestamp + progress value)
     */
    struct ProgressSample {
        uint64_t timestamp_ms;  // When sample was taken
        uint64_t value;         // Progress value (blocks, headers, etc.)
    };

    /**
     * @brief Create ETA estimator
     *
     * @param min_window_ms Minimum time window for stability (default: 30s)
     * @param max_samples Maximum samples to keep (default: 60, for ~1min history at 1s sampling)
     */
    explicit SyncETAEstimator(
        uint64_t min_window_ms = 30000,
        size_t max_samples = 60
    );

    /**
     * @brief Record progress sample
     *
     * Call this periodically (e.g. every 1s) to track progress.
     *
     * @param timestamp_ms Current timestamp (milliseconds)
     * @param current_value Current progress value (e.g. blocks synced)
     */
    void RecordSample(uint64_t timestamp_ms, uint64_t current_value);

    /**
     * @brief Calculate ETA for remaining work
     *
     * Returns ETA only if:
     * - Data is stable (≥min_window_ms of samples)
     * - Rate is positive (making progress)
     * - Not frozen (no reorg in progress)
     *
     * @param current_value Current progress value
     * @param target_value Target progress value
     * @param current_time_ms Current timestamp
     * @return ETA in seconds, or nullopt if unstable/unknown
     */
    std::optional<std::chrono::seconds> CalculateETA(
        uint64_t current_value,
        uint64_t target_value,
        uint64_t current_time_ms
    ) const;

    /**
     * @brief Get current rate (items/second)
     *
     * Uses rolling average over available samples.
     *
     * @return Rate, or 0.0 if no rate available
     */
    double GetCurrentRate() const;

    /**
     * @brief Check if estimator has enough data for stable ETA
     *
     * @param current_time_ms Current timestamp
     * @return True if ≥min_window_ms of data available
     */
    bool IsStable(uint64_t current_time_ms) const;

    /**
     * @brief Freeze ETA calculation (e.g. during reorg)
     *
     * While frozen, CalculateETA() will return nullopt.
     */
    void Freeze() { is_frozen_ = true; }

    /**
     * @brief Unfreeze ETA calculation
     */
    void Unfreeze() { is_frozen_ = false; }

    /**
     * @brief Check if frozen
     */
    bool IsFrozen() const { return is_frozen_; }

    /**
     * @brief Clear all samples and reset state
     */
    void Clear();

    /**
     * @brief Get number of samples collected
     */
    size_t GetSampleCount() const { return samples_.size(); }

    /**
     * @brief Get time span of collected samples (milliseconds)
     *
     * @return Span from oldest to newest sample, or 0 if < 2 samples
     */
    uint64_t GetSampleSpan() const;

private:
    /**
     * @brief Calculate rate from samples (items/second)
     *
     * Uses linear regression for smooth rate calculation.
     *
     * @return Rate, or 0.0 if insufficient data
     */
    double CalculateRate() const;

    std::deque<ProgressSample> samples_;  // Rolling window of samples
    uint64_t min_window_ms_;              // Minimum time window for stability
    size_t max_samples_;                  // Maximum samples to keep
    bool is_frozen_;                      // Frozen during reorg
};

} // namespace dinero
