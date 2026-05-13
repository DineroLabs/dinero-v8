#include "mining/transaction_scorer.h"
#include "daemon/mempool.h"
#include "common/logger.h"
#include <algorithm>
#include <cmath>

namespace dinero {

// ============================================================================
// FeeHistogram Implementation
// ============================================================================

constexpr uint64_t FeeHistogram::BUCKET_BOUNDARIES[NUM_BUCKETS];

FeeHistogram::FeeHistogram()
    : total_count_(0)
{
    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        buckets_[i] = 0;
    }
}

void FeeHistogram::AddTransaction(uint64_t fee_rate) {
    size_t bucket = GetBucketIndex(fee_rate);
    buckets_[bucket]++;
    total_count_++;
}

void FeeHistogram::RemoveTransaction(uint64_t fee_rate) {
    if (total_count_ == 0) return;

    size_t bucket = GetBucketIndex(fee_rate);
    if (buckets_[bucket] > 0) {
        buckets_[bucket]--;
        total_count_--;
    }
}

double FeeHistogram::GetPercentile(uint64_t fee_rate) const {
    if (total_count_ == 0) {
        return 0.5;  // No data, return neutral
    }

    // Count transactions with lower fee rate
    size_t lower_count = 0;
    size_t target_bucket = GetBucketIndex(fee_rate);

    for (size_t i = 0; i < target_bucket; ++i) {
        lower_count += buckets_[i];
    }

    // Add half of transactions in same bucket (assume uniform distribution)
    lower_count += buckets_[target_bucket] / 2;

    // Calculate percentile
    double percentile = static_cast<double>(lower_count) / total_count_;
    return std::min(1.0, std::max(0.0, percentile));
}

uint64_t FeeHistogram::GetFeeRateAtPercentile(double percentile) const {
    if (total_count_ == 0) {
        return 1;  // No data, return minimum fee
    }

    // Clamp percentile to [0, 1]
    percentile = std::min(1.0, std::max(0.0, percentile));

    // Target count: how many txs should be below this percentile
    size_t target_count = static_cast<size_t>(percentile * total_count_);

    // Walk buckets until we reach target count
    size_t cumulative = 0;
    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        cumulative += buckets_[i];
        if (cumulative >= target_count) {
            // Return the upper boundary of this bucket as the fee rate
            // This is conservative: "at least this fee rate to be in this percentile"
            return BUCKET_BOUNDARIES[i];
        }
    }

    // All buckets traversed, return highest boundary
    return BUCKET_BOUNDARIES[NUM_BUCKETS - 1];
}

void FeeHistogram::Clear() {
    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        buckets_[i] = 0;
    }
    total_count_ = 0;
}

void FeeHistogram::BuildFromMempool(const Mempool* mempool) {
    Clear();

    if (!mempool) {
        return;
    }

    // Build histogram from real mempool fee distribution
    // Guardrail: Skip zero-fee entries to prevent skewed percentiles
    mempool->forEachEntry([this](const MempoolEntry& entry) {
        if (entry.fee_rate > 0) {
            AddTransaction(static_cast<uint64_t>(entry.fee_rate));
        }
    });

    dinero::g_logger.debug("FeeHistogram: Built from mempool with " +
                          std::to_string(total_count_) + " transactions");
}

size_t FeeHistogram::GetBucketIndex(uint64_t fee_rate) const {
    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        if (fee_rate < BUCKET_BOUNDARIES[i]) {
            return i;
        }
    }
    return NUM_BUCKETS - 1;  // Highest bucket
}

// ============================================================================
// TransactionScorer Implementation
// ============================================================================

TransactionScorer::TransactionScorer(const BlockAssemblyContext& context)
    : context_(context)
    , fee_histogram_(nullptr)
{
}

TransactionScorer::ScoredTransaction TransactionScorer::ScoreTransaction(
    const uint256& txid,
    uint64_t fee_rate,
    uint64_t entry_time_ms,
    uint64_t current_time_ms
) const {
    ScoredTransaction scored;
    scored.txid = txid;
    scored.fee_rate = fee_rate;
    scored.entry_time_ms = entry_time_ms;

    // Calculate age in seconds
    uint64_t age_ms = (current_time_ms > entry_time_ms) ?
                      (current_time_ms - entry_time_ms) : 0;
    uint64_t age_seconds = age_ms / 1000;

    // Calculate mining probability (age + fee percentile)
    scored.mining_probability = CalculateMiningProbability(fee_rate, age_seconds);

    // Calculate compact reconstructability (propagation age + bias)
    scored.compact_recon = CalculateCompactReconstructability(age_seconds);

    // Combined score: fee_rate × mining_probability × compact_recon
    // Fee rate is the dominant factor, others are multipliers (0-1 range)
    scored.score = static_cast<double>(fee_rate) *
                   scored.mining_probability *
                   scored.compact_recon;

    return scored;
}

std::vector<TransactionScorer::ScoredTransaction> TransactionScorer::ScoreTransactions(
    const std::vector<std::tuple<uint256, uint64_t, uint64_t>>& transactions,
    uint64_t current_time_ms
) const {
    std::vector<ScoredTransaction> scored;
    scored.reserve(transactions.size());

    for (const auto& [txid, fee_rate, entry_time] : transactions) {
        scored.push_back(ScoreTransaction(txid, fee_rate, entry_time, current_time_ms));
    }

    // Sort by score descending (highest score first)
    std::sort(scored.begin(), scored.end());

    return scored;
}

double TransactionScorer::CalculateMiningProbability(
    uint64_t fee_rate,
    uint64_t age_seconds
) const {
    // Factor 1: Age-based propagation (0.0-1.0)
    double age_factor = 0.0;

    if (age_seconds >= 300) {
        // 5+ minutes: fully propagated
        age_factor = 1.0;
    } else if (age_seconds >= 60) {
        // 1-5 minutes: ramp from 0.2 to 1.0
        double progress = static_cast<double>(age_seconds - 60) / 240.0;  // 240s = 4 minutes
        age_factor = 0.2 + (progress * 0.8);
    } else {
        // 0-60 seconds: ramp from 0.0 to 0.2
        age_factor = static_cast<double>(age_seconds) / 300.0;
    }

    // Factor 2: Fee percentile (0.0-1.0)
    double fee_percentile = 0.5;  // Default: median
    if (fee_histogram_ && fee_histogram_->GetTotalCount() > 0) {
        fee_percentile = fee_histogram_->GetPercentile(fee_rate);
    }

    // Hybrid: 50% age, 50% fee percentile
    double probability = 0.5 * age_factor + 0.5 * fee_percentile;

    return std::min(1.0, std::max(0.0, probability));
}

double TransactionScorer::CalculateCompactReconstructability(
    uint64_t age_seconds
) const {
    // Base reconstructability: propagation age
    // 0 seconds = 50% minimum (miner always has it, plus immediate broadcast to some peers)
    // 5 minutes = 100% of peers have it (assume full propagation)
    double base = 0.5 + (0.5 * std::min(1.0, static_cast<double>(age_seconds) / 300.0));

    // Bonus from context: compact-friendly bias
    // If compact blocks are successful, prefer well-propagated transactions
    double bias = context_.GetCompactFriendlyBias();
    double bonus = bias * (base - 0.5);  // Bonus scales with propagation above minimum

    // Total reconstructability (can exceed 1.0 for strong preference)
    double reconstructability = base + bonus;

    return reconstructability;
}

} // namespace dinero
