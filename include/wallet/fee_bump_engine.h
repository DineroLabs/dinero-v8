#pragma once

// SPDX-License-Identifier: MIT
// Phase W.4.3: Fee Bump Recommendation Engine

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "primitives/uint256.h"

// Forward declarations
namespace dinero {
    class Mempool;
    class WalletManager;
    struct TxInclusionStatus;
    struct RescueStrategy;
}

namespace dinero {

// ============================================================================
// Fee Bump Strategy
// ============================================================================

/**
 * Recommended fee bump strategy
 */
enum class BumpStrategy {
    NONE = 0,        ///< No bump recommended (tx is fine)
    RBF,             ///< Replace-By-Fee recommended
    CPFP,            ///< Child-Pays-For-Parent recommended
    BOTH,            ///< Either RBF or CPFP viable (user choice)
    WAIT             ///< Wait for confirmation (no viable bump)
};

/**
 * Convert BumpStrategy to string
 */
std::string BumpStrategyToString(BumpStrategy strategy);

// ============================================================================
// RBF Recommendation
// ============================================================================

/**
 * RBF fee bump recommendation
 */
struct RbfRecommendation {
    bool viable;                           ///< RBF is possible
    uint64_t original_fee;                 ///< Original transaction fee (sats)
    uint64_t min_replacement_fee;          ///< Minimum replacement fee (sats)
    uint64_t recommended_fee;              ///< Recommended new fee (sats)
    uint64_t recommended_feerate;          ///< Recommended fee rate (sat/vB)
    uint64_t additional_cost;              ///< Additional cost over original (sats)
    std::string explanation;               ///< Human-readable explanation

    RbfRecommendation();
};

// ============================================================================
// CPFP Recommendation
// ============================================================================

/**
 * CPFP fee bump recommendation
 */
struct CpfpRecommendation {
    bool viable;                           ///< CPFP is possible
    uint256 parent_txid;                   ///< Parent transaction to bump
    uint32_t output_index;                 ///< Output to spend for CPFP
    uint64_t output_amount;                ///< Available amount (sats)
    uint64_t parent_fee;                   ///< Parent transaction fee (sats)
    uint64_t parent_feerate;               ///< Parent fee rate (sat/vB)
    uint64_t recommended_child_fee;        ///< Recommended child tx fee (sats)
    uint64_t recommended_child_feerate;    ///< Recommended child fee rate (sat/vB)
    uint64_t package_feerate;              ///< Combined package fee rate (sat/vB)
    uint64_t total_cost;                   ///< Total cost (parent + child fees)
    std::string explanation;               ///< Human-readable explanation

    CpfpRecommendation();
};

// ============================================================================
// Fee Bump Recommendation
// ============================================================================

/**
 * Comprehensive fee bump recommendation
 */
struct FeeBumpRecommendation {
    uint256 txid;                          ///< Transaction hash
    BumpStrategy strategy;                 ///< Recommended strategy
    std::string rationale;                 ///< Why this strategy is recommended

    // Strategy-specific recommendations
    std::optional<RbfRecommendation> rbf;  ///< RBF recommendation (if viable)
    std::optional<CpfpRecommendation> cpfp; ///< CPFP recommendation (if viable)

    // Context
    uint64_t current_feerate;              ///< Current tx fee rate (sat/vB)
    uint64_t target_feerate;               ///< Target fee rate for fast confirmation (sat/vB)
    uint64_t mempool_min_feerate;          ///< Current mempool minimum (sat/vB)

    // Time estimates
    uint32_t estimated_blocks_current;     ///< Blocks to confirm at current fee
    uint32_t estimated_blocks_target;      ///< Blocks to confirm at target fee

    // Warnings
    std::vector<std::string> warnings;     ///< Any warnings or caveats

    uint64_t timestamp_ms;                 ///< Recommendation timestamp

    FeeBumpRecommendation();
};

// ============================================================================
// Fee Bump Engine
// ============================================================================

/**
 * @brief Phase W.4.3: Fee Bump Recommendation Engine
 *
 * Generates actionable fee bump recommendations based on:
 * - Transaction inclusion analysis (W.4.1)
 * - RBF/CPFP capability detection (W.4.2)
 * - Current mempool conditions
 * - User urgency preferences
 *
 * Provides specific fee suggestions for:
 * - RBF: Exact replacement fee to achieve target confirmation time
 * - CPFP: Child transaction parameters for package boost
 *
 * Design principles:
 * - Advisory recommendations only
 * - No automatic fee bumps
 * - Cost-benefit transparency
 * - Multiple urgency levels
 */
class FeeBumpEngine {
public:
    FeeBumpEngine();
    ~FeeBumpEngine();

    // ========================================================================
    // Core Recommendation API
    // ========================================================================

    /**
     * Generate comprehensive fee bump recommendation
     *
     * Analyzes transaction and recommends best strategy to achieve
     * target confirmation time.
     *
     * @param txid Transaction hash
     * @param inclusion_status Transaction inclusion status (from W.4.1)
     * @param rescue_strategy Rescue capabilities (from W.4.2)
     * @param mempool Mempool instance (for fee estimates)
     * @param wallet Wallet manager (for CPFP outputs)
     * @param target_blocks Target confirmation time (blocks)
     * @return Fee bump recommendation
     */
    FeeBumpRecommendation GenerateRecommendation(
        const uint256& txid,
        const TxInclusionStatus& inclusion_status,
        const RescueStrategy& rescue_strategy,
        const Mempool* mempool = nullptr,
        const WalletManager* wallet = nullptr,
        uint32_t target_blocks = 1  // Default: next block
    ) const;

    /**
     * Generate RBF-specific recommendation
     *
     * @param txid Transaction hash
     * @param rescue_strategy Rescue capabilities
     * @param target_feerate Target fee rate (sat/vB)
     * @param mempool Mempool instance
     * @return RBF recommendation
     */
    RbfRecommendation GenerateRbfRecommendation(
        const uint256& txid,
        const RescueStrategy& rescue_strategy,
        uint64_t target_feerate,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * Generate CPFP-specific recommendation
     *
     * @param txid Parent transaction hash
     * @param rescue_strategy Rescue capabilities
     * @param target_feerate Target package fee rate (sat/vB)
     * @param mempool Mempool instance
     * @param wallet Wallet manager
     * @return CPFP recommendation
     */
    CpfpRecommendation GenerateCpfpRecommendation(
        const uint256& txid,
        const RescueStrategy& rescue_strategy,
        uint64_t target_feerate,
        const Mempool* mempool = nullptr,
        const WalletManager* wallet = nullptr
    ) const;

    // ========================================================================
    // Fee Estimation
    // ========================================================================

    /**
     * Estimate fee rate for target confirmation time
     *
     * @param target_blocks Desired confirmation time (blocks)
     * @param mempool Mempool instance (for current conditions)
     * @return Estimated fee rate (sat/vB)
     */
    uint64_t EstimateFeeRate(
        uint32_t target_blocks,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * Estimate confirmation blocks for given fee rate
     *
     * @param feerate Fee rate (sat/vB)
     * @param mempool Mempool instance
     * @return Estimated blocks to confirmation
     */
    uint32_t EstimateConfirmationBlocks(
        uint64_t feerate,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * Get current mempool minimum fee rate
     *
     * @param mempool Mempool instance
     * @return Minimum fee rate to enter mempool (sat/vB)
     */
    uint64_t GetMempoolMinFeeRate(const Mempool* mempool = nullptr) const;

    // ========================================================================
    // Cost Analysis
    // ========================================================================

    /**
     * Calculate cost-benefit ratio for fee bump
     *
     * Returns ratio of additional cost to urgency benefit.
     * Lower is better (cheaper relative to benefit).
     *
     * @param original_fee Original transaction fee (sats)
     * @param new_fee Proposed new fee (sats)
     * @param blocks_saved Blocks saved by fee bump
     * @return Cost-benefit ratio (sats per block saved)
     */
    double CalculateCostBenefit(
        uint64_t original_fee,
        uint64_t new_fee,
        uint32_t blocks_saved
    ) const;

    /**
     * Check if fee bump is cost-effective
     *
     * @param original_fee Original fee (sats)
     * @param new_fee Proposed new fee (sats)
     * @param blocks_saved Blocks saved
     * @param max_ratio Maximum acceptable cost/benefit ratio
     * @return true if cost-effective
     */
    bool IsCostEffective(
        uint64_t original_fee,
        uint64_t new_fee,
        uint32_t blocks_saved,
        double max_ratio = 1000.0  // Default: max 1000 sats per block saved
    ) const;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Set minimum fee bump increment (sat/vB)
     */
    void SetMinBumpIncrement(uint64_t increment);

    /**
     * Get minimum fee bump increment
     */
    uint64_t GetMinBumpIncrement() const;

    /**
     * Set safety margin multiplier for fee estimates
     *
     * Multiplier applied to estimated fees to increase reliability.
     * Default: 1.1 (10% safety margin)
     */
    void SetSafetyMargin(double margin);

    /**
     * Get safety margin multiplier
     */
    double GetSafetyMargin() const;

private:
    // ========================================================================
    // Helper Methods
    // ========================================================================

    /**
     * Get current timestamp (milliseconds)
     */
    uint64_t GetCurrentTimeMs() const;

    /**
     * Determine best strategy from available options
     */
    BumpStrategy DetermineBestStrategy(
        const RescueStrategy& rescue_strategy,
        const TxInclusionStatus& inclusion_status
    ) const;

    /**
     * Generate rationale for recommended strategy
     */
    std::string GenerateRationale(
        BumpStrategy strategy,
        const TxInclusionStatus& inclusion_status,
        const RescueStrategy& rescue_strategy
    ) const;

    /**
     * Calculate RBF replacement fee
     */
    uint64_t CalculateRbfReplacementFee(
        uint64_t original_fee,
        uint64_t tx_size,
        uint64_t target_feerate
    ) const;

    /**
     * Calculate CPFP child fee
     */
    uint64_t CalculateCpfpChildFee(
        uint64_t parent_fee,
        uint64_t parent_size,
        uint64_t child_size,
        uint64_t target_package_feerate
    ) const;

    /**
     * Add warnings for edge cases
     */
    void AddWarnings(
        FeeBumpRecommendation& recommendation,
        const RescueStrategy& rescue_strategy
    ) const;

    // Configuration
    uint64_t min_bump_increment_;   ///< Minimum fee bump (sat/vB)
    double safety_margin_;          ///< Safety margin multiplier

    // Constants
    static constexpr uint64_t DEFAULT_MIN_BUMP = 1;       // 1 sat/vB minimum
    static constexpr double DEFAULT_SAFETY_MARGIN = 1.1;  // 10% safety margin
    static constexpr uint64_t MIN_RELAY_FEE = 1;          // 1 sat/vB min relay
    static constexpr uint32_t ESTIMATED_CHILD_SIZE = 250; // Typical child tx size
};

} // namespace dinero
