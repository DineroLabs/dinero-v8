/**
 * CT Selection Policy Implementation
 *
 * Confidential transaction-specific policies for mining template creation.
 */

#include "mining/ct_selection_policy.h"
#include "consensus/chainparams.h"
#include <algorithm>

namespace dinero {
namespace mining {

CTSelectionPolicy::CTSelectionPolicy(const CTSelectionConfig& config)
    : config_(config)
{
}

// ============================================================================
// Policy Checks
// ============================================================================

CTPolicyResult CTSelectionPolicy::CheckPolicy(
    const Transaction& tx,
    uint32_t current_height,
    size_t current_ct_count,
    size_t current_ct_proof_bytes
) const {
    // Skip non-CT transactions - always acceptable under CT policy
    if (!HasConfidentialOutputs(tx)) {
        return CTPolicyResult::Accept();
    }

    // Check activation height
    if (!IsCTActivated(current_height)) {
        return CTPolicyResult::Reject(
            "CT not activated at height " + std::to_string(current_height)
        );
    }

    // Check kill switch (via chain params)
    const auto& params = dinero::Params();
    if (params.disable_confidential_transactions) {
        return CTPolicyResult::Reject("CT kill switch engaged");
    }

    // Check max CT per block limit
    if (current_ct_count >= config_.max_ct_per_block) {
        return CTPolicyResult::Reject(
            "Max CT per block exceeded (" +
            std::to_string(config_.max_ct_per_block) + ")"
        );
    }

    // Check proof data limit
    size_t tx_proof_bytes = GetTotalProofBytes(tx);
    if (current_ct_proof_bytes + tx_proof_bytes > config_.max_ct_proof_data) {
        return CTPolicyResult::Reject(
            "Max CT proof data exceeded (" +
            std::to_string(config_.max_ct_proof_data) + " bytes)"
        );
    }

    // Check minimum fee rate
    if (!MeetsMinimumFeeRate(tx)) {
        return CTPolicyResult::Reject(
            "CT fee rate below minimum (" +
            std::to_string(config_.ct_min_fee_rate) + " sat/vB)"
        );
    }

    return CTPolicyResult::Accept();
}

bool CTSelectionPolicy::HasConfidentialOutputs(const Transaction& tx) const {
    for (const auto& output : tx.vout) {
        if (output.is_confidential) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Weight Calculation
// ============================================================================

uint64_t CTSelectionPolicy::CalculateCTWeight(const Transaction& tx) const {
    auto info = GetWeightInfo(tx);
    return info.total_weight;
}

CTWeightInfo CTSelectionPolicy::GetWeightInfo(const Transaction& tx) const {
    CTWeightInfo info;

    // Calculate base weight
    info.base_weight = CalculateBaseWeight(tx);

    // Count confidential outputs and proof bytes
    info.confidential_outputs = GetConfidentialOutputCount(tx);
    info.proof_bytes = GetTotalProofBytes(tx);

    // Calculate proof weight
    // Proof bytes are weighted to reflect verification cost
    info.proof_weight = info.proof_bytes * config_.ct_proof_weight_factor;

    // Apply CT multiplier to base weight for CT transactions
    if (info.confidential_outputs > 0) {
        // Total = (base * multiplier) + proof_weight
        info.total_weight = static_cast<uint64_t>(
            info.base_weight * config_.ct_weight_multiplier
        ) + info.proof_weight;
    } else {
        // Non-CT: just base weight
        info.total_weight = info.base_weight;
    }

    return info;
}

uint64_t CTSelectionPolicy::CalculateBaseWeight(const Transaction& tx) {
    // SegWit-style weight calculation
    // Weight = (base_size * 3) + total_size
    // For simplified calculation, use serialized size * 4 (conservative)

    auto serialized = tx.Serialize();
    size_t total_size = serialized.size();

    // Estimate base size (non-witness data)
    // For now, use simplified calculation
    // In production, this should separate witness from non-witness data
    size_t base_size = total_size;  // Conservative: assume all is base

    // Calculate weight (SegWit formula)
    // Non-witness data counts 4x, witness data counts 1x
    // Without witness separation, we use total * 4 as upper bound
    uint64_t weight = base_size * 4;

    return weight;
}

// ============================================================================
// Fee Rate Calculation
// ============================================================================

double CTSelectionPolicy::CalculateAdjustedFeeRate(const Transaction& tx) const {
    uint64_t weight = CalculateCTWeight(tx);
    if (weight == 0) {
        return 0.0;
    }

    // Get fee from transaction
    // Note: For CT, fee is in explicit_fee field
    uint64_t fee = 0;
    if (tx.HasExplicitFee()) {
        fee = tx.GetExplicitFee();
    }

    // Fee rate in sat per weight unit
    // Multiply by 4 to get sat/vbyte equivalent
    return static_cast<double>(fee) / static_cast<double>(weight) * 4.0;
}

bool CTSelectionPolicy::MeetsMinimumFeeRate(const Transaction& tx) const {
    double fee_rate = CalculateAdjustedFeeRate(tx);
    return fee_rate >= static_cast<double>(config_.ct_min_fee_rate);
}

// ============================================================================
// Batch Optimization
// ============================================================================

bool CTSelectionPolicy::ShouldUseBatchVerification(size_t ct_count) const {
    if (!config_.enable_batch_optimization) {
        return false;
    }
    return ct_count >= config_.batch_size_threshold;
}

std::vector<Transaction> CTSelectionPolicy::OptimizeForBatchVerification(
    std::vector<Transaction> txs
) const {
    if (!config_.enable_batch_optimization) {
        return txs;
    }

    // Partition into CT and non-CT transactions
    std::vector<Transaction> ct_txs;
    std::vector<Transaction> transparent_txs;

    for (auto& tx : txs) {
        if (HasConfidentialOutputs(tx)) {
            ct_txs.push_back(std::move(tx));
        } else {
            transparent_txs.push_back(std::move(tx));
        }
    }

    // Sort CT transactions by proof size for optimal batching
    // Similar sizes batch more efficiently
    std::sort(ct_txs.begin(), ct_txs.end(),
        [this](const Transaction& a, const Transaction& b) {
            return GetTotalProofBytes(a) < GetTotalProofBytes(b);
        }
    );

    // Combine: transparent first, then CT batch
    // This allows early exit if transparent validation fails
    std::vector<Transaction> result;
    result.reserve(txs.size());

    // Coinbase (first transparent tx) must remain first
    if (!transparent_txs.empty()) {
        result.push_back(std::move(transparent_txs[0]));
        for (size_t i = 1; i < transparent_txs.size(); ++i) {
            result.push_back(std::move(transparent_txs[i]));
        }
    }

    // Add CT transactions grouped for batch verification
    for (auto& tx : ct_txs) {
        result.push_back(std::move(tx));
    }

    return result;
}

// ============================================================================
// Private Helpers
// ============================================================================

size_t CTSelectionPolicy::GetTotalProofBytes(const Transaction& tx) const {
    size_t total = 0;
    for (const auto& output : tx.vout) {
        if (output.is_confidential) {
            total += output.range_proof.size();
        }
    }
    return total;
}

size_t CTSelectionPolicy::GetConfidentialOutputCount(const Transaction& tx) const {
    size_t count = 0;
    for (const auto& output : tx.vout) {
        if (output.is_confidential) {
            ++count;
        }
    }
    return count;
}

bool CTSelectionPolicy::IsCTActivated(uint32_t height) const {
    const auto& params = dinero::Params();
    return height >= params.confidential_activation_height;
}

} // namespace mining
} // namespace dinero
