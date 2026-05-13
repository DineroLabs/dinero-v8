#include "mempool/policy_engine.h"
#include "policy/fee_estimator.h"
#include <algorithm>
#include <cmath>

namespace dinero {
namespace mempool {

PolicyEngine::PolicyEngine() {
    // Initialize fee estimator
    fee_estimator_ = std::make_shared<policy::FeeEstimator>(5); // Minimum 5 samples
    smart_recommender_ = std::make_unique<policy::SmartFeeRecommender>(fee_estimator_);
    
    // Initialize with default fee estimates
    updateFeeEstimates();
}

FeeEstimate PolicyEngine::estimateFee(uint32_t target_blocks) const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Update fee estimator data
    fee_estimator_->update();
    
    // Map target blocks to FeeTarget enum
    policy::FeeTarget target;
    if (target_blocks <= 1) {
        target = policy::FeeTarget::IMMEDIATE;
    } else if (target_blocks <= 3) {
        target = policy::FeeTarget::FAST;
    } else if (target_blocks <= 6) {
        target = policy::FeeTarget::NORMAL;
    } else if (target_blocks <= 12) {
        target = policy::FeeTarget::SLOW;
    } else {
        target = policy::FeeTarget::ECONOMY;
    }
    
    // Get estimate from fee estimator
    policy::FeeEstimate policy_estimate = fee_estimator_->estimateFee(target);
    
    // Convert to our FeeEstimate format
    FeeEstimate result;
    result.blocks = target_blocks;
    result.confidence = 0.8; // Default confidence
    
    if (policy_estimate.is_sufficient_data && policy_estimate.fee_rate > 0) {
        result.fee_rate = policy_estimate.fee_rate;
        result.confidence = std::min(1.0, policy_estimate.sample_size / 10.0); // Scale confidence by sample size
    } else {
        // Use smart recommender fallback
        auto recommendation = smart_recommender_->recommendFee("normal");
        result.fee_rate = recommendation.fee_rate;
        result.confidence = recommendation.confidence;
    }
    
    return result;
}

std::vector<FeeEstimate> PolicyEngine::getFeeEstimates() const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    // Update fee estimator data
    fee_estimator_->update();
    
    // Get all estimates from the fee estimator
    auto policy_estimates = fee_estimator_->getAllEstimates();
    
    std::vector<FeeEstimate> result;
    
    // Convert policy estimates to our format
    std::vector<uint32_t> targets = {1, 3, 6, 12, 24, 48, 144, 504, 1008};
    for (uint32_t target : targets) {
        FeeEstimate estimate = estimateFee(target);
        result.push_back(estimate);
    }
    
    return result;
}

bool PolicyEngine::validateTransaction(const Transaction& tx) const {
    // Basic format validation (Phase M.1.A: txid is now uint256, check IsNull instead of empty)
    if (tx.txid.IsNull() || tx.size == 0) {
        return false;
    }
    
    // Check transaction size limits
    if (tx.size > 100000) {  // 100KB max transaction size
        return false;
    }
    
    // Check weight limits (for segwit compatibility)
    if (tx.weight > 400000) {  // 400K weight units max
        return false;
    }
    
    // Validate fee amount
    if (tx.fee < 0) {
        return false;
    }
    
    // Check minimum fee rate
    double fee_rate = static_cast<double>(tx.fee) / tx.size;
    if (fee_rate < min_relay_fee_ / 1000.0) {  // Convert from sat/kB to sat/B
        return false;
    }
    
    // Check maximum fee rate (prevent accidental high fees)
    if (fee_rate > 1000.0) {  // 1000 sat/B max (very high fee protection)
        return false;
    }
    
    // Check for dust outputs (if we have output information)
    // This would require extending the Transaction struct to include outputs
    
    // Check mempool size limits
    std::lock_guard<std::mutex> lock(mtx_);
    if (getMempoolBytes() + tx.size > max_mempool_size_) {
        return false;
    }
    
    // Check for double spending (basic check)
    if (hasTransaction(tx.txid)) {
        return false;  // Transaction already in mempool
    }
    
    // Additional validation checks would go here:
    // - Input validation (UTXOs exist and are unspent)
    // - Signature verification
    // - Script validation
    // - Timelock validation
    
    return true;
}

bool PolicyEngine::isRBFEnabled(const uint256& txid) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = mempool_.find(txid);
    return it != mempool_.end() && it->second.rbf_enabled;
}

void PolicyEngine::addTransaction(const Transaction& tx) {
    std::lock_guard<std::mutex> lock(mtx_);
    mempool_[tx.txid] = tx;
    
    // Add transaction to fee estimator for tracking
    if (fee_estimator_ && tx.size > 0) {
        uint64_t fee_rate = (tx.fee * 1000) / tx.size; // Convert to sat/KB
        fee_estimator_->addMempoolTransaction(tx.txid, fee_rate, current_height_);
    }
}

void PolicyEngine::removeTransaction(const uint256& txid) {
    std::lock_guard<std::mutex> lock(mtx_);
    mempool_.erase(txid);
}

void PolicyEngine::confirmTransaction(const uint256& txid, uint32_t confirm_height) {
    std::lock_guard<std::mutex> lock(mtx_);

    // Notify fee estimator of confirmation
    if (fee_estimator_) {
        uint64_t confirm_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        fee_estimator_->addConfirmedTransaction(txid, confirm_height, confirm_time);
    }

    // Remove from mempool
    mempool_.erase(txid);
}

void PolicyEngine::updateCurrentHeight(uint32_t height) {
    std::lock_guard<std::mutex> lock(mtx_);
    current_height_ = height;
}

bool PolicyEngine::hasTransaction(const uint256& txid) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return mempool_.find(txid) != mempool_.end();
}

size_t PolicyEngine::getMempoolCount() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return mempool_.size();
}

uint64_t PolicyEngine::getMempoolBytes() const {
    std::lock_guard<std::mutex> lock(mtx_);
    uint64_t total_bytes = 0;
    for (const auto& pair : mempool_) {
        total_bytes += pair.second.size;
    }
    return total_bytes;
}

din::Json PolicyEngine::toJson() const {
    std::lock_guard<std::mutex> lock(mtx_);
    
    din::Json result;
    result["size"] = getMempoolCount();
    result["bytes"] = getMempoolBytes();
    result["usage"] = getMempoolBytes();
    result["maxmempool"] = max_mempool_size_;
    result["minrelaytxfee"] = static_cast<double>(min_relay_fee_) / 100000000.0;  // Convert to DIN
    
    // Add fee estimates
    din::Json estimates = din::Json::array();
    for (const auto& est : cached_estimates_) {
        din::Json estimate;
        estimate["blocks"] = est.blocks;
        estimate["feerate"] = est.fee_rate;
        estimate["confidence"] = est.confidence;
        estimates.append(estimate);
    }
    result["fee_estimates"] = estimates;
    
    return result;
}

void PolicyEngine::updateFeeEstimates() const {
    // Update fee estimator data
    if (fee_estimator_) {
        fee_estimator_->update();
    }
    
    last_estimate_update_ = std::chrono::system_clock::now();
}

double PolicyEngine::calculateFeeRate(uint32_t target_blocks) const {
    // Simple fee estimation algorithm
    // In production, this would analyze historical block data
    
    double base_rate = static_cast<double>(min_relay_fee_) / 1000.0;  // Convert to sat/B
    
    if (target_blocks <= 1) {
        return base_rate * 10.0;  // High priority
    } else if (target_blocks <= 3) {
        return base_rate * 5.0;   // Medium-high priority
    } else if (target_blocks <= 6) {
        return base_rate * 2.0;   // Medium priority
    } else if (target_blocks <= 12) {
        return base_rate * 1.5;   // Low-medium priority
    } else {
        return base_rate;         // Low priority
    }
}

} // namespace mempool
} // namespace dinero
