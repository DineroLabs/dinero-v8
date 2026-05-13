#include "wallet/sync_eta_estimator.h"
#include "common/logger.h"
#include <algorithm>
#include <cmath>

namespace dinero {

// ============================================================================
// Constructor
// ============================================================================

SyncETAEstimator::SyncETAEstimator(uint64_t min_window_ms, size_t max_samples)
    : min_window_ms_(min_window_ms)
    , max_samples_(max_samples)
    , is_frozen_(false)
{
}

// ============================================================================
// Sample Recording
// ============================================================================

void SyncETAEstimator::RecordSample(uint64_t timestamp_ms, uint64_t current_value) {
    // Add new sample
    samples_.push_back({timestamp_ms, current_value});

    // Enforce max samples (keep most recent)
    while (samples_.size() > max_samples_) {
        samples_.pop_front();
    }

    // Remove stale samples (older than reasonable window, e.g. 2 minutes)
    const uint64_t MAX_AGE_MS = 120000;  // 2 minutes
    while (!samples_.empty() &&
           timestamp_ms - samples_.front().timestamp_ms > MAX_AGE_MS) {
        samples_.pop_front();
    }
}

// ============================================================================
// ETA Calculation
// ============================================================================

std::optional<std::chrono::seconds> SyncETAEstimator::CalculateETA(
    uint64_t current_value,
    uint64_t target_value,
    uint64_t current_time_ms
) const {
    // Rule 1: Frozen (e.g. during reorg) → no ETA
    if (is_frozen_) {
        return std::nullopt;
    }

    // Rule 2: Already complete → 0 seconds
    if (current_value >= target_value) {
        return std::chrono::seconds(0);
    }

    // Rule 3: Need stable data (≥min_window_ms)
    if (!IsStable(current_time_ms)) {
        return std::nullopt;  // Show "Estimating..."
    }

    // Rule 4: Calculate rate
    double rate = CalculateRate();

    // Rule 5: Need positive rate (making progress)
    if (rate <= 0.0) {
        return std::nullopt;  // Stalled or no progress
    }

    // Rule 6: Calculate remaining work
    uint64_t remaining = target_value - current_value;

    // Rule 7: ETA = remaining / rate
    double eta_seconds = static_cast<double>(remaining) / rate;

    // Rule 8: Sanity check (cap at reasonable maximum, e.g. 24 hours)
    const double MAX_ETA_SECONDS = 24 * 3600;  // 24 hours
    if (eta_seconds > MAX_ETA_SECONDS) {
        return std::nullopt;  // Too far in future, don't guess
    }

    return std::chrono::seconds(static_cast<int64_t>(eta_seconds));
}

// ============================================================================
// Rate Calculation (Linear Regression)
// ============================================================================

double SyncETAEstimator::CalculateRate() const {
    // Need at least 2 samples for rate
    if (samples_.size() < 2) {
        return 0.0;
    }

    // Simple approach: delta between first and last sample
    const auto& oldest = samples_.front();
    const auto& newest = samples_.back();

    // Time span (convert to seconds)
    uint64_t time_span_ms = newest.timestamp_ms - oldest.timestamp_ms;
    if (time_span_ms == 0) {
        return 0.0;  // No time elapsed
    }

    double time_span_sec = static_cast<double>(time_span_ms) / 1000.0;

    // Value change
    int64_t value_delta = static_cast<int64_t>(newest.value) -
                          static_cast<int64_t>(oldest.value);

    // Handle negative progress (reorg, rollback) → rate = 0
    if (value_delta < 0) {
        return 0.0;
    }

    // Rate = change / time
    double rate = static_cast<double>(value_delta) / time_span_sec;

    // Smooth with moving average if we have many samples
    if (samples_.size() >= 10) {
        // Calculate rate over multiple windows for stability
        double sum_rates = 0.0;
        int count = 0;

        // Calculate rate for each consecutive pair
        for (size_t i = 1; i < samples_.size(); ++i) {
            uint64_t dt_ms = samples_[i].timestamp_ms - samples_[i-1].timestamp_ms;
            if (dt_ms == 0) continue;

            int64_t dv = static_cast<int64_t>(samples_[i].value) -
                        static_cast<int64_t>(samples_[i-1].value);
            if (dv < 0) continue;  // Skip negative deltas

            double dt_sec = static_cast<double>(dt_ms) / 1000.0;
            sum_rates += static_cast<double>(dv) / dt_sec;
            count++;
        }

        if (count > 0) {
            rate = sum_rates / count;  // Average rate
        }
    }

    return rate;
}

// ============================================================================
// Stability Check
// ============================================================================

bool SyncETAEstimator::IsStable(uint64_t current_time_ms) const {
    // Need at least 2 samples
    if (samples_.size() < 2) {
        return false;
    }

    // Check time span
    uint64_t span = GetSampleSpan();
    if (span < min_window_ms_) {
        return false;  // Need more time
    }

    // Check if we have minimum sample count (e.g. 10)
    const size_t MIN_SAMPLES = 10;
    if (samples_.size() < MIN_SAMPLES) {
        return false;
    }

    return true;
}

// ============================================================================
// Current Rate Getter
// ============================================================================

double SyncETAEstimator::GetCurrentRate() const {
    return CalculateRate();
}

// ============================================================================
// Sample Span
// ============================================================================

uint64_t SyncETAEstimator::GetSampleSpan() const {
    if (samples_.size() < 2) {
        return 0;
    }

    return samples_.back().timestamp_ms - samples_.front().timestamp_ms;
}

// ============================================================================
// Clear
// ============================================================================

void SyncETAEstimator::Clear() {
    samples_.clear();
    is_frozen_ = false;
}

} // namespace dinero
