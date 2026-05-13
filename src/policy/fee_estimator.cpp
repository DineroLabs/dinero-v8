#include "policy/fee_estimator.h"
#include "common/logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace dinero {
namespace policy {

// Fee rate bucket boundaries (una per KB)
const std::vector<uint64_t> FeeEstimator::BUCKET_BOUNDARIES = {
    1000,    // 1 sat/KB
    2000,    // 2 sat/KB
    5000,    // 5 sat/KB
    10000,   // 10 sat/KB
    20000,   // 20 sat/KB
    50000,   // 50 sat/KB
    100000,  // 100 sat/KB
    200000,  // 200 sat/KB
    500000,  // 500 sat/KB
    1000000  // 1000 sat/KB
};

// Fallback rates when insufficient data (una per KB)
const std::map<FeeTarget, uint64_t> SmartFeeRecommender::FALLBACK_RATES = {
    {FeeTarget::IMMEDIATE, 100000},  // 100 sat/KB
    {FeeTarget::FAST, 50000},        // 50 sat/KB
    {FeeTarget::NORMAL, 20000},      // 20 sat/KB
    {FeeTarget::SLOW, 10000},        // 10 sat/KB
    {FeeTarget::ECONOMY, 5000}       // 5 sat/KB
};

// EWMABucket implementation

EWMABucket::EWMABucket(double decay_factor)
    : decay_factor_(decay_factor)
    , avg_fee_rate_(0.0)
    , avg_blocks_(0.0)
    , sample_count_(0)
    , last_update_time_(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) {}

void EWMABucket::addSample(uint64_t fee_rate, uint32_t blocks_to_confirm) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (sample_count_ == 0) {
        avg_fee_rate_ = static_cast<double>(fee_rate);
        avg_blocks_ = static_cast<double>(blocks_to_confirm);
    } else {
        // EWMA update
        double alpha = 1.0 - decay_factor_;
        avg_fee_rate_ = decay_factor_ * avg_fee_rate_ + alpha * fee_rate;
        avg_blocks_ = decay_factor_ * avg_blocks_ + alpha * blocks_to_confirm;
    }
    
    sample_count_++;
    last_update_time_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

FeeEstimate EWMABucket::getEstimate(FeeTarget target) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    FeeEstimate estimate;
    estimate.sample_size = sample_count_;
    
    if (sample_count_ == 0) {
        return estimate; // No data
    }
    
    uint32_t target_blocks = static_cast<uint32_t>(target);
    
    // Calculate confidence based on how close our average is to target
    double blocks_diff = std::abs(avg_blocks_ - target_blocks);
    estimate.confidence = std::max(0.0, 1.0 - (blocks_diff / target_blocks));
    
    // Adjust fee rate based on target vs average
    double adjustment = 1.0;
    if (avg_blocks_ > target_blocks) {
        // Need higher fee for faster confirmation
        adjustment = 1.0 + (avg_blocks_ - target_blocks) * 0.1;
    } else if (avg_blocks_ < target_blocks) {
        // Can use lower fee for slower confirmation
        adjustment = std::max(0.5, 1.0 - (target_blocks - avg_blocks_) * 0.05);
    }
    
    estimate.fee_rate = static_cast<uint64_t>(avg_fee_rate_ * adjustment);
    estimate.is_sufficient_data = sample_count_ >= 5; // Minimum samples
    
    return estimate;
}

void EWMABucket::decay() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Decay based on time elapsed
    uint64_t time_diff = now - last_update_time_;
    if (time_diff > 0) {
        double decay_periods = time_diff / 600.0; // 10-minute periods
        double total_decay = std::pow(decay_factor_, decay_periods);
        
        avg_fee_rate_ *= total_decay;
        avg_blocks_ *= total_decay;
        sample_count_ = static_cast<uint32_t>(sample_count_ * total_decay);
        
        last_update_time_ = now;
    }
}

// FeeEstimator implementation

FeeEstimator::FeeEstimator(uint32_t min_samples) : min_samples_(min_samples) {
    // Initialize buckets for each fee rate range
    for (size_t i = 0; i <= BUCKET_BOUNDARIES.size(); ++i) {
        fee_buckets_.push_back(std::make_unique<EWMABucket>());
    }
}

void FeeEstimator::addMempoolTransaction(const std::string& txid,
                                        uint64_t fee_rate,
                                        uint32_t current_height) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ConfirmationData data;
    data.txid = txid;
    data.fee_rate = fee_rate;
    data.entry_height = current_height;
    data.entry_time = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    pending_txs_[txid] = data;
    
    dinero::g_logger.debug("Added mempool tx " + txid + " with fee rate " + 
                          std::to_string(fee_rate) + " sat/KB");
}

void FeeEstimator::addConfirmedTransaction(const std::string& txid,
                                          uint32_t confirm_height,
                                          uint64_t confirm_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = pending_txs_.find(txid);
    if (it == pending_txs_.end()) {
        return; // Transaction not tracked
    }
    
    ConfirmationData& data = it->second;
    data.confirm_height = confirm_height;
    data.confirm_time = confirm_time;
    
    processConfirmation(data);
    pending_txs_.erase(it);
    
    dinero::g_logger.debug("Confirmed tx " + txid + " after " + 
                          std::to_string(confirm_height - data.entry_height) + " blocks");
}

FeeEstimate FeeEstimator::estimateFee(FeeTarget target) const {
    std::lock_guard<std::mutex> lock(mutex_);

    FeeEstimate best_estimate;
    double best_confidence = 0.0;

    // Check all buckets and find the best estimate
    for (const auto& bucket : fee_buckets_) {
        FeeEstimate estimate = bucket->getEstimate(target);

        if (estimate.sample_size >= min_samples_ &&
            estimate.confidence > best_confidence) {
            best_estimate = estimate;
            best_confidence = estimate.confidence;
        }
    }

    return best_estimate;
}

CTFeeEstimate FeeEstimator::estimateCTFee(FeeTarget target, size_t proof_bytes) const {
    // Phase 3: CT Fee Market Tuning
    // CT transactions require higher fee rates due to verification overhead

    CTFeeEstimate ct_estimate;

    // Get base transparent fee estimate
    FeeEstimate base = estimateFee(target);

    ct_estimate.base_fee_rate = base.fee_rate;
    ct_estimate.confidence = base.confidence;
    ct_estimate.is_sufficient_data = base.is_sufficient_data;
    ct_estimate.estimated_proof_bytes = proof_bytes;
    ct_estimate.ct_multiplier = ct_weight_multiplier_;
    ct_estimate.ct_proof_weight = ct_proof_weight_factor_;

    // Use fallback if no base estimate available
    if (ct_estimate.base_fee_rate == 0) {
        // Conservative fallback based on target
        switch (target) {
            case FeeTarget::IMMEDIATE:
                ct_estimate.base_fee_rate = 100000;  // 100 sat/KB
                break;
            case FeeTarget::FAST:
                ct_estimate.base_fee_rate = 50000;   // 50 sat/KB
                break;
            case FeeTarget::NORMAL:
                ct_estimate.base_fee_rate = 20000;   // 20 sat/KB
                break;
            case FeeTarget::SLOW:
                ct_estimate.base_fee_rate = 10000;   // 10 sat/KB
                break;
            case FeeTarget::ECONOMY:
                ct_estimate.base_fee_rate = 5000;    // 5 sat/KB
                break;
        }
    }

    // Calculate CT-adjusted fee rate
    // CT transactions need higher fee rates to achieve the same effective priority
    // because they have higher verification costs
    //
    // Effective rate = base_rate * ct_weight_multiplier
    // This ensures CT transactions pay proportionally more for their verification overhead
    ct_estimate.ct_adjusted_rate = static_cast<uint64_t>(
        ct_estimate.base_fee_rate * ct_weight_multiplier_
    );

    // Ensure minimum CT fee rate
    if (ct_estimate.ct_adjusted_rate < min_ct_fee_rate_) {
        ct_estimate.ct_adjusted_rate = min_ct_fee_rate_;
    }

    dinero::g_logger.debug(
        "CT fee estimate: base=" + std::to_string(ct_estimate.base_fee_rate) +
        " sat/KB, adjusted=" + std::to_string(ct_estimate.ct_adjusted_rate) +
        " sat/KB, multiplier=" + std::to_string(ct_weight_multiplier_) +
        ", proof_bytes=" + std::to_string(proof_bytes)
    );

    return ct_estimate;
}

std::map<FeeTarget, FeeEstimate> FeeEstimator::getAllEstimates() const {
    std::map<FeeTarget, FeeEstimate> estimates;
    
    estimates[FeeTarget::IMMEDIATE] = estimateFee(FeeTarget::IMMEDIATE);
    estimates[FeeTarget::FAST] = estimateFee(FeeTarget::FAST);
    estimates[FeeTarget::NORMAL] = estimateFee(FeeTarget::NORMAL);
    estimates[FeeTarget::SLOW] = estimateFee(FeeTarget::SLOW);
    estimates[FeeTarget::ECONOMY] = estimateFee(FeeTarget::ECONOMY);
    
    return estimates;
}

void FeeEstimator::update() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Decay all buckets
    for (auto& bucket : fee_buckets_) {
        bucket->decay();
    }
    
    // Clean up old pending transactions
    cleanupOldData();
}

FeeEstimator::Statistics FeeEstimator::getStatistics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Statistics stats;
    stats.pending_transactions = pending_txs_.size();
    stats.total_samples = 0;
    
    for (const auto& bucket : fee_buckets_) {
        stats.total_samples += bucket->getSampleCount();
    }
    
    // Calculate oldest sample age
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    stats.oldest_sample_age = 0;
    if (!pending_txs_.empty()) {
        uint64_t oldest = now;
        for (const auto& [txid, data] : pending_txs_) {
            oldest = std::min(oldest, data.entry_time);
        }
        stats.oldest_sample_age = now - oldest;
    }
    
    // Calculate average confirmation time (simplified)
    stats.average_confirmation_time = 600.0; // 10 minutes default
    
    return stats;
}

// Private methods

size_t FeeEstimator::getFeeRateBucket(uint64_t fee_rate) const {
    for (size_t i = 0; i < BUCKET_BOUNDARIES.size(); ++i) {
        if (fee_rate <= BUCKET_BOUNDARIES[i]) {
            return i;
        }
    }
    return BUCKET_BOUNDARIES.size(); // Highest bucket
}

void FeeEstimator::processConfirmation(const ConfirmationData& data) {
    uint32_t blocks_to_confirm = data.confirm_height - data.entry_height;
    
    // Add to appropriate bucket
    size_t bucket_idx = getFeeRateBucket(data.fee_rate);
    if (bucket_idx < fee_buckets_.size()) {
        fee_buckets_[bucket_idx]->addSample(data.fee_rate, blocks_to_confirm);
    }
}

void FeeEstimator::cleanupOldData() {
    uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Remove transactions older than 24 hours
    auto it = pending_txs_.begin();
    while (it != pending_txs_.end()) {
        if (now - it->second.entry_time > 86400) { // 24 hours
            it = pending_txs_.erase(it);
        } else {
            ++it;
        }
    }
}

// SmartFeeRecommender implementation

SmartFeeRecommender::SmartFeeRecommender(std::shared_ptr<FeeEstimator> estimator)
    : estimator_(estimator) {}

SmartFeeRecommender::FeeRecommendation SmartFeeRecommender::recommendFee(
    const std::string& priority) const {
    
    FeeRecommendation rec;
    
    // Map priority string to target
    if (priority == "immediate" || priority == "urgent") {
        rec.target = FeeTarget::IMMEDIATE;
    } else if (priority == "fast" || priority == "high") {
        rec.target = FeeTarget::FAST;
    } else if (priority == "normal" || priority == "medium") {
        rec.target = FeeTarget::NORMAL;
    } else if (priority == "slow" || priority == "low") {
        rec.target = FeeTarget::SLOW;
    } else {
        rec.target = FeeTarget::ECONOMY;
    }
    
    // Get estimate from fee estimator
    FeeEstimate estimate = estimator_->estimateFee(rec.target);
    
    if (estimate.is_sufficient_data && estimate.confidence > 0.5) {
        rec.fee_rate = estimate.fee_rate;
        rec.confidence = estimate.confidence;
        rec.estimated_blocks = static_cast<uint32_t>(rec.target);
        rec.reasoning = "Based on " + std::to_string(estimate.sample_size) + 
                       " recent transactions";
    } else {
        // Use fallback rate
        rec.fee_rate = FALLBACK_RATES.at(rec.target);
        rec.confidence = 0.3; // Low confidence for fallback
        rec.estimated_blocks = static_cast<uint32_t>(rec.target);
        rec.reasoning = "Insufficient data, using conservative estimate";
    }
    
    return rec;
}

uint64_t SmartFeeRecommender::getFeeForTarget(FeeTarget target) const {
    FeeEstimate estimate = estimator_->estimateFee(target);
    
    if (estimate.is_sufficient_data) {
        return estimate.fee_rate;
    }
    
    return FALLBACK_RATES.at(target);
}

} // namespace policy
} // namespace dinero
