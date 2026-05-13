#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>

namespace dinero {
namespace policy {

/**
 * Fee estimation target (blocks to confirmation)
 */
enum class FeeTarget {
    IMMEDIATE = 1,    // Next block
    FAST = 3,         // ~3 blocks
    NORMAL = 6,       // ~6 blocks  
    SLOW = 12,        // ~12 blocks
    ECONOMY = 24      // ~24 blocks
};

/**
 * Fee rate estimation result
 */
struct FeeEstimate {
    uint64_t fee_rate = 0;        // una per KB
    double confidence = 0.0;      // 0.0 to 1.0
    uint32_t sample_size = 0;     // Number of transactions in sample
    bool is_sufficient_data = false;
};

/**
 * CT (Confidential Transaction) fee estimation result
 * Phase 3: CT Fee Market Tuning
 */
struct CTFeeEstimate {
    uint64_t base_fee_rate = 0;       // Base transparent fee rate (sat/KB)
    uint64_t ct_adjusted_rate = 0;    // Fee rate adjusted for CT overhead (sat/KB)
    double ct_multiplier = 1.5;       // Applied weight multiplier
    uint32_t ct_proof_weight = 4;     // Proof weight factor
    size_t estimated_proof_bytes = 0; // Estimated proof size in bytes
    double confidence = 0.0;          // 0.0 to 1.0
    bool is_sufficient_data = false;

    // Convenience method to get fee for transaction size
    uint64_t estimateFeeForSize(size_t tx_bytes, size_t proof_bytes = 0) const {
        if (proof_bytes == 0) proof_bytes = estimated_proof_bytes;
        // CT effective weight = base_bytes * 4 * multiplier + proof_bytes * proof_weight
        uint64_t effective_weight = static_cast<uint64_t>(
            tx_bytes * 4 * ct_multiplier + proof_bytes * ct_proof_weight
        );
        // fee = (weight * rate_per_kb) / (1000 * 4) [weight units]
        return (effective_weight * ct_adjusted_rate) / 4000;
    }
};

/**
 * Transaction confirmation tracking
 */
struct ConfirmationData {
    std::string txid;
    uint64_t fee_rate;           // una per KB
    uint32_t entry_height;       // Block height when entered mempool
    uint32_t confirm_height;     // Block height when confirmed
    uint64_t entry_time;         // Unix timestamp when entered mempool
    uint64_t confirm_time;       // Unix timestamp when confirmed
};

/**
 * EWMA (Exponentially Weighted Moving Average) bucket for fee estimation
 */
class EWMABucket {
public:
    explicit EWMABucket(double decay_factor = 0.998);
    
    void addSample(uint64_t fee_rate, uint32_t blocks_to_confirm);
    FeeEstimate getEstimate(FeeTarget target) const;
    void decay();
    
    // Statistics
    uint32_t getSampleCount() const { return sample_count_; }
    double getAverageFeeRate() const { return avg_fee_rate_; }
    
private:
    double decay_factor_;
    double avg_fee_rate_;
    double avg_blocks_;
    uint32_t sample_count_;
    uint64_t last_update_time_;
    
    mutable std::mutex mutex_;
};

/**
 * Rolling fee estimator using EWMA buckets
 */
class FeeEstimator {
public:
    explicit FeeEstimator(uint32_t min_samples = 10);
    
    /**
     * Record a transaction entering the mempool
     */
    void addMempoolTransaction(const std::string& txid, 
                              uint64_t fee_rate,
                              uint32_t current_height);
    
    /**
     * Record a transaction being confirmed
     */
    void addConfirmedTransaction(const std::string& txid,
                                uint32_t confirm_height,
                                uint64_t confirm_time);
    
    /**
     * Get fee estimate for target confirmation time
     */
    FeeEstimate estimateFee(FeeTarget target) const;

    /**
     * Get CT fee estimate for target confirmation time
     * Phase 3: CT Fee Market Tuning
     *
     * CT transactions require higher fee rates due to:
     * - Bulletproof range proof verification overhead (~1-2ms per proof)
     * - Larger transaction weight from proof data
     *
     * @param target       Confirmation target (blocks)
     * @param proof_bytes  Estimated proof size (default ~5000 for 7 outputs)
     * @return             CT-adjusted fee estimate
     */
    CTFeeEstimate estimateCTFee(FeeTarget target, size_t proof_bytes = 5000) const;

    /**
     * Get all available fee estimates
     */
    std::map<FeeTarget, FeeEstimate> getAllEstimates() const;
    
    /**
     * Update estimator (call periodically to decay old data)
     */
    void update();
    
    /**
     * Get statistics
     */
    struct Statistics {
        uint32_t total_samples;
        uint32_t pending_transactions;
        uint64_t oldest_sample_age;
        double average_confirmation_time;
    };
    
    Statistics getStatistics() const;
    
    // Configuration
    void setMinSamples(uint32_t min_samples) { min_samples_ = min_samples; }
    uint32_t getMinSamples() const { return min_samples_; }

    // CT Fee Policy Configuration (Phase 3)
    void setMinCTFeeRate(uint64_t rate) { min_ct_fee_rate_ = rate; }
    uint64_t getMinCTFeeRate() const { return min_ct_fee_rate_; }

    void setCTWeightMultiplier(double multiplier) { ct_weight_multiplier_ = multiplier; }
    double getCTWeightMultiplier() const { return ct_weight_multiplier_; }

    void setCTProofWeightFactor(uint32_t factor) { ct_proof_weight_factor_ = factor; }
    uint32_t getCTProofWeightFactor() const { return ct_proof_weight_factor_; }

private:
    // Fee rate buckets (different ranges)
    std::vector<std::unique_ptr<EWMABucket>> fee_buckets_;
    
    // Pending transactions (in mempool)
    std::map<std::string, ConfirmationData> pending_txs_;
    
    // Configuration
    uint32_t min_samples_;

    // CT Fee Policy Configuration (Phase 3)
    uint64_t min_ct_fee_rate_ = 2000;       // 2 sat/vB = 2000 sat/KB (2x transparent minimum)
    double ct_weight_multiplier_ = 1.5;     // CT verification overhead multiplier
    uint32_t ct_proof_weight_factor_ = 4;   // Proof weight factor (similar to SegWit witness discount inverse)

    // Thread safety
    mutable std::mutex mutex_;
    
    // Helper methods
    size_t getFeeRateBucket(uint64_t fee_rate) const;
    void processConfirmation(const ConfirmationData& data);
    void cleanupOldData();
    
    // Fee rate bucket boundaries (una per KB)
    static const std::vector<uint64_t> BUCKET_BOUNDARIES;
};

/**
 * Smart fee recommendation engine
 */
class SmartFeeRecommender {
public:
    explicit SmartFeeRecommender(std::shared_ptr<FeeEstimator> estimator);
    
    /**
     * Get recommended fee for transaction priority
     */
    struct FeeRecommendation {
        uint64_t fee_rate;           // una per KB
        FeeTarget target;            // Recommended target
        uint32_t estimated_blocks;   // Estimated blocks to confirmation
        double confidence;           // Confidence in estimate
        std::string reasoning;       // Human-readable explanation
    };
    
    FeeRecommendation recommendFee(const std::string& priority = std::string("normal")) const;
    
    /**
     * Get fee for specific target with fallback logic
     */
    uint64_t getFeeForTarget(FeeTarget target) const;
    
private:
    std::shared_ptr<FeeEstimator> estimator_;
    
    // Fallback fee rates when insufficient data
    static const std::map<FeeTarget, uint64_t> FALLBACK_RATES;
};

} // namespace policy
} // namespace dinero
