#pragma once

// SPDX-License-Identifier: MIT
// Phase W.4.1: Transaction Inclusion Analyzer - Advisory feedback on mining likelihood

#include <string>
#include <optional>
#include <cstdint>
#include "primitives/uint256.h"

// Forward declarations
namespace dinero {
    class Mempool;
    class BlockAssembler;
    struct NodeHealth;
}

namespace dinero {

// ============================================================================
// Transaction Inclusion State
// ============================================================================

/**
 * Advisory classification of transaction mining likelihood
 */
enum class InclusionState {
    LIKELY = 0,    ///< High probability of inclusion in next block
    POSSIBLE,      ///< Near cutoff, may be included
    STALLED,       ///< Needs action (fee bump or wait)
    BLOCKED        ///< Cannot be mined as-is (policy/consensus issue)
};

std::string InclusionStateToString(InclusionState state);

// Stream operator for test assertions
inline std::ostream& operator<<(std::ostream& os, InclusionState state) {
    return os << InclusionStateToString(state);
}

// ============================================================================
// Inclusion Reason (Why Not Included)
// ============================================================================

/**
 * Primary reason why a transaction may not be included
 */
enum class InclusionReason {
    NONE = 0,                  ///< No issues, likely to be mined
    LOW_FEERATE,              ///< Individual feerate below cutoff
    LOW_ANCESTOR_FEERATE,     ///< Package feerate below cutoff
    MEMPOOL_CONGESTED,        ///< Mempool full, eviction risk
    PACKAGE_LIMIT,            ///< Exceeds ancestor/descendant limits
    COMPACT_RISK,             ///< Low compact block reconstruction success
    NODE_NOT_READY,           ///< Node not synced or in reorg
    REORG_RECOVERY,           ///< Active reorg in progress
    NOT_IN_MEMPOOL            ///< Transaction not found in mempool
};

std::string InclusionReasonToString(InclusionReason reason);

// Stream operator for test assertions
inline std::ostream& operator<<(std::ostream& os, InclusionReason reason) {
    return os << InclusionReasonToString(reason);
}

// ============================================================================
// Transaction Inclusion Status (Advisory Result)
// ============================================================================

/**
 * @brief Advisory analysis of transaction mining likelihood
 *
 * This is purely local introspection - not a promise or guarantee.
 * Helps wallets understand why transactions aren't confirming and
 * what actions (RBF/CPFP/wait) might help.
 *
 * Phase W.4.1: Read-only analysis, no side effects.
 */
struct TxInclusionStatus {
    uint256 txid;                          ///< Transaction hash

    // Classification
    InclusionState state;                  ///< LIKELY, POSSIBLE, STALLED, BLOCKED
    InclusionReason primary_reason;        ///< Main reason for state

    // Feerate analysis
    double estimated_inclusion_prob;       ///< 0.0 - 1.0 (advisory estimate)
    uint64_t effective_feerate;            ///< Transaction's effective feerate (sat/vB)
    uint64_t cutoff_feerate;               ///< Current block assembly cutoff (sat/vB)

    // Rescue options
    bool rbf_available;                    ///< RBF is signaled and viable
    bool cpfp_available;                   ///< CPFP rescue is possible

    // Recommendation
    std::optional<uint64_t> suggested_bump_feerate; ///< Target feerate for bump (sat/vB)
    std::string explanation;               ///< Human-readable explanation

    // Timestamp
    uint64_t timestamp_ms;                 ///< When analysis was performed

    TxInclusionStatus();
};

// ============================================================================
// Transaction Inclusion Analyzer
// ============================================================================

/**
 * @brief Phase W.4.1: Transaction Inclusion Analyzer
 *
 * Provides read-only, advisory analysis of why transactions may not be
 * included in blocks. Integrates:
 * - Mempool state (feerate, ancestry)
 * - BlockAssembler cutoff (current mining threshold)
 * - Node health (W.3: sync state, reorgs)
 *
 * Design principles:
 * - No consensus changes
 * - No miner promises
 * - No policy signaling
 * - No transaction prioritization
 * - No side effects
 * - Fully local & advisory
 *
 * This is strictly better UX with zero protocol risk.
 */
class TxInclusionAnalyzer {
public:
    TxInclusionAnalyzer();
    ~TxInclusionAnalyzer();

    // ========================================================================
    // Core Analysis
    // ========================================================================

    /**
     * Analyze transaction mining likelihood
     *
     * @param txid Transaction hash
     * @param mempool Mempool instance (for tx state & feerate)
     * @param block_assembler BlockAssembler (for current cutoff)
     * @param node_health Node health status (W.3, optional)
     * @return Inclusion status with advisory recommendations
     */
    TxInclusionStatus Analyze(
        const uint256& txid,
        const Mempool* mempool,
        const BlockAssembler* block_assembler = nullptr,
        const NodeHealth* node_health = nullptr
    ) const;

    // ========================================================================
    // Component Analysis (Internal Helpers)
    // ========================================================================

    /**
     * Check if node is ready for mining
     *
     * @param node_health Node health status
     * @return true if node is synced and ready
     */
    bool IsNodeReady(const NodeHealth* node_health) const;

    /**
     * Determine inclusion state based on feerate comparison
     *
     * @param effective_feerate Transaction's effective feerate
     * @param cutoff_feerate Current block cutoff
     * @return Inclusion state (LIKELY, POSSIBLE, STALLED, BLOCKED)
     */
    InclusionState DetermineState(
        uint64_t effective_feerate,
        uint64_t cutoff_feerate
    ) const;

    /**
     * Calculate estimated inclusion probability
     *
     * @param effective_feerate Transaction's effective feerate
     * @param cutoff_feerate Current block cutoff
     * @return Probability estimate (0.0 - 1.0)
     */
    double EstimateInclusionProbability(
        uint64_t effective_feerate,
        uint64_t cutoff_feerate
    ) const;

    /**
     * Suggest fee bump target
     *
     * @param current_feerate Current effective feerate
     * @param cutoff_feerate Current block cutoff
     * @return Suggested target feerate (sat/vB)
     */
    uint64_t SuggestFeeBump(
        uint64_t current_feerate,
        uint64_t cutoff_feerate
    ) const;

    /**
     * Generate human-readable explanation
     *
     * @param status Inclusion status
     * @return User-friendly explanation
     */
    std::string GenerateExplanation(const TxInclusionStatus& status) const;

private:
    // Helper: Get current timestamp
    uint64_t GetCurrentTimeMs() const;

    // Constants
    static constexpr double LIKELY_THRESHOLD = 1.5;    // 1.5x cutoff = likely
    static constexpr double POSSIBLE_THRESHOLD = 1.0;  // 1.0x cutoff = possible
    static constexpr double STALLED_THRESHOLD = 0.9;   // 0.9x cutoff = stalled
    static constexpr double BUMP_MARGIN = 1.2;         // Suggest 1.2x cutoff for bumps
};

} // namespace dinero
