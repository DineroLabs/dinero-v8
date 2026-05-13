// SPDX-License-Identifier: MIT
// Phase W.4.1: Transaction Inclusion Analyzer - Implementation

#include "mining/tx_inclusion_analyzer.h"
#include "daemon/mempool.h"
#include "mining/block_assembler.h"
#include "mining/transaction_scorer.h"  // FeeHistogram for percentile-based cutoff
#include "rpc/ergonomics_rpc_handlers.h"  // For NodeHealth
#include "common/logger.h"
#include <chrono>
#include <sstream>

namespace dinero {

// ============================================================================
// Enum to String Conversions
// ============================================================================

std::string InclusionStateToString(InclusionState state) {
    switch (state) {
        case InclusionState::LIKELY:   return "likely";
        case InclusionState::POSSIBLE: return "possible";
        case InclusionState::STALLED:  return "stalled";
        case InclusionState::BLOCKED:  return "blocked";
        default:                        return "unknown";
    }
}

std::string InclusionReasonToString(InclusionReason reason) {
    switch (reason) {
        case InclusionReason::NONE:                  return "none";
        case InclusionReason::LOW_FEERATE:          return "low_feerate";
        case InclusionReason::LOW_ANCESTOR_FEERATE: return "low_ancestor_feerate";
        case InclusionReason::MEMPOOL_CONGESTED:    return "mempool_congested";
        case InclusionReason::PACKAGE_LIMIT:        return "package_limit";
        case InclusionReason::COMPACT_RISK:         return "compact_risk";
        case InclusionReason::NODE_NOT_READY:       return "node_not_ready";
        case InclusionReason::REORG_RECOVERY:       return "reorg_recovery";
        case InclusionReason::NOT_IN_MEMPOOL:       return "not_in_mempool";
        default:                                     return "unknown";
    }
}

// ============================================================================
// TxInclusionStatus Constructor
// ============================================================================

TxInclusionStatus::TxInclusionStatus()
    : txid()
    , state(InclusionState::BLOCKED)
    , primary_reason(InclusionReason::NOT_IN_MEMPOOL)
    , estimated_inclusion_prob(0.0)
    , effective_feerate(0)
    , cutoff_feerate(0)
    , rbf_available(false)
    , cpfp_available(false)
    , suggested_bump_feerate(std::nullopt)
    , explanation("")
    , timestamp_ms(0)
{
}

// ============================================================================
// TxInclusionAnalyzer Constructor/Destructor
// ============================================================================

TxInclusionAnalyzer::TxInclusionAnalyzer() {
}

TxInclusionAnalyzer::~TxInclusionAnalyzer() {
}

// ============================================================================
// Helper: Get Current Timestamp
// ============================================================================

uint64_t TxInclusionAnalyzer::GetCurrentTimeMs() const {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// ============================================================================
// Core Analysis
// ============================================================================

TxInclusionStatus TxInclusionAnalyzer::Analyze(
    const uint256& txid,
    const Mempool* mempool,
    const BlockAssembler* block_assembler,
    const NodeHealth* node_health
) const {
    TxInclusionStatus status;
    status.txid = txid;
    status.timestamp_ms = GetCurrentTimeMs();

    // ========================================================================
    // Step 1: Check if node is ready for mining
    // ========================================================================
    if (node_health && !IsNodeReady(node_health)) {
        status.state = InclusionState::BLOCKED;

        // Determine specific node readiness issue
        if (node_health->overall_grade == NodeHealthGrade::RED) {
            status.primary_reason = InclusionReason::NODE_NOT_READY;
            status.explanation = "Node is not ready for mining (critical issues detected)";
        } else {
            status.primary_reason = InclusionReason::REORG_RECOVERY;
            status.explanation = "Node is recovering from reorg, mining paused";
        }

        status.effective_feerate = 0;
        status.cutoff_feerate = 0;
        status.estimated_inclusion_prob = 0.0;
        status.rbf_available = false;
        status.cpfp_available = false;

        return status;
    }

    // ========================================================================
    // Step 2: Check if transaction is in mempool
    // ========================================================================
    if (!mempool) {
        status.state = InclusionState::BLOCKED;
        status.primary_reason = InclusionReason::NOT_IN_MEMPOOL;
        status.explanation = "Cannot analyze: mempool unavailable";
        status.estimated_inclusion_prob = 0.0;
        return status;
    }

    // Query mempool for transaction entry
    auto entry_opt = mempool->getMempoolEntry(txid);
    bool tx_in_mempool = entry_opt.has_value();

    if (!tx_in_mempool) {
        status.state = InclusionState::BLOCKED;
        status.primary_reason = InclusionReason::NOT_IN_MEMPOOL;
        status.explanation = "Transaction not found in mempool (may be confirmed or rejected)";
        status.estimated_inclusion_prob = 0.0;
        return status;
    }

    // ========================================================================
    // Step 3: Get transaction feerate and cutoff
    // ========================================================================

    // Get actual feerate from mempool entry (una/vB)
    const auto& entry = *entry_opt;
    uint64_t effective_feerate = static_cast<uint64_t>(entry.adjusted_fee_rate);

    // Use package score if lower (CPFP-aware and CT-cost-aware)
    if (entry.ancestor_adjusted_feerate > 0 &&
        entry.ancestor_adjusted_feerate < entry.adjusted_fee_rate) {
        effective_feerate = static_cast<uint64_t>(entry.ancestor_adjusted_feerate);
    }

    // ========================================================================
    // Percentile-based cutoff (not averages)
    // ========================================================================
    // Build fee histogram from current mempool state
    // Use 15th percentile as cutoff: "barely acceptable for inclusion"
    // This represents the bottom edge of what miners are currently accepting
    //
    // Percentile meaning:
    //   10-20%  → backlog threshold (barely included)
    //   25-50%  → normal flow
    //   50-75%  → priority
    //   75%+    → urgency / bidding war
    // ========================================================================

    FeeHistogram histogram;
    histogram.BuildFromMempool(mempool);

    // Default cutoff: 1 una/vB (minimum relay fee)
    uint64_t cutoff_feerate = 1;

    if (histogram.Count() > 0) {
        // Use 15th percentile as the inclusion threshold
        // This is the feerate below which ~15% of mempool txs sit
        cutoff_feerate = histogram.GetFeeRateAtPercentile(0.15);

        // Floor at 1 una/vB
        if (cutoff_feerate < 1) cutoff_feerate = 1;

        dinero::g_logger.debug("TxInclusionAnalyzer: percentile-based cutoff=" +
                              std::to_string(cutoff_feerate) + " una/vB (p15 of " +
                              std::to_string(histogram.Count()) + " txs)");
    } else {
        // Empty mempool: any fee is likely to be included
        dinero::g_logger.debug("TxInclusionAnalyzer: empty mempool, cutoff=1 una/vB");
    }

    status.effective_feerate = effective_feerate;
    status.cutoff_feerate = cutoff_feerate;

    // ========================================================================
    // Step 4: Determine inclusion state
    // ========================================================================
    status.state = DetermineState(effective_feerate, cutoff_feerate);

    // Set primary reason based on state
    if (status.state == InclusionState::LIKELY) {
        status.primary_reason = InclusionReason::NONE;
    } else if (status.state == InclusionState::STALLED) {
        status.primary_reason = InclusionReason::LOW_FEERATE;
    } else {
        status.primary_reason = InclusionReason::LOW_FEERATE;
    }

    // ========================================================================
    // Step 5: Calculate inclusion probability
    // ========================================================================
    status.estimated_inclusion_prob = EstimateInclusionProbability(
        effective_feerate,
        cutoff_feerate
    );

    // ========================================================================
    // Step 6: Check rescue options (RBF/CPFP)
    // ========================================================================

    // Check if transaction signals RBF (BIP125: any input with sequence < 0xfffffffe)
    status.rbf_available = false;
    for (const auto& vin : entry.tx.vin) {
        if (vin.sequence < 0xfffffffe) {
            status.rbf_available = true;
            break;
        }
    }

    // CPFP is viable if transaction has unspent outputs (can be spent to add fees)
    // This is always true for unconfirmed transactions with outputs
    status.cpfp_available = !entry.tx.vout.empty();

    // ========================================================================
    // Step 7: Suggest fee bump if needed
    // ========================================================================
    if (status.state == InclusionState::STALLED ||
        status.state == InclusionState::POSSIBLE) {
        status.suggested_bump_feerate = SuggestFeeBump(
            effective_feerate,
            cutoff_feerate
        );
    }

    // ========================================================================
    // Step 8: Generate explanation
    // ========================================================================
    status.explanation = GenerateExplanation(status);

    return status;
}

// ============================================================================
// Component Analysis
// ============================================================================

bool TxInclusionAnalyzer::IsNodeReady(const NodeHealth* node_health) const {
    if (!node_health) {
        return true;  // Assume ready if no health info
    }

    // Node is not ready if:
    // - Overall grade is RED (critical issues)
    // - Sync subsystem is not GREEN

    if (node_health->overall_grade == NodeHealthGrade::RED) {
        return false;
    }

    // Check if sync subsystem is healthy
    for (const auto& subsystem : node_health->subsystems) {
        if (subsystem.name == "sync") {
            if (subsystem.grade == NodeHealthGrade::RED) {
                return false;
            }
        }
    }

    return true;
}

InclusionState TxInclusionAnalyzer::DetermineState(
    uint64_t effective_feerate,
    uint64_t cutoff_feerate
) const {
    if (cutoff_feerate == 0) {
        // No cutoff means empty mempool - transaction is likely
        return InclusionState::LIKELY;
    }

    double ratio = static_cast<double>(effective_feerate) / cutoff_feerate;

    if (ratio >= LIKELY_THRESHOLD) {
        return InclusionState::LIKELY;
    } else if (ratio >= POSSIBLE_THRESHOLD) {
        return InclusionState::POSSIBLE;
    } else if (ratio >= STALLED_THRESHOLD) {
        return InclusionState::STALLED;
    } else {
        return InclusionState::BLOCKED;
    }
}

double TxInclusionAnalyzer::EstimateInclusionProbability(
    uint64_t effective_feerate,
    uint64_t cutoff_feerate
) const {
    if (cutoff_feerate == 0) {
        return 1.0;  // No competition, 100% chance
    }

    double ratio = static_cast<double>(effective_feerate) / cutoff_feerate;

    // Sigmoid-like function for probability
    // - ratio >= 1.5 → ~95% probability
    // - ratio == 1.0 → ~50% probability
    // - ratio <= 0.5 → ~5% probability

    if (ratio >= LIKELY_THRESHOLD) {
        return 0.95;
    } else if (ratio >= POSSIBLE_THRESHOLD) {
        return 0.50 + (ratio - POSSIBLE_THRESHOLD) * 0.45 / (LIKELY_THRESHOLD - POSSIBLE_THRESHOLD);
    } else if (ratio >= STALLED_THRESHOLD) {
        return 0.20 + (ratio - STALLED_THRESHOLD) * 0.30 / (POSSIBLE_THRESHOLD - STALLED_THRESHOLD);
    } else {
        return 0.05 + (ratio / STALLED_THRESHOLD) * 0.15;
    }
}

uint64_t TxInclusionAnalyzer::SuggestFeeBump(
    uint64_t current_feerate,
    uint64_t cutoff_feerate
) const {
    if (cutoff_feerate == 0) {
        // No cutoff - suggest minimal bump
        return current_feerate + 1;
    }

    // Suggest 1.2x the current cutoff with a minimum margin
    uint64_t suggested = static_cast<uint64_t>(cutoff_feerate * BUMP_MARGIN);

    // Ensure suggestion is higher than current feerate
    if (suggested <= current_feerate) {
        suggested = current_feerate + 1;
    }

    return suggested;
}

std::string TxInclusionAnalyzer::GenerateExplanation(const TxInclusionStatus& status) const {
    std::ostringstream oss;

    switch (status.state) {
        case InclusionState::LIKELY:
            oss << "Transaction has high fee rate (" << status.effective_feerate
                << " sat/vB) and is likely to be included in next block.";
            break;

        case InclusionState::POSSIBLE:
            oss << "Transaction fee rate (" << status.effective_feerate
                << " sat/vB) is near the cutoff (" << status.cutoff_feerate
                << " sat/vB). May be included depending on mempool conditions.";
            if (status.suggested_bump_feerate) {
                oss << " Consider bumping to " << *status.suggested_bump_feerate
                    << " sat/vB for faster confirmation.";
            }
            break;

        case InclusionState::STALLED:
            oss << "Transaction fee rate (" << status.effective_feerate
                << " sat/vB) is below the current block cutoff (" << status.cutoff_feerate
                << " sat/vB). ";
            if (status.suggested_bump_feerate) {
                oss << "Recommend fee bump to " << *status.suggested_bump_feerate
                    << " sat/vB.";
            } else {
                oss << "Consider increasing fee or using CPFP.";
            }
            break;

        case InclusionState::BLOCKED:
            if (status.primary_reason == InclusionReason::NOT_IN_MEMPOOL) {
                oss << "Transaction not found in mempool. It may be confirmed, rejected, or not yet broadcast.";
            } else if (status.primary_reason == InclusionReason::NODE_NOT_READY) {
                oss << "Node is not ready for mining. Check node health.";
            } else if (status.primary_reason == InclusionReason::REORG_RECOVERY) {
                oss << "Node is recovering from a blockchain reorganization. Wait for sync to complete.";
            } else {
                oss << "Transaction is blocked from mining. Reason: "
                    << InclusionReasonToString(status.primary_reason);
            }
            break;

        default:
            oss << "Unable to determine inclusion status.";
    }

    return oss.str();
}

} // namespace dinero
