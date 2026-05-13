/**
 * Phase 29: Mempool Covenant Policy (LEGACY - See Phase C.2)
 *
 * NOTE: This file is DEPRECATED - Phase C.2 implements covenant policy
 * directly in mempool.cpp without wallet dependencies.
 *
 * This file remains for reference but should NOT be used.
 * Phase C.2 implements clean policy enforcement:
 * - Covenant detection via script heuristic (no wallet)
 * - Ancestor safety rules (policy-only)
 * - DoS protection limits
 * - Proper boundary separation
 */

#pragma once

// Phase C.2: Fixed boundary violations - use primitives instead of wallet
#include "primitives/transaction.h"
#include "consensus/covenants.h"
#include "primitives/uint256.h"
#include <vector>
#include <string>
#include <set>
#include <cstdint>
#include <optional>

namespace dinero {
namespace mempool {

// ============================================================================
// Covenant Policy Configuration
// ============================================================================

/**
 * @brief Configuration for covenant policy enforcement
 */
struct CovenantPolicyConfig {
    // Maximum sizes
    uint32_t max_ctv_outputs = 100;              // Max outputs in CTV template
    uint32_t max_ctv_template_size = 10000;      // Max serialized template size
    uint32_t max_csfs_message_size = 520;        // Max CSFS message size
    uint32_t max_contract_state_size = 4096;     // Max contract state data
    uint32_t max_covenant_script_size = 10000;   // Max covenant script size

    // Depth limits (prevent recursive covenants)
    uint32_t max_covenant_depth = 10;            // Max nesting depth
    uint32_t max_contract_transitions = 1000;    // Max state counter

    // Fee requirements
    uint64_t min_covenant_fee_rate = 1;          // Min sat/vB for covenant tx
    uint64_t covenant_fee_premium_percent = 10;  // Extra fee for covenant validation

    // Anti-DoS
    uint32_t max_covenant_inputs_per_tx = 10;    // Max covenant inputs per tx
    uint32_t max_txhash_ops_per_script = 5;      // Max TXHASH ops per script
    bool allow_unconfirmed_covenant_spend = false; // Spend unconfirmed covenant UTXOs

    // RBF rules
    bool require_covenant_rbf_signal = true;     // Must signal RBF for covenant spend
    uint64_t covenant_rbf_min_fee_bump = 1000;   // Min fee increase for replacement
};

// ============================================================================
// Covenant Validation Result
// ============================================================================

/**
 * @brief Result of covenant policy validation
 */
struct CovenantValidationResult {
    bool accepted;                              // Transaction accepted to mempool
    std::string reject_reason;                  // Why rejected (if not accepted)
    int reject_code;                            // Numeric reject code

    // Detailed validation info
    struct InputValidation {
        uint32_t input_index;
        wallet::CovenantType type;
        bool valid;
        std::string error;
    };
    std::vector<InputValidation> input_validations;

    // Covenant metrics
    uint32_t covenant_inputs;                   // Number of covenant inputs
    uint32_t covenant_outputs;                  // Number of covenant outputs
    uint64_t covenant_value_spent;              // Value of covenant inputs
    uint64_t covenant_value_created;            // Value of covenant outputs

    // Fee analysis
    uint64_t base_fee;
    uint64_t covenant_fee_premium;
    uint64_t required_fee;
    bool fee_sufficient;

    // Warnings (accepted but flagged)
    std::vector<std::string> warnings;

    CovenantValidationResult() : accepted(false), reject_code(0),
        covenant_inputs(0), covenant_outputs(0), covenant_value_spent(0),
        covenant_value_created(0), base_fee(0), covenant_fee_premium(0),
        required_fee(0), fee_sufficient(false) {}
};

// ============================================================================
// Reject Codes for Covenant Violations
// ============================================================================

enum class CovenantRejectCode : int {
    VALID = 0,

    // CTV errors (100-119)
    CTV_HASH_MISMATCH = 100,
    CTV_OUTPUT_COUNT_MISMATCH = 101,
    CTV_OUTPUT_VALUE_MISMATCH = 102,
    CTV_OUTPUT_SCRIPT_MISMATCH = 103,
    CTV_VERSION_MISMATCH = 104,
    CTV_LOCKTIME_MISMATCH = 105,
    CTV_SEQUENCE_MISMATCH = 106,
    CTV_TEMPLATE_NOT_FOUND = 107,
    CTV_TOO_MANY_OUTPUTS = 108,

    // CSFS errors (120-139)
    CSFS_INVALID_SIGNATURE = 120,
    CSFS_INVALID_PUBKEY = 121,
    CSFS_MESSAGE_TOO_LARGE = 122,
    CSFS_DELEGATION_NOT_FOUND = 123,
    CSFS_DELEGATION_EXPIRED = 124,
    CSFS_DELEGATION_USED = 125,

    // TXHASH errors (140-159)
    TXHASH_TOO_MANY_OPS = 140,
    TXHASH_INVALID_FLAG = 141,
    TXHASH_INTROSPECTION_FAILED = 142,

    // CCV errors (160-179)
    CCV_STATE_HASH_MISMATCH = 160,
    CCV_COUNTER_NOT_INCREMENTED = 161,
    CCV_CODE_HASH_CHANGED = 162,
    CCV_STATE_TOO_LARGE = 163,
    CCV_MAX_TRANSITIONS_EXCEEDED = 164,

    // General covenant errors (180-199)
    COVENANT_SCRIPT_TOO_LARGE = 180,
    COVENANT_DEPTH_EXCEEDED = 181,
    COVENANT_TOO_MANY_INPUTS = 182,
    COVENANT_FEE_INSUFFICIENT = 183,
    COVENANT_UNCONFIRMED_PARENT = 184,
    COVENANT_RBF_NOT_SIGNALED = 185,
    COVENANT_RBF_FEE_TOO_LOW = 186,
    COVENANT_UNKNOWN_TYPE = 187,

    // Script errors (200-219)
    SCRIPT_INVALID = 200,
    SCRIPT_EVALUATION_FAILED = 201
};

// ============================================================================
// Covenant Policy Validator
// ============================================================================

/**
 * @brief Validates transactions against covenant policy rules
 *
 * Called by mempool before accepting transactions.
 * Enforces all covenant-specific validation rules.
 */
class CovenantPolicyValidator {
public:
    explicit CovenantPolicyValidator(const CovenantPolicyConfig& config);

    /**
     * @brief Validate transaction for mempool acceptance
     *
     * @param tx Transaction to validate
     * @param utxo_lookup Function to lookup UTXOs
     * @param covenant_wallet Optional covenant wallet for template lookup
     * @return Validation result with detailed info
     */
    CovenantValidationResult validate(
        const Transaction& tx,
        std::function<std::optional<wallet::CovenantUTXO>(const std::string&, uint32_t)> utxo_lookup,
        wallet::CovenantWallet* covenant_wallet = nullptr) const;

    /**
     * @brief Check if transaction has any covenant inputs
     */
    bool hasCovenantInputs(
        const Transaction& tx,
        std::function<std::optional<wallet::CovenantUTXO>(const std::string&, uint32_t)> utxo_lookup) const;

    /**
     * @brief Estimate covenant fee premium
     */
    uint64_t estimateCovenantFeePremium(
        const Transaction& tx,
        std::function<std::optional<wallet::CovenantUTXO>(const std::string&, uint32_t)> utxo_lookup) const;

    /**
     * @brief Get human-readable error for reject code
     */
    static std::string rejectCodeToString(CovenantRejectCode code);

private:
    CovenantPolicyConfig config_;

    // Individual validation methods
    CovenantValidationResult validateCTVInput(
        const Transaction& tx,
        uint32_t input_index,
        const wallet::CovenantUTXO& utxo,
        wallet::CovenantWallet* covenant_wallet) const;

    CovenantValidationResult validateCSFSInput(
        const Transaction& tx,
        uint32_t input_index,
        const wallet::CovenantUTXO& utxo,
        wallet::CovenantWallet* covenant_wallet) const;

    CovenantValidationResult validateCCVInput(
        const Transaction& tx,
        uint32_t input_index,
        const wallet::CovenantUTXO& utxo,
        wallet::CovenantWallet* covenant_wallet) const;

    // Script analysis
    uint32_t countTxHashOps(const std::vector<uint8_t>& script) const;
    uint32_t measureCovenantDepth(const std::vector<uint8_t>& script) const;

    // Fee calculation
    uint64_t calculateRequiredFee(
        const Transaction& tx,
        uint32_t covenant_inputs,
        uint64_t fee_rate) const;
};

// ============================================================================
// Covenant Mempool Tracker
// ============================================================================

/**
 * @brief Tracks covenant transactions in mempool
 *
 * Maintains state for covenant-specific mempool policies:
 * - Prevents double-spending of covenant UTXOs
 * - Tracks pending contract state transitions
 * - Enforces RBF rules for covenant outputs
 */
class CovenantMempoolTracker {
public:
    CovenantMempoolTracker();

    /**
     * @brief Add transaction to tracker
     *
     * @param tx Transaction being added to mempool
     * @param covenant_inputs List of covenant inputs being spent (Phase M.0: uint256 txid)
     * @return true if added successfully
     */
    bool addTransaction(
        const Transaction& tx,
        const std::vector<std::pair<uint256, uint32_t>>& covenant_inputs);

    /**
     * @brief Remove transaction from tracker
     *
     * @param txid Transaction ID to remove (Phase M.0: uint256 identity)
     */
    void removeTransaction(const uint256& txid);

    /**
     * @brief Check if covenant UTXO is already being spent in mempool
     *
     * @param txid UTXO txid (Phase M.0: uint256 identity)
     * @param vout UTXO vout
     * @return txid of conflicting transaction, or nullopt if none
     */
    std::optional<uint256> getConflictingTx(const uint256& txid, uint32_t vout) const;

    /**
     * @brief Check if contract state transition is pending
     *
     * @param contract_id Contract ID
     * @return true if contract has pending transition in mempool
     */
    bool hasContractPendingTransition(const std::string& contract_id) const;

    /**
     * @brief Get pending contract transitions
     *
     * @param contract_id Contract ID
     * @return List of pending transition txids
     */
    std::vector<std::string> getContractPendingTransitions(
        const std::string& contract_id) const;

    /**
     * @brief Clear all tracking data
     */
    void clear();

    /**
     * @brief Get tracking statistics
     */
    struct Stats {
        uint64_t tracked_transactions;
        uint64_t tracked_covenant_inputs;
        uint64_t tracked_contract_transitions;
    };
    Stats getStats() const;

private:
    // Map: outpoint (txid:vout) -> spending txid
    std::map<std::string, std::string> covenant_spends_;

    // Map: contract_id -> list of pending transition txids
    std::map<std::string, std::vector<std::string>> contract_transitions_;

    // Map: txid -> list of covenant inputs it spends
    std::map<std::string, std::vector<std::string>> tx_covenant_inputs_;

    mutable std::mutex mutex_;

    std::string makeOutpoint(const uint256& txid, uint32_t vout) const;
};

// ============================================================================
// Mining Policy for Covenants
// ============================================================================

/**
 * @brief Mining-specific covenant policies
 *
 * Additional rules for block template construction:
 * - Priority scoring for covenant transactions
 * - Batching opportunities (multiple CTV outputs)
 * - Contract closure bundling
 */
struct CovenantMiningPolicy {
    // Priority adjustments
    double ctv_priority_multiplier = 1.0;       // CTV transactions priority
    double csfs_priority_multiplier = 1.0;      // CSFS transactions priority
    double ccv_priority_multiplier = 0.9;       // CCV (slightly lower, more complex)

    // Batching
    bool enable_ctv_batching = true;            // Batch related CTV spends
    uint32_t max_batch_size = 10;               // Max transactions per batch

    // Contract handling
    bool prioritize_contract_closures = true;   // Prefer closing contracts
    uint32_t max_contract_transitions_per_block = 100;

    /**
     * @brief Calculate adjusted priority for covenant transaction
     */
    double calculatePriority(
        const Transaction& tx,
        double base_priority,
        wallet::CovenantType type) const;

    /**
     * @brief Check if transactions can be batched
     */
    bool canBatch(
        const Transaction& tx1,
        const Transaction& tx2,
        wallet::CovenantType type) const;
};

} // namespace mempool
} // namespace dinero
