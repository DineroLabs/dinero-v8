#include "mining/template_delta_tracker.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {

// ============================================================================
// Constructor
// ============================================================================

TemplateDeltaTracker::TemplateDeltaTracker()
    : last_snapshot_time_ms_(0)
    , removed_template_txs_(0)
{
}

// ============================================================================
// Mempool Snapshot
// ============================================================================

void TemplateDeltaTracker::SnapshotMempool(
    const std::unordered_set<uint256>& txids,
    uint64_t snapshot_time_ms
) {
    // Clear previous delta tracking
    added_txs_.clear();
    removed_txs_.clear();
    added_fee_rates_.clear();
    removed_template_txs_ = 0;

    // Record new snapshot
    template_txids_ = txids;
    last_snapshot_time_ms_ = snapshot_time_ms;

    dinero::g_logger.debug("TemplateDeltaTracker: Snapshot " +
                          std::to_string(txids.size()) +
                          " transactions at time " +
                          std::to_string(snapshot_time_ms));
}

// ============================================================================
// Delta Tracking
// ============================================================================

void TemplateDeltaTracker::OnTransactionAdded(
    const uint256& txid,
    uint64_t fee_rate,
    uint64_t entry_time_ms
) {
    // If this tx was previously removed, un-remove it
    if (removed_txs_.count(txid) > 0) {
        removed_txs_.erase(txid);

        // Was it in the template?
        if (template_txids_.count(txid) > 0 && removed_template_txs_ > 0) {
            removed_template_txs_--;
        }
    } else {
        // New transaction (not in snapshot)
        added_txs_.insert(txid);
        added_fee_rates_[txid] = fee_rate;
    }
}

void TemplateDeltaTracker::OnTransactionRemoved(const uint256& txid) {
    // If this tx was previously added (never in snapshot), just remove it from added
    if (added_txs_.count(txid) > 0) {
        added_txs_.erase(txid);
        added_fee_rates_.erase(txid);
    } else {
        // Transaction was in snapshot, mark as removed
        removed_txs_.insert(txid);

        // Was it in the template?
        if (template_txids_.count(txid) > 0) {
            removed_template_txs_++;
        }
    }
}

// ============================================================================
// Refresh Decision Logic
// ============================================================================

bool TemplateDeltaTracker::ShouldRefreshTemplate(
    uint64_t current_time_ms,
    size_t delta_threshold,
    uint64_t max_age_ms
) const {
    // Criteria 1: Delta count exceeds threshold
    size_t delta_count = GetDeltaCount();
    if (delta_count >= delta_threshold) {
        dinero::g_logger.debug("ShouldRefreshTemplate: YES (delta=" +
                              std::to_string(delta_count) + " >= threshold=" +
                              std::to_string(delta_threshold) + ")");
        return true;
    }

    // Criteria 2: Template age exceeds maximum
    if (last_snapshot_time_ms_ > 0) {
        uint64_t age_ms = current_time_ms - last_snapshot_time_ms_;
        if (age_ms > max_age_ms) {
            dinero::g_logger.debug("ShouldRefreshTemplate: YES (age=" +
                                  std::to_string(age_ms / 1000) + "s > max=" +
                                  std::to_string(max_age_ms / 1000) + "s)");
            return true;
        }
    }

    // Criteria 3: Significant template transaction removed
    // If > 10% of template txs removed, refresh
    if (template_txids_.size() > 0) {
        double removal_ratio = static_cast<double>(removed_template_txs_) / template_txids_.size();
        if (removal_ratio > 0.1) {  // 10% threshold
            dinero::g_logger.debug("ShouldRefreshTemplate: YES (removed " +
                                  std::to_string(removed_template_txs_) + "/" +
                                  std::to_string(template_txids_.size()) +
                                  " template txs = " +
                                  std::to_string(static_cast<int>(removal_ratio * 100)) + "%)");
            return true;
        }
    }

    return false;
}

uint64_t TemplateDeltaTracker::GetHighestAddedFeeRate() const {
    if (added_fee_rates_.empty()) {
        return 0;
    }

    uint64_t max_fee_rate = 0;
    for (const auto& [txid, fee_rate] : added_fee_rates_) {
        if (fee_rate > max_fee_rate) {
            max_fee_rate = fee_rate;
        }
    }

    return max_fee_rate;
}

double TemplateDeltaTracker::GetRefreshUrgency(
    uint64_t current_time_ms,
    uint64_t median_fee_rate
) const {
    double urgency = 0.0;

    // Factor 1: Delta count (10 txs = 0.2, 50 txs = 1.0)
    size_t delta_count = GetDeltaCount();
    double delta_factor = std::min(1.0, static_cast<double>(delta_count) / 50.0);
    urgency += delta_factor * 0.4;  // Delta contributes 40%

    // Factor 2: Template age (30s = 0.5, 60s = 1.0)
    if (last_snapshot_time_ms_ > 0) {
        uint64_t age_ms = current_time_ms - last_snapshot_time_ms_;
        double age_seconds = static_cast<double>(age_ms) / 1000.0;
        double age_factor = std::min(1.0, age_seconds / 60.0);
        urgency += age_factor * 0.3;  // Age contributes 30%
    }

    // Factor 3: High-fee additions (multiplier up to 2×)
    uint64_t highest_fee = GetHighestAddedFeeRate();
    if (highest_fee > median_fee_rate * 2) {
        // High-fee transaction added (>2× median)
        double fee_multiplier = std::min(2.0, static_cast<double>(highest_fee) / median_fee_rate);
        urgency *= fee_multiplier;  // Can double the urgency
    }

    // Factor 4: Removed template transactions (penalty)
    if (template_txids_.size() > 0) {
        double removal_ratio = static_cast<double>(removed_template_txs_) / template_txids_.size();
        urgency += removal_ratio * 0.3;  // Removals contribute 30%
    }

    return urgency;
}

void TemplateDeltaTracker::Clear() {
    template_txids_.clear();
    last_snapshot_time_ms_ = 0;
    added_txs_.clear();
    removed_txs_.clear();
    added_fee_rates_.clear();
    removed_template_txs_ = 0;

    dinero::g_logger.debug("TemplateDeltaTracker: Cleared all data");
}

} // namespace dinero
