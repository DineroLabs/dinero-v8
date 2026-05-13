#pragma once

// SPDX-License-Identifier: MIT
// Phase W.4.2: RBF & CPFP Capability Detection

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "primitives/uint256.h"

// Forward declarations
namespace dinero {
    class Mempool;
    class WalletManager;
    struct Transaction;
}

namespace dinero {

// ============================================================================
// Constants
// ============================================================================

// BIP125 RBF sequence threshold
static constexpr uint32_t RBF_SEQUENCE_THRESHOLD = 0xfffffffe;  // MAX_SEQUENCE - 1

// Bitcoin Core mempool limits
static constexpr uint32_t DEFAULT_MAX_ANCESTORS = 25;
static constexpr uint64_t DEFAULT_MIN_RELAY_FEE = 1;  // 1 sat/vB

// ============================================================================
// RBF (Replace-By-Fee) Capability
// ============================================================================

/**
 * RBF detection result
 */
struct RbfCapability {
    bool signals_rbf;              ///< Transaction signals BIP125 opt-in RBF
    bool can_be_replaced;          ///< Transaction can be replaced (not confirmed)
    uint64_t min_replacement_fee;  ///< Minimum fee for replacement (sats)
    std::string rbf_status;        ///< Human-readable RBF status

    RbfCapability();
};

// ============================================================================
// CPFP (Child-Pays-For-Parent) Capability
// ============================================================================

/**
 * Spendable output for CPFP
 */
struct SpendableOutput {
    uint256 txid;                  ///< Parent transaction hash
    uint32_t vout;                 ///< Output index
    uint64_t amount;               ///< Output amount (sats)
    bool is_confirmed;             ///< Output is confirmed
    bool is_wallet_controlled;     ///< Wallet has key for this output

    SpendableOutput();
};

/**
 * CPFP viability result
 */
struct CpfpCapability {
    bool viable;                           ///< CPFP is possible
    std::vector<SpendableOutput> outputs;  ///< Spendable outputs for CPFP
    uint32_t current_ancestor_count;       ///< Current ancestor count
    uint32_t max_ancestor_count;           ///< Maximum allowed ancestors
    bool within_package_limits;            ///< Not exceeding package limits
    std::string cpfp_status;               ///< Human-readable CPFP status

    CpfpCapability();
};

// ============================================================================
// Rescue Strategy
// ============================================================================

/**
 * Overall rescue strategy (RBF or CPFP)
 */
struct RescueStrategy {
    bool rbf_available;            ///< RBF is viable
    bool cpfp_available;           ///< CPFP is viable
    std::string recommended_action; ///< "rbf", "cpfp", "wait", or "none"
    std::string explanation;       ///< Why this strategy is recommended

    RbfCapability rbf_details;     ///< RBF capability details
    CpfpCapability cpfp_details;   ///< CPFP capability details

    RescueStrategy();
};

// ============================================================================
// RBF & CPFP Detector
// ============================================================================

/**
 * @brief Phase W.4.2: RBF & CPFP Capability Detector
 *
 * Analyzes transactions to determine if they can be rescued via:
 * - RBF (Replace-By-Fee): Replace the transaction with higher fee
 * - CPFP (Child-Pays-For-Parent): Spend an output with high fee child
 *
 * Integrates with:
 * - Mempool: Transaction ancestry and RBF signaling
 * - Wallet: UTXO availability and control
 *
 * Design principles:
 * - Read-only analysis
 * - No automatic fee bumps
 * - Advisory recommendations only
 */
class RbfCpfpDetector {
public:
    RbfCpfpDetector();
    ~RbfCpfpDetector();

    // ========================================================================
    // RBF Detection
    // ========================================================================

    /**
     * Check if transaction signals RBF (BIP125)
     *
     * BIP125 opt-in RBF: Transaction signals replaceability if:
     * - Any input has nSequence < 0xfffffffe (MAX_SEQUENCE - 1)
     *
     * @param txid Transaction hash
     * @param mempool Mempool instance (for tx lookup)
     * @return RBF capability details
     */
    RbfCapability CheckRbfCapability(
        const uint256& txid,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * Check if transaction can be replaced
     *
     * Requirements:
     * - Transaction must signal RBF
     * - Transaction must be unconfirmed
     * - Replacement must have higher fee
     *
     * @param txid Transaction hash
     * @param mempool Mempool instance
     * @return true if transaction can be replaced
     */
    bool CanBeReplaced(
        const uint256& txid,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * Calculate minimum fee for RBF replacement
     *
     * BIP125 rules:
     * - New fee must be higher than original
     * - Must pay for own bandwidth (incremental relay fee)
     *
     * @param txid Original transaction hash
     * @param mempool Mempool instance
     * @return Minimum replacement fee (sats)
     */
    uint64_t CalculateMinReplacementFee(
        const uint256& txid,
        const Mempool* mempool = nullptr
    ) const;

    // ========================================================================
    // CPFP Detection
    // ========================================================================

    /**
     * Check if CPFP is viable for this transaction
     *
     * CPFP is viable if:
     * - Transaction has unconfirmed outputs
     * - Wallet controls at least one output
     * - Not exceeding ancestor package limits
     *
     * @param txid Parent transaction hash
     * @param wallet Wallet manager (for UTXO control)
     * @param mempool Mempool instance (for ancestry checks)
     * @return CPFP capability details
     */
    CpfpCapability CheckCpfpCapability(
        const uint256& txid,
        const WalletManager* wallet = nullptr,
        const Mempool* mempool = nullptr
    ) const;

    /**
     * Find spendable outputs for CPFP
     *
     * @param txid Parent transaction hash
     * @param wallet Wallet manager
     * @return List of spendable outputs
     */
    std::vector<SpendableOutput> FindSpendableOutputs(
        const uint256& txid,
        const WalletManager* wallet = nullptr
    ) const;

    /**
     * Check if transaction is within package limits
     *
     * Bitcoin Core limits:
     * - Max 25 ancestors
     * - Max 101 KB ancestor size
     *
     * @param txid Transaction hash
     * @param mempool Mempool instance
     * @return true if within limits
     */
    bool IsWithinPackageLimits(
        const uint256& txid,
        const Mempool* mempool = nullptr
    ) const;

    // ========================================================================
    // Rescue Strategy
    // ========================================================================

    /**
     * Determine best rescue strategy (RBF or CPFP)
     *
     * Priority:
     * 1. RBF (if available) - cleaner, simpler
     * 2. CPFP (if RBF unavailable but CPFP viable)
     * 3. Wait (if neither available)
     *
     * @param txid Transaction hash
     * @param wallet Wallet manager
     * @param mempool Mempool instance
     * @return Recommended rescue strategy
     */
    RescueStrategy DetermineRescueStrategy(
        const uint256& txid,
        const WalletManager* wallet = nullptr,
        const Mempool* mempool = nullptr
    ) const;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Set maximum ancestor count for CPFP
     */
    void SetMaxAncestorCount(uint32_t max_ancestors);

    /**
     * Get current maximum ancestor count
     */
    uint32_t GetMaxAncestorCount() const;

private:
    // Configuration
    uint32_t max_ancestor_count_;       ///< Maximum ancestor count (default: 25)
    uint64_t min_relay_fee_rate_;       ///< Minimum relay fee rate (sat/vB)
};

} // namespace dinero
