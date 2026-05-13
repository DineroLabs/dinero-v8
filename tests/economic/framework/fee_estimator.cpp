#include "fee_estimator.h"
#include <algorithm>
#include <cmath>

namespace dinero {
namespace economic {
namespace test {

FeeEstimator::FeeEstimator(const EconomicPolicy& policy)
    : policy_(policy)
    , processed_blocks_(0)
{
}

void FeeEstimator::reset() {
    tracked_txs_.clear();
    processed_blocks_ = 0;
}

void FeeEstimator::processBlock(
    uint32_t block_height,
    const std::vector<MempoolEntry>& confirmed_txs,
    uint64_t timestamp
) {
    // Add confirmed transactions to history
    for (const auto& entry : confirmed_txs) {
        ConfirmedTx confirmed;
        confirmed.tx_id = entry.tx_id;
        confirmed.block_height = block_height;
        confirmed.fee_una = entry.fee_una;
        confirmed.tx_size_bytes = entry.tx_size_bytes;
        confirmed.fee_rate = entry.fee_rate;
        confirmed.timestamp = timestamp;

        tracked_txs_.push_back(confirmed);
    }

    processed_blocks_++;

    // Prune old history to keep memory bounded
    pruneOldHistory(block_height);
}

std::optional<double> FeeEstimator::estimateFeeRate(uint32_t confirmation_target) const {
    if (!hasSufficientData()) {
        return std::nullopt;
    }

    return computeMedianFeeRate(confirmation_target);
}

std::vector<FeeEstimate> FeeEstimator::getAllEstimates() const {
    std::vector<FeeEstimate> estimates;

    for (uint32_t target : policy_.confirmation_targets) {
        auto fee_rate = estimateFeeRate(target);
        if (fee_rate) {
            FeeEstimate estimate;
            estimate.confirmation_target = target;
            estimate.estimated_fee_rate = *fee_rate;
            estimate.timestamp = 0;  // Simulator will set this
            estimates.push_back(estimate);
        }
    }

    return estimates;
}

bool FeeEstimator::hasSufficientData() const {
    return tracked_txs_.size() >= MIN_TXS_FOR_ESTIMATE && processed_blocks_ >= 1;
}

std::optional<double> FeeEstimator::computeMedianFeeRate(uint32_t confirmation_target) const {
    // Get transactions from recent blocks (up to confirmation_target blocks)
    auto recent_txs = getRecentTxs(confirmation_target);

    if (recent_txs.size() < MIN_TXS_FOR_ESTIMATE) {
        return std::nullopt;
    }

    // Extract fee rates
    std::vector<double> fee_rates;
    fee_rates.reserve(recent_txs.size());
    for (const auto& tx : recent_txs) {
        fee_rates.push_back(tx.fee_rate);
    }

    // Sort to find median
    std::sort(fee_rates.begin(), fee_rates.end());

    // Compute median
    size_t mid = fee_rates.size() / 2;
    double median;
    if (fee_rates.size() % 2 == 0) {
        median = (fee_rates[mid - 1] + fee_rates[mid]) / 2.0;
    } else {
        median = fee_rates[mid];
    }

    // Ensure estimate is at least min relay fee rate
    double min_relay_rate = static_cast<double>(policy_.min_relay_fee_una) / 1000.0;  // Assume 1KB avg tx
    return std::max(median, min_relay_rate);
}

std::vector<FeeEstimator::ConfirmedTx> FeeEstimator::getRecentTxs(uint32_t block_count) const {
    if (tracked_txs_.empty()) {
        return {};
    }

    // Find the highest block height
    uint32_t max_height = 0;
    for (const auto& tx : tracked_txs_) {
        max_height = std::max(max_height, tx.block_height);
    }

    // Collect txs from recent blocks
    uint32_t min_height = (max_height >= block_count) ? (max_height - block_count + 1) : 0;

    std::vector<ConfirmedTx> recent;
    for (const auto& tx : tracked_txs_) {
        if (tx.block_height >= min_height) {
            recent.push_back(tx);
        }
    }

    return recent;
}

void FeeEstimator::pruneOldHistory(uint32_t current_height) {
    if (tracked_txs_.empty()) {
        return;
    }

    // Keep only transactions from recent MAX_HISTORY_BLOCKS
    uint32_t min_height = (current_height >= MAX_HISTORY_BLOCKS)
        ? (current_height - MAX_HISTORY_BLOCKS + 1)
        : 0;

    // Remove old transactions
    tracked_txs_.erase(
        std::remove_if(
            tracked_txs_.begin(),
            tracked_txs_.end(),
            [min_height](const ConfirmedTx& tx) {
                return tx.block_height < min_height;
            }
        ),
        tracked_txs_.end()
    );
}

} // namespace test
} // namespace economic
} // namespace dinero
