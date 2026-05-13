#include "mempool/fee_estimator.h"
#include "common/logger.h"
#include <algorithm>
#include <cmath>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════════

FeeEstimator::FeeEstimator() {
    g_logger.info("FeeEstimator initialized (Bitcoin Core conservative approach)");
}

// ═══════════════════════════════════════════════════════════════════════════
// Event Tracking
// ═══════════════════════════════════════════════════════════════════════════

void FeeEstimator::recordTxEntry(const uint256& txid, double feerate, uint32_t entry_height) {
    std::lock_guard<std::mutex> lock(mutex_);

    tracked_txs_[txid] = TrackedTx(txid, feerate, entry_height);

    // Phase M.1.A: Logging boundary - convert uint256 to hex at output
    g_logger.debug("FeeEstimator: Tracking tx " + txid.GetHex() +
                   " (feerate: " + std::to_string(feerate) + " sat/byte, height: " +
                   std::to_string(entry_height) + ")");
}

void FeeEstimator::recordTxConfirmation(const uint256& txid, uint32_t confirmation_height) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tracked_txs_.find(txid);
    if (it == tracked_txs_.end()) {
        // Transaction not tracked (might have been added before fee estimator started)
        return;
    }

    TrackedTx& tx = it->second;
    tx.confirmation_height = confirmation_height;

    // Calculate blocks to confirmation
    if (confirmation_height <= tx.entry_height) {
        // Phase M.1.A: Logging boundary - convert uint256 to hex at output
        g_logger.warning("FeeEstimator: Invalid confirmation height for " + txid.GetHex());
        tracked_txs_.erase(it);
        return;
    }

    uint32_t blocks_to_confirmation = confirmation_height - tx.entry_height;

    // Add to appropriate bucket
    uint32_t bucket_key = getConfirmationBucket(blocks_to_confirmation);
    confirmation_buckets_[bucket_key].feerates.push_back(tx.feerate);

    total_confirmed_++;

    // Phase M.1.A: Logging boundary - convert uint256 to hex at output
    g_logger.debug("FeeEstimator: Confirmed tx " + txid.GetHex() +
                   " (feerate: " + std::to_string(tx.feerate) + " sat/byte, " +
                   "confirmed in " + std::to_string(blocks_to_confirmation) + " blocks, " +
                   "bucket: " + std::to_string(bucket_key) + ")");

    // Remove from tracked transactions (no longer needed)
    tracked_txs_.erase(it);
}

void FeeEstimator::recordTxEviction(const uint256& txid) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tracked_txs_.find(txid);
    if (it == tracked_txs_.end()) {
        return;
    }

    total_evicted_++;

    // Phase M.1.A: Logging boundary - convert uint256 to hex at output
    g_logger.debug("FeeEstimator: Evicted tx " + txid.GetHex() +
                   " (feerate: " + std::to_string(it->second.feerate) + " sat/byte)");

    // Remove from tracked transactions
    tracked_txs_.erase(it);
}

// ═══════════════════════════════════════════════════════════════════════════
// Fee Estimation
// ═══════════════════════════════════════════════════════════════════════════

std::optional<double> FeeEstimator::estimateFee(uint32_t target_blocks) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate target
    if (target_blocks < MIN_TARGET_BLOCKS || target_blocks > MAX_TARGET_BLOCKS) {
        g_logger.warning("FeeEstimator: Invalid target blocks: " + std::to_string(target_blocks));
        return std::nullopt;
    }

    // Determine which bucket to use
    uint32_t bucket_key;
    if (target_blocks <= 2) {
        bucket_key = 2;  // Fast: 1-2 blocks
    } else if (target_blocks <= 6) {
        bucket_key = 6;  // Medium: 3-6 blocks
    } else {
        bucket_key = 12; // Slow: 6-12 blocks
    }

    // Try exact bucket first
    auto estimate = getFeerateForBucket(bucket_key);
    if (estimate.has_value()) {
        g_logger.debug("FeeEstimator: Estimate for " + std::to_string(target_blocks) +
                       " blocks: " + std::to_string(estimate.value()) + " sat/byte (bucket: " +
                       std::to_string(bucket_key) + ")");
        return estimate;
    }

    // Try fallback to slower bucket (more conservative)
    if (bucket_key == 2) {
        estimate = getFeerateForBucket(6);
        if (estimate.has_value()) {
            g_logger.debug("FeeEstimator: Using medium bucket for fast estimate");
            return estimate;
        }
        estimate = getFeerateForBucket(12);
        if (estimate.has_value()) {
            g_logger.debug("FeeEstimator: Using slow bucket for fast estimate");
            return estimate;
        }
    } else if (bucket_key == 6) {
        estimate = getFeerateForBucket(12);
        if (estimate.has_value()) {
            g_logger.debug("FeeEstimator: Using slow bucket for medium estimate");
            return estimate;
        }
    }

    g_logger.debug("FeeEstimator: Insufficient data for " + std::to_string(target_blocks) + " blocks");
    return std::nullopt;
}

FeeEstimator::Stats FeeEstimator::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.tracked_txs = tracked_txs_.size();
    stats.confirmed_txs = total_confirmed_;
    stats.evicted_txs = total_evicted_;

    // Count samples per bucket
    auto it_fast = confirmation_buckets_.find(2);
    stats.fast_samples = (it_fast != confirmation_buckets_.end()) ? it_fast->second.feerates.size() : 0;

    auto it_medium = confirmation_buckets_.find(6);
    stats.medium_samples = (it_medium != confirmation_buckets_.end()) ? it_medium->second.feerates.size() : 0;

    auto it_slow = confirmation_buckets_.find(12);
    stats.slow_samples = (it_slow != confirmation_buckets_.end()) ? it_slow->second.feerates.size() : 0;

    return stats;
}

void FeeEstimator::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    tracked_txs_.clear();
    confirmation_buckets_.clear();
    total_confirmed_ = 0;
    total_evicted_ = 0;

    g_logger.info("FeeEstimator: Cleared all data");
}

// ═══════════════════════════════════════════════════════════════════════════
// Helper Methods
// ═══════════════════════════════════════════════════════════════════════════

uint32_t FeeEstimator::getConfirmationBucket(uint32_t blocks_to_confirmation) const {
    // Bucket mapping:
    // 1-2 blocks → bucket 2 (fast)
    // 3-6 blocks → bucket 6 (medium)
    // 7-12 blocks → bucket 12 (slow)
    // 13+ blocks → bucket 12 (slow, treat as slow)

    if (blocks_to_confirmation <= 2) {
        return 2;
    } else if (blocks_to_confirmation <= 6) {
        return 6;
    } else {
        return 12;
    }
}

std::optional<double> FeeEstimator::getFeerateForBucket(uint32_t bucket_key) const {
    auto it = confirmation_buckets_.find(bucket_key);
    if (it == confirmation_buckets_.end()) {
        return std::nullopt;
    }

    const ConfirmationBucket& bucket = it->second;

    // Need minimum samples for reliable estimate
    if (bucket.feerates.size() < MIN_SAMPLES_FOR_ESTIMATE) {
        return std::nullopt;
    }

    // Use 75th percentile for conservative estimate
    // (Bitcoin Core uses various percentiles depending on target)
    return bucket.get75thPercentileFeerate();
}

// ═══════════════════════════════════════════════════════════════════════════
// ConfirmationBucket Implementation
// ═══════════════════════════════════════════════════════════════════════════

std::optional<double> FeeEstimator::ConfirmationBucket::getMedianFeerate() const {
    if (feerates.empty()) {
        return std::nullopt;
    }

    std::vector<double> sorted = feerates;
    std::sort(sorted.begin(), sorted.end());

    size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 0) {
        return (sorted[mid - 1] + sorted[mid]) / 2.0;
    } else {
        return sorted[mid];
    }
}

std::optional<double> FeeEstimator::ConfirmationBucket::get75thPercentileFeerate() const {
    if (feerates.empty()) {
        return std::nullopt;
    }

    std::vector<double> sorted = feerates;
    std::sort(sorted.begin(), sorted.end());

    // 75th percentile position
    size_t pos = static_cast<size_t>(std::ceil(0.75 * sorted.size())) - 1;
    if (pos >= sorted.size()) {
        pos = sorted.size() - 1;
    }

    return sorted[pos];
}

} // namespace dinero
