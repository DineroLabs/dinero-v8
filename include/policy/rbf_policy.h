#pragma once

#include "primitives/uint256.h"  // Phase M.0: uint256 type
#include "mining/ct_selection_policy.h"
#include <vector>
#include <string>
#include <unordered_set>
#include <cstdint>

namespace dinero {

// Forward declarations
struct Transaction;
struct MempoolEntry;
class Mempool;  // Phase B: For entry lookup in RBF validation

using dinero::uint256;  // Phase M.0: Make uint256 available

namespace policy {

/**
 * @file rbf_policy.h
 * @brief BIP125 Replace-By-Fee (RBF) Policy Implementation
 *
 * MAINNET BLOCKER FIX: Complete Bitcoin-compatible RBF validation
 *
 * BIP125 defines opt-in Replace-By-Fee with 5 rules:
 * 1. Original transaction signals replaceability (sequence < 0xfffffffe)
 * 2. Replacement doesn't add new unconfirmed inputs
 * 3. Replacement pays higher absolute fee
 * 4. Replacement pays for own bandwidth (fee delta >= replaced tx sizes * minrelay)
 * 5. No more than 100 original transactions replaced
 *
 * References:
 * - https://github.com/bitcoin/bips/blob/master/bip-0125.mediawiki
 * - Bitcoin Core src/policy/rbf.cpp
 */

/**
 * RBF validation result codes
 */
enum class RBFValidationResult {
    VALID,                        // Replacement is valid

    // Rule 1 violations
    NOT_SIGNALED,                 // Original tx doesn't signal RBF (sequence >= 0xfffffffe)

    // Rule 2 violations
    NEW_UNCONFIRMED_INPUT,        // Replacement adds new unconfirmed inputs

    // Rule 3 violations
    INSUFFICIENT_FEE,             // Replacement fee not higher than sum of replaced fees

    // Rule 4 violations
    INSUFFICIENT_FEE_RATE,        // Fee delta doesn't pay for replacement bandwidth

    // Rule 5 violations
    TOO_MANY_REPLACEMENTS,        // More than 100 transactions would be replaced

    // General errors
    ORIGINAL_NOT_FOUND,           // Original transaction not in mempool
    REPLACEMENT_ADDS_UTXOS        // Replacement would require replacing confirmed txs
};

/**
 * @brief Conflict set for a replacement transaction
 *
 * Tracks all transactions that would be evicted by a replacement,
 * including the direct conflict and all dependent descendants.
 */
struct RBFConflictSet {
    std::unordered_set<uint256> direct_conflicts;     // Txs spending same inputs (Phase M.0: uint256)
    std::unordered_set<uint256> descendant_conflicts; // Children of direct conflicts (Phase M.0: uint256)

    uint64_t total_fee;              // Sum of all fees being replaced
    size_t total_size;                // Sum of all sizes being replaced
    size_t total_virtual_size;        // Sum of all virtual sizes being replaced
    size_t total_effective_vsize;     // Sum of CT-adjusted vsize for bandwidth checks
    size_t conflict_count;            // Total number of transactions

    RBFConflictSet()
        : total_fee(0)
        , total_size(0)
        , total_virtual_size(0)
        , total_effective_vsize(0)
        , conflict_count(0) {}
};

/**
 * @brief BIP125 Replace-By-Fee policy enforcer
 */
class RBFPolicy {
public:
    /**
     * BIP125 configuration
     */
    struct Config {
        bool enable_rbf;                           // Enable RBF globally
        uint64_t min_relay_fee_rate;              // Minimum relay fee (una/KB)
        uint64_t incremental_relay_fee;           // Incremental relay fee for replacement
        size_t max_replacement_count;              // Max transactions replaced (BIP125 rule 5)

        Config()
            : enable_rbf(true)
            , min_relay_fee_rate(1000)
            , incremental_relay_fee(1000)
            , max_replacement_count(100) {}
    };

    explicit RBFPolicy(const Config& config = Config{});

    // ═══════════════════════════════════════════════════════════════════════
    // BIP125 Rule Validation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * BIP125 Rule #1: Original transaction signals replaceability
     *
     * A transaction signals RBF if any input has nSequence < 0xfffffffe.
     * This is the opt-in mechanism.
     *
     * @param tx Transaction to check
     * @return true if transaction signals RBF
     */
    bool isRBFSignaled(const Transaction& tx) const;

    /**
     * BIP125 Rule #2: Replacement doesn't add new unconfirmed inputs
     *
     * All inputs of the replacement must either:
     * - Spend same outputs as original transaction, OR
     * - Come from confirmed transactions
     *
     * This prevents pinning attacks where an attacker adds unconfirmed
     * inputs to make the replacement expensive.
     *
     * @param replacement_tx The new transaction
     * @param original_entries Mempool entries being replaced
     * @param mempool_entries All current mempool entries
     * @return true if rule passes
     */
    bool checkNoNewUnconfirmed(
        const Transaction& replacement_tx,
        const std::vector<MempoolEntry>& original_entries,
        const std::unordered_set<uint256>& mempool_txids  // Phase M.0: uint256
    ) const;

    /**
     * BIP125 Rule #3: Replacement pays higher absolute fee
     *
     * replacement_fee > sum(original_fees)
     *
     * @param replacement_fee Fee of replacement transaction
     * @param conflict_set All transactions being replaced
     * @return true if replacement fee is higher
     */
    bool checkHigherFee(
        uint64_t replacement_fee,
        const RBFConflictSet& conflict_set
    ) const;

    /**
     * BIP125 Rule #4: Replacement pays for own bandwidth
     *
     * The replacement must pay for bandwidth of evicting original transactions.
     * Formula: fee_delta >= sum(replaced_sizes) * minrelayfee
     *
     * Where fee_delta = replacement_fee - sum(original_fees)
     *
     * This prevents DoS by making replacements increasingly expensive.
     *
     * @param replacement_fee Fee of replacement transaction
     * @param replacement_effective_vsize Effective vsize of replacement (for fee calc)
     * @param conflict_set All transactions being replaced
     * @return true if bandwidth is paid for
     */
    bool checkPaysForBandwidth(
        uint64_t replacement_fee,
        size_t replacement_effective_vsize,
        const RBFConflictSet& conflict_set
    ) const;

    /**
     * BIP125 Rule #5: No more than 100 transactions replaced
     *
     * Limits the number of transactions evicted to prevent excessive
     * mempool churn.
     *
     * @param conflict_set All transactions being replaced
     * @return true if count within limit
     */
    bool checkReplacementLimit(const RBFConflictSet& conflict_set) const;

    // ═══════════════════════════════════════════════════════════════════════
    // Complete RBF Validation
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Validate RBF replacement against all BIP125 rules
     *
     * This is the main entry point for RBF validation.
     *
     * @param replacement_tx The new transaction
     * @param replacement_fee Fee of replacement
     * @param conflict_set Transactions being replaced
     * @param mempool_txids All current mempool transaction IDs
     * @param error Output parameter for error message
     * @return RBFValidationResult indicating success or specific failure
     */
    RBFValidationResult validateReplacement(
        const Transaction& replacement_tx,
        uint64_t replacement_fee,
        const RBFConflictSet& conflict_set,
        const mining::CTSelectionConfig& ct_config,
        const std::unordered_set<uint256>& mempool_txids,  // Phase M.0: uint256
        const std::vector<MempoolEntry>& original_entries,
        std::string& error
    ) const;

    /**
     * Build conflict set for a replacement transaction
     *
     * Identifies all transactions that would be evicted:
     * 1. Direct conflicts (spending same inputs)
     * 2. Descendant conflicts (children of direct conflicts)
     *
     * @param replacement_tx The new transaction
     * @param mempool_entries All current mempool entries
     * @return Complete conflict set
     */
    static RBFConflictSet buildConflictSet(
        const Transaction& replacement_tx,
        const std::vector<MempoolEntry>& mempool_entries,
        const mining::CTSelectionConfig& ct_config
    );

    // ═══════════════════════════════════════════════════════════════════════
    // Utilities
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Get human-readable error message for validation result
     */
    std::string getErrorMessage(RBFValidationResult result) const;

    /**
     * Calculate fee delta for replacement
     *
     * fee_delta = replacement_fee - sum(original_fees)
     */
    static uint64_t calculateFeeDelta(
        uint64_t replacement_fee,
        const RBFConflictSet& conflict_set
    );

    /**
     * Calculate minimum relay fee for a given size
     */
    uint64_t calculateMinRelayFee(size_t size_bytes) const;

    // Configuration access
    const Config& getConfig() const { return config_; }
    void updateConfig(const Config& config) { config_ = config; }

    /**
     * Runtime enable/disable RBF
     * Used by MempoolService to apply config settings
     */
    void setEnabled(bool enabled) { config_.enable_rbf = enabled; }
    bool isEnabled() const { return config_.enable_rbf; }

private:
    Config config_;

    // Helper: Check if transaction spends any outputs from unconfirmed tx
    bool spendsUnconfirmedOutput(
        const Transaction& tx,
        const std::unordered_set<std::string>& mempool_txids
    ) const;
};

} // namespace policy
} // namespace dinero
