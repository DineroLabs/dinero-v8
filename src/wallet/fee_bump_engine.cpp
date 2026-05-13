// SPDX-License-Identifier: MIT
// Phase W.4.3: Fee Bump Recommendation Engine - Implementation

#include "wallet/fee_bump_engine.h"
#include "wallet/rbf_cpfp_detector.h"
#include "mining/tx_inclusion_analyzer.h"
#include "daemon/mempool.h"  // For getMempoolEntry, MempoolEntry
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace dinero {

// ============================================================================
// Enum to String Conversions
// ============================================================================

std::string BumpStrategyToString(BumpStrategy strategy) {
    switch (strategy) {
        case BumpStrategy::NONE:  return "none";
        case BumpStrategy::RBF:   return "rbf";
        case BumpStrategy::CPFP:  return "cpfp";
        case BumpStrategy::BOTH:  return "both";
        case BumpStrategy::WAIT:  return "wait";
        default:                   return "unknown";
    }
}

// ============================================================================
// Struct Constructors
// ============================================================================

RbfRecommendation::RbfRecommendation()
    : viable(false)
    , original_fee(0)
    , min_replacement_fee(0)
    , recommended_fee(0)
    , recommended_feerate(0)
    , additional_cost(0)
    , explanation("")
{
}

CpfpRecommendation::CpfpRecommendation()
    : viable(false)
    , parent_txid()
    , output_index(0)
    , output_amount(0)
    , parent_fee(0)
    , parent_feerate(0)
    , recommended_child_fee(0)
    , recommended_child_feerate(0)
    , package_feerate(0)
    , total_cost(0)
    , explanation("")
{
}

FeeBumpRecommendation::FeeBumpRecommendation()
    : txid()
    , strategy(BumpStrategy::WAIT)
    , rationale("")
    , rbf(std::nullopt)
    , cpfp(std::nullopt)
    , current_feerate(0)
    , target_feerate(0)
    , mempool_min_feerate(0)
    , estimated_blocks_current(0)
    , estimated_blocks_target(0)
    , warnings()
    , timestamp_ms(0)
{
}

// ============================================================================
// FeeBumpEngine Constructor/Destructor
// ============================================================================

FeeBumpEngine::FeeBumpEngine()
    : min_bump_increment_(DEFAULT_MIN_BUMP)
    , safety_margin_(DEFAULT_SAFETY_MARGIN)
{
}

FeeBumpEngine::~FeeBumpEngine() {
}

// ============================================================================
// Helper: Get Current Timestamp
// ============================================================================

uint64_t FeeBumpEngine::GetCurrentTimeMs() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// ============================================================================
// Core Recommendation API
// ============================================================================

FeeBumpRecommendation FeeBumpEngine::GenerateRecommendation(
    const uint256& txid,
    const TxInclusionStatus& inclusion_status,
    const RescueStrategy& rescue_strategy,
    const Mempool* mempool,
    const WalletManager* wallet,
    uint32_t target_blocks
) const {
    FeeBumpRecommendation recommendation;
    recommendation.txid = txid;
    recommendation.timestamp_ms = GetCurrentTimeMs();

    // Get current mempool conditions
    recommendation.mempool_min_feerate = GetMempoolMinFeeRate(mempool);
    recommendation.current_feerate = inclusion_status.effective_feerate;

    // Estimate target fee rate for desired confirmation time
    recommendation.target_feerate = EstimateFeeRate(target_blocks, mempool);

    // Estimate confirmation times
    recommendation.estimated_blocks_current = EstimateConfirmationBlocks(
        recommendation.current_feerate,
        mempool
    );
    recommendation.estimated_blocks_target = target_blocks;

    // Determine best strategy
    recommendation.strategy = DetermineBestStrategy(rescue_strategy, inclusion_status);

    // Generate strategy-specific recommendations
    if (rescue_strategy.rbf_available) {
        recommendation.rbf = GenerateRbfRecommendation(
            txid,
            rescue_strategy,
            recommendation.target_feerate,
            mempool
        );
    }

    if (rescue_strategy.cpfp_available) {
        recommendation.cpfp = GenerateCpfpRecommendation(
            txid,
            rescue_strategy,
            recommendation.target_feerate,
            mempool,
            wallet
        );
    }

    // Generate rationale
    recommendation.rationale = GenerateRationale(
        recommendation.strategy,
        inclusion_status,
        rescue_strategy
    );

    // Add warnings
    AddWarnings(recommendation, rescue_strategy);

    dinero::g_logger.debug("Fee bump recommendation: " +
                          BumpStrategyToString(recommendation.strategy));

    return recommendation;
}

RbfRecommendation FeeBumpEngine::GenerateRbfRecommendation(
    const uint256& txid,
    const RescueStrategy& rescue_strategy,
    uint64_t target_feerate,
    const Mempool* mempool
) const {
    RbfRecommendation rbf;

    if (!rescue_strategy.rbf_available) {
        rbf.viable = false;
        rbf.explanation = "RBF not available for this transaction";
        return rbf;
    }

    rbf.viable = true;

    // Get actual transaction size and fee from mempool
    uint64_t tx_size = 250;        // Fallback if lookup fails
    rbf.original_fee = 1000;       // Fallback if lookup fails
    if (mempool) {
        auto entry_opt = mempool->getMempoolEntry(txid);
        if (entry_opt) {
            const auto& entry = *entry_opt;
            tx_size = entry.tx_size > 0 ? entry.tx_size : 250;
            rbf.original_fee = entry.fee;
        }
    }
    rbf.min_replacement_fee = rescue_strategy.rbf_details.min_replacement_fee;

    // Calculate recommended fee based on target fee rate
    rbf.recommended_fee = CalculateRbfReplacementFee(
        rbf.original_fee,
        tx_size,
        target_feerate
    );

    // Apply safety margin
    rbf.recommended_fee = static_cast<uint64_t>(rbf.recommended_fee * safety_margin_);

    // Ensure meets minimum replacement requirement
    if (rbf.recommended_fee < rbf.min_replacement_fee) {
        rbf.recommended_fee = rbf.min_replacement_fee;
    }

    rbf.recommended_feerate = rbf.recommended_fee / tx_size;
    rbf.additional_cost = rbf.recommended_fee - rbf.original_fee;

    // Generate explanation
    std::ostringstream oss;
    oss << "Replace transaction with fee of " << rbf.recommended_fee
        << " sats (" << rbf.recommended_feerate << " sat/vB). "
        << "Additional cost: " << rbf.additional_cost << " sats.";
    rbf.explanation = oss.str();

    return rbf;
}

CpfpRecommendation FeeBumpEngine::GenerateCpfpRecommendation(
    const uint256& txid,
    const RescueStrategy& rescue_strategy,
    uint64_t target_feerate,
    const Mempool* mempool,
    const WalletManager* wallet
) const {
    CpfpRecommendation cpfp;

    if (!rescue_strategy.cpfp_available) {
        cpfp.viable = false;
        cpfp.explanation = "CPFP not available for this transaction";
        return cpfp;
    }

    if (rescue_strategy.cpfp_details.outputs.empty()) {
        cpfp.viable = false;
        cpfp.explanation = "No spendable outputs available for CPFP";
        return cpfp;
    }

    cpfp.viable = true;

    // Use first spendable output
    const auto& output = rescue_strategy.cpfp_details.outputs[0];
    cpfp.parent_txid = output.txid;
    cpfp.output_index = output.vout;
    cpfp.output_amount = output.amount;

    // Get actual parent transaction details from mempool
    uint64_t parent_size = 250;  // Fallback if lookup fails
    cpfp.parent_fee = 1000;      // Fallback if lookup fails
    if (mempool) {
        auto entry_opt = mempool->getMempoolEntry(txid);
        if (entry_opt) {
            const auto& entry = *entry_opt;
            parent_size = entry.tx_size > 0 ? entry.tx_size : 250;
            cpfp.parent_fee = entry.fee;
        }
    }
    cpfp.parent_feerate = parent_size > 0 ? cpfp.parent_fee / parent_size : 0;

    // Calculate child fee needed to achieve target package fee rate
    cpfp.recommended_child_fee = CalculateCpfpChildFee(
        cpfp.parent_fee,
        parent_size,
        ESTIMATED_CHILD_SIZE,
        target_feerate
    );

    // Apply safety margin
    cpfp.recommended_child_fee = static_cast<uint64_t>(
        cpfp.recommended_child_fee * safety_margin_
    );

    cpfp.recommended_child_feerate = cpfp.recommended_child_fee / ESTIMATED_CHILD_SIZE;

    // Calculate package fee rate
    uint64_t total_size = parent_size + ESTIMATED_CHILD_SIZE;
    uint64_t total_fees = cpfp.parent_fee + cpfp.recommended_child_fee;
    cpfp.package_feerate = total_fees / total_size;

    cpfp.total_cost = total_fees;

    // Check if output has enough value
    if (cpfp.recommended_child_fee > cpfp.output_amount) {
        cpfp.viable = false;
        cpfp.explanation = "Output value too small to pay required child fee";
        return cpfp;
    }

    // Generate explanation
    std::ostringstream oss;
    oss << "Create child transaction spending output " << cpfp.output_index
        << " with fee of " << cpfp.recommended_child_fee << " sats ("
        << cpfp.recommended_child_feerate << " sat/vB). "
        << "Package fee rate: " << cpfp.package_feerate << " sat/vB.";
    cpfp.explanation = oss.str();

    return cpfp;
}

// ============================================================================
// Fee Estimation
// ============================================================================

uint64_t FeeBumpEngine::EstimateFeeRate(
    uint32_t target_blocks,
    const Mempool* mempool
) const {
    // TODO: Query mempool for actual fee estimates
    // For now, use simple heuristic

    uint64_t mempool_min = GetMempoolMinFeeRate(mempool);

    // Heuristic: faster confirmation = higher multiplier
    double multiplier = 1.0;
    if (target_blocks == 1) {
        multiplier = 2.0;  // Next block: 2x mempool min
    } else if (target_blocks <= 3) {
        multiplier = 1.5;  // Within 3 blocks: 1.5x
    } else if (target_blocks <= 6) {
        multiplier = 1.2;  // Within 6 blocks: 1.2x
    }

    uint64_t estimated = static_cast<uint64_t>(mempool_min * multiplier);

    // Ensure at least minimum relay fee
    if (estimated < MIN_RELAY_FEE) {
        estimated = MIN_RELAY_FEE;
    }

    return estimated;
}

uint32_t FeeBumpEngine::EstimateConfirmationBlocks(
    uint64_t feerate,
    const Mempool* mempool
) const {
    // TODO: Query mempool for actual estimates based on current conditions
    // For now, use simple heuristic

    uint64_t mempool_min = GetMempoolMinFeeRate(mempool);

    if (mempool_min == 0) {
        return 1;  // Empty mempool, next block
    }

    double ratio = static_cast<double>(feerate) / mempool_min;

    // Heuristic based on fee rate ratio
    if (ratio >= 2.0) {
        return 1;   // High fee: next block
    } else if (ratio >= 1.5) {
        return 2;   // Good fee: 2 blocks
    } else if (ratio >= 1.2) {
        return 4;   // Moderate fee: 4 blocks
    } else if (ratio >= 1.0) {
        return 6;   // At minimum: 6 blocks
    } else {
        return 12;  // Below minimum: 12+ blocks
    }
}

uint64_t FeeBumpEngine::GetMempoolMinFeeRate(const Mempool* mempool) const {
    if (!mempool) {
        return MIN_RELAY_FEE;  // Default to minimum relay fee
    }

    // TODO: Query mempool for actual minimum fee rate
    // For now, return minimum relay fee
    return MIN_RELAY_FEE;
}

// ============================================================================
// Cost Analysis
// ============================================================================

double FeeBumpEngine::CalculateCostBenefit(
    uint64_t original_fee,
    uint64_t new_fee,
    uint32_t blocks_saved
) const {
    if (blocks_saved == 0) {
        return std::numeric_limits<double>::infinity();  // No benefit
    }

    uint64_t additional_cost = new_fee - original_fee;
    return static_cast<double>(additional_cost) / blocks_saved;
}

bool FeeBumpEngine::IsCostEffective(
    uint64_t original_fee,
    uint64_t new_fee,
    uint32_t blocks_saved,
    double max_ratio
) const {
    double ratio = CalculateCostBenefit(original_fee, new_fee, blocks_saved);
    return ratio <= max_ratio;
}

// ============================================================================
// Configuration
// ============================================================================

void FeeBumpEngine::SetMinBumpIncrement(uint64_t increment) {
    min_bump_increment_ = increment;
}

uint64_t FeeBumpEngine::GetMinBumpIncrement() const {
    return min_bump_increment_;
}

void FeeBumpEngine::SetSafetyMargin(double margin) {
    safety_margin_ = margin;
}

double FeeBumpEngine::GetSafetyMargin() const {
    return safety_margin_;
}

// ============================================================================
// Helper Methods
// ============================================================================

BumpStrategy FeeBumpEngine::DetermineBestStrategy(
    const RescueStrategy& rescue_strategy,
    const TxInclusionStatus& inclusion_status
) const {
    // If transaction is likely to be included, no bump needed
    if (inclusion_status.state == InclusionState::LIKELY) {
        return BumpStrategy::NONE;
    }

    // If both RBF and CPFP are available, prefer RBF (cleaner)
    if (rescue_strategy.rbf_available && rescue_strategy.cpfp_available) {
        return BumpStrategy::BOTH;  // Let user choose
    }

    // If only RBF is available
    if (rescue_strategy.rbf_available) {
        return BumpStrategy::RBF;
    }

    // If only CPFP is available
    if (rescue_strategy.cpfp_available) {
        return BumpStrategy::CPFP;
    }

    // No viable bump options
    return BumpStrategy::WAIT;
}

std::string FeeBumpEngine::GenerateRationale(
    BumpStrategy strategy,
    const TxInclusionStatus& inclusion_status,
    const RescueStrategy& rescue_strategy
) const {
    std::ostringstream oss;

    switch (strategy) {
        case BumpStrategy::NONE:
            oss << "Transaction has sufficient fee and is likely to be included soon. "
                << "No fee bump needed.";
            break;

        case BumpStrategy::RBF:
            oss << "RBF (Replace-By-Fee) is recommended. "
                << "Replace the original transaction with a higher fee version.";
            break;

        case BumpStrategy::CPFP:
            oss << "CPFP (Child-Pays-For-Parent) is recommended. "
                << "Create a child transaction with high fee to boost the parent.";
            break;

        case BumpStrategy::BOTH:
            oss << "Both RBF and CPFP are available. "
                << "RBF is generally simpler (replaces original), "
                << "while CPFP adds a second transaction. Choose based on preference.";
            break;

        case BumpStrategy::WAIT:
            oss << "No fee bump options available. ";
            if (!rescue_strategy.rbf_details.signals_rbf) {
                oss << "Transaction does not signal RBF. ";
            }
            if (rescue_strategy.cpfp_details.outputs.empty()) {
                oss << "No spendable outputs for CPFP. ";
            }
            oss << "Wait for confirmation or create a new transaction.";
            break;

        default:
            oss << "Unable to determine fee bump strategy.";
    }

    return oss.str();
}

uint64_t FeeBumpEngine::CalculateRbfReplacementFee(
    uint64_t original_fee,
    uint64_t tx_size,
    uint64_t target_feerate
) const {
    // Target fee based on desired fee rate
    uint64_t target_fee = tx_size * target_feerate;

    // Ensure it's higher than original
    if (target_fee <= original_fee) {
        target_fee = original_fee + (tx_size * min_bump_increment_);
    }

    return target_fee;
}

uint64_t FeeBumpEngine::CalculateCpfpChildFee(
    uint64_t parent_fee,
    uint64_t parent_size,
    uint64_t child_size,
    uint64_t target_package_feerate
) const {
    // Package fee rate = (parent_fee + child_fee) / (parent_size + child_size)
    // Solve for child_fee:
    // child_fee = (target_package_feerate * (parent_size + child_size)) - parent_fee

    uint64_t total_size = parent_size + child_size;
    uint64_t required_package_fee = target_package_feerate * total_size;

    if (required_package_fee <= parent_fee) {
        // Parent already has sufficient fee
        return child_size * MIN_RELAY_FEE;  // Minimum child fee
    }

    uint64_t child_fee = required_package_fee - parent_fee;

    // Ensure child fee meets minimum relay fee
    uint64_t min_child_fee = child_size * MIN_RELAY_FEE;
    if (child_fee < min_child_fee) {
        child_fee = min_child_fee;
    }

    return child_fee;
}

void FeeBumpEngine::AddWarnings(
    FeeBumpRecommendation& recommendation,
    const RescueStrategy& rescue_strategy
) const {
    // Warn if approaching package limits
    if (rescue_strategy.cpfp_available) {
        uint32_t ancestor_count = rescue_strategy.cpfp_details.current_ancestor_count;
        uint32_t max_ancestors = rescue_strategy.cpfp_details.max_ancestor_count;

        if (ancestor_count >= max_ancestors - 5) {
            recommendation.warnings.push_back(
                "Transaction is near ancestor package limits. "
                "CPFP may fail if package size exceeds limits."
            );
        }
    }

    // Warn if CPFP output value is low
    if (recommendation.cpfp && recommendation.cpfp->viable) {
        uint64_t output_amount = recommendation.cpfp->output_amount;
        uint64_t child_fee = recommendation.cpfp->recommended_child_fee;

        if (child_fee > output_amount * 0.5) {  // Child fee > 50% of output
            recommendation.warnings.push_back(
                "CPFP will consume significant portion of output value. "
                "Consider RBF if available."
            );
        }
    }

    // Warn if fee bump is very expensive
    if (recommendation.rbf && recommendation.rbf->viable) {
        uint64_t additional_cost = recommendation.rbf->additional_cost;
        uint64_t original_fee = recommendation.rbf->original_fee;

        if (additional_cost > original_fee * 2) {  // More than 3x original fee
            recommendation.warnings.push_back(
                "Fee bump will more than triple the original transaction cost."
            );
        }
    }
}

} // namespace dinero
