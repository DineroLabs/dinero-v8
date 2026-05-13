#pragma once

/**
 * CT Selection Policy for Mining Templates
 *
 * Confidential transaction-specific policies for block template creation:
 * - Weight multiplier (proofs are larger, affect block space)
 * - Minimum fee rate (ensure CT txs pay for verification cost)
 * - Per-block CT limits (prevent DoS via expensive proofs)
 * - Batch verification optimization hints
 *
 * Usage:
 *   CTSelectionPolicy policy(config);
 *   if (policy.CheckPolicy(tx, height, ct_count, proof_bytes).acceptable) {
 *       uint64_t weight = policy.CalculateCTWeight(tx);
 *       double fee_rate = policy.CalculateAdjustedFeeRate(tx);
 *   }
 */

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include "wallet/transaction.h"

namespace dinero {
namespace mining {

/**
 * Configuration for CT transaction selection in mining
 */
struct CTSelectionConfig {
    // Weight multiplier for CT transactions (default: 1.5x)
    // Reflects additional verification cost of Bulletproofs
    // CT transactions consume more "effective" block space
    double ct_weight_multiplier = 1.5;

    // Minimum fee rate for CT transactions (sat/vbyte)
    // Higher than transparent minimum to cover proof verification cost
    // Bulletproof verification is ~1-2ms per proof
    uint64_t ct_min_fee_rate = 2;

    // Maximum CT transactions per block
    // Prevents DoS via many expensive proof verifications
    // 50 CTs * ~2ms = ~100ms additional validation time
    size_t max_ct_per_block = 50;

    // Maximum CT proof data per block (bytes)
    // Limits total proof data to prevent oversized blocks
    // ~50KB = ~70 CT outputs (714 bytes per proof average)
    size_t max_ct_proof_data = 50000;

    // Enable batch verification optimization grouping
    // When true, policy hints at grouping CTs for batch speedup
    bool enable_batch_optimization = true;

    // Minimum batch size for batch verification benefit
    // Below this, individual verification may be faster
    size_t batch_size_threshold = 5;

    // Weight factor for CT proof bytes
    // Applied as: proof_bytes * ct_proof_weight_factor
    // Default 4 matches SegWit witness discount inverse
    uint32_t ct_proof_weight_factor = 4;
};

/**
 * Result of policy check
 */
struct CTPolicyResult {
    bool acceptable = true;
    std::string rejection_reason;

    static CTPolicyResult Accept() {
        return {true, ""};
    }

    static CTPolicyResult Reject(const std::string& reason) {
        return {false, reason};
    }
};

/**
 * CT-specific weight calculation result
 */
struct CTWeightInfo {
    uint64_t base_weight;           // Standard transaction weight
    uint64_t proof_weight;          // Additional weight from proofs
    uint64_t total_weight;          // Combined weight
    size_t proof_bytes;             // Total proof data size
    size_t confidential_outputs;    // Number of CT outputs
};

/**
 * CT Selection Policy
 *
 * Enforces CT-specific rules during block template creation.
 * Used by BlockAssembler to filter and weight CT transactions.
 */
class CTSelectionPolicy {
public:
    explicit CTSelectionPolicy(const CTSelectionConfig& config = {});
    ~CTSelectionPolicy() = default;

    // ========================================================================
    // Policy Checks
    // ========================================================================

    /**
     * Check if CT transaction meets policy requirements
     *
     * @param tx Transaction to check
     * @param current_height Current block height (for activation check)
     * @param current_ct_count Number of CTs already selected for this block
     * @param current_ct_proof_bytes Total proof bytes already in block
     * @return Policy result with acceptance status and reason if rejected
     */
    CTPolicyResult CheckPolicy(
        const Transaction& tx,
        uint32_t current_height,
        size_t current_ct_count,
        size_t current_ct_proof_bytes
    ) const;

    /**
     * Check if a transaction has confidential outputs
     *
     * @param tx Transaction to check
     * @return true if transaction has at least one confidential output
     */
    bool HasConfidentialOutputs(const Transaction& tx) const;

    // ========================================================================
    // Weight Calculation
    // ========================================================================

    /**
     * Calculate effective weight for CT transaction
     *
     * Weight includes base transaction weight plus additional weight
     * for proof verification overhead.
     *
     * @param tx Transaction to calculate weight for
     * @return Total effective weight
     */
    uint64_t CalculateCTWeight(const Transaction& tx) const;

    /**
     * Get detailed weight breakdown
     *
     * @param tx Transaction to analyze
     * @return Detailed weight information
     */
    CTWeightInfo GetWeightInfo(const Transaction& tx) const;

    /**
     * Calculate base transaction weight (without CT multiplier)
     *
     * @param tx Transaction to calculate
     * @return Base weight in weight units
     */
    static uint64_t CalculateBaseWeight(const Transaction& tx);

    // ========================================================================
    // Fee Rate Calculation
    // ========================================================================

    /**
     * Calculate adjusted fee rate accounting for CT overhead
     *
     * Fee rate is calculated using effective CT weight, not raw size.
     * This ensures CT transactions pay fairly for block space.
     *
     * @param tx Transaction to calculate fee rate for
     * @return Adjusted fee rate (sat/weight unit)
     */
    double CalculateAdjustedFeeRate(const Transaction& tx) const;

    /**
     * Check if transaction meets minimum CT fee rate
     *
     * @param tx Transaction to check
     * @return true if fee rate >= ct_min_fee_rate
     */
    bool MeetsMinimumFeeRate(const Transaction& tx) const;

    // ========================================================================
    // Batch Optimization
    // ========================================================================

    /**
     * Check if batch verification optimization should be used
     *
     * @param ct_count Number of CT transactions in block
     * @return true if batch verification is recommended
     */
    bool ShouldUseBatchVerification(size_t ct_count) const;

    /**
     * Sort transactions for optimal batch verification
     *
     * Groups CT transactions together for efficient batch processing.
     * Transparent transactions are placed before or after CT batch.
     *
     * @param txs Transactions to sort
     * @return Sorted transactions (CT grouped for batching)
     */
    std::vector<Transaction> OptimizeForBatchVerification(
        std::vector<Transaction> txs
    ) const;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Get current configuration
     */
    const CTSelectionConfig& GetConfig() const { return config_; }

    /**
     * Update configuration at runtime
     */
    void SetConfig(const CTSelectionConfig& config) { config_ = config; }

    /**
     * Update individual settings
     */
    void SetCTWeightMultiplier(double multiplier) {
        config_.ct_weight_multiplier = multiplier;
    }

    void SetMaxCTPerBlock(size_t max) {
        config_.max_ct_per_block = max;
    }

    void SetMaxCTProofData(size_t max) {
        config_.max_ct_proof_data = max;
    }

    void SetMinFeeRate(uint64_t rate) {
        config_.ct_min_fee_rate = rate;
    }

    void SetBatchOptimization(bool enabled) {
        config_.enable_batch_optimization = enabled;
    }

private:
    CTSelectionConfig config_;

    /**
     * Get total proof bytes from transaction
     */
    size_t GetTotalProofBytes(const Transaction& tx) const;

    /**
     * Get count of confidential outputs
     */
    size_t GetConfidentialOutputCount(const Transaction& tx) const;

    /**
     * Check activation height
     */
    bool IsCTActivated(uint32_t height) const;
};

} // namespace mining
} // namespace dinero
