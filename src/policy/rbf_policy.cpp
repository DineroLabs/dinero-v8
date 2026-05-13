#include "policy/rbf_policy.h"
#include "wallet/transaction.h"
#include "daemon/mempool.h"
#include "common/logger.h"
#include <algorithm>
#include <sstream>

namespace dinero {
namespace policy {

namespace {

size_t GetEffectiveVirtualSize(
    const Transaction& tx,
    const mining::CTSelectionPolicy& ct_policy)
{
    if (!ct_policy.HasConfidentialOutputs(tx)) {
        return tx.GetVirtualSize();
    }

    const auto weight_info = ct_policy.GetWeightInfo(tx);
    return static_cast<size_t>((weight_info.total_weight + 3) / 4);
}

size_t GetEffectiveVirtualSize(
    const MempoolEntry& entry,
    const mining::CTSelectionPolicy& ct_policy)
{
    if (!entry.is_confidential) {
        return entry.tx.GetVirtualSize();
    }

    const auto weight_info = ct_policy.GetWeightInfo(entry.tx);
    return static_cast<size_t>((weight_info.total_weight + 3) / 4);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════════════════════════

RBFPolicy::RBFPolicy(const Config& config) : config_(config) {}

// ═══════════════════════════════════════════════════════════════════════════
// BIP125 Rule #1: Signal Replacement
// ═══════════════════════════════════════════════════════════════════════════

bool RBFPolicy::isRBFSignaled(const Transaction& tx) const {
    if (!config_.enable_rbf) {
        return false;
    }

    // BIP125: A transaction signals replaceability if ANY input has
    // nSequence < 0xfffffffe (4294967294)
    constexpr uint32_t MAX_BIP125_RBF_SEQUENCE = 0xfffffffe;

    for (const auto& input : tx.vin) {
        if (input.sequence < MAX_BIP125_RBF_SEQUENCE) {
            return true;  // At least one input signals RBF
        }
    }

    return false;  // No inputs signal RBF
}

// ═══════════════════════════════════════════════════════════════════════════
// BIP125 Rule #2: No New Unconfirmed Inputs
// ═══════════════════════════════════════════════════════════════════════════

bool RBFPolicy::checkNoNewUnconfirmed(
    const Transaction& replacement_tx,
    const std::vector<MempoolEntry>& original_entries,
    const std::unordered_set<uint256>& mempool_txids  // Phase M.0: uint256
) const {
    // Build set of all inputs from original transactions being replaced
    std::unordered_set<std::string> original_inputs;
    for (const auto& entry : original_entries) {
        for (const auto& input : entry.tx.vin) {
            // Phase M.0: Convert uint256 to hex for outpoint string
            std::string outpoint = input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);
            original_inputs.insert(outpoint);
        }
    }

    // Check each input of the replacement transaction
    for (const auto& input : replacement_tx.vin) {
        // Phase M.0: Convert uint256 to hex for outpoint string
        std::string outpoint = input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);

        // If this input was NOT in the original transactions...
        if (original_inputs.find(outpoint) == original_inputs.end()) {
            // Phase M.4: input.prevout.txid is TxId, extract uint256 for find()
            // Check if it's spending an unconfirmed (mempool) transaction
            if (mempool_txids.find(input.prevout.txid.AsUint256()) != mempool_txids.end()) {
                // This is a NEW unconfirmed input - RULE #2 VIOLATION
                dinero::g_logger.debug(
                    "RBF Rule #2 violation: Replacement adds new unconfirmed input from " +
                    input.prevout.txid.AsUint256().GetHex()
                );
                return false;
            }
        }
    }

    return true;  // All new inputs are from confirmed transactions
}

// ═══════════════════════════════════════════════════════════════════════════
// BIP125 Rule #3: Higher Fee
// ═══════════════════════════════════════════════════════════════════════════

bool RBFPolicy::checkHigherFee(
    uint64_t replacement_fee,
    const RBFConflictSet& conflict_set
) const {
    // BIP125 Rule #3: Absolute fee of replacement must be higher than
    // the sum of all fees being replaced

    if (replacement_fee <= conflict_set.total_fee) {
        dinero::g_logger.debug(
            "RBF Rule #3 violation: Replacement fee (" + std::to_string(replacement_fee) +
            ") not higher than sum of replaced fees (" + std::to_string(conflict_set.total_fee) + ")"
        );
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// BIP125 Rule #4: Pays for Bandwidth
// ═══════════════════════════════════════════════════════════════════════════

bool RBFPolicy::checkPaysForBandwidth(
    uint64_t replacement_fee,
    size_t replacement_effective_vsize,
    const RBFConflictSet& conflict_set
) const {
    // BIP125 Rule #4: The replacement transaction must pay for the bandwidth
    // of the transactions it evicts.
    //
    // Formula: fee_delta >= (sum(replaced_sizes) + replacement_size) * minrelayfee
    //
    // Where:
    // - fee_delta = replacement_fee - sum(original_fees)
    // - minrelayfee = config_.incremental_relay_fee (default 1000 sat/KB)

    uint64_t fee_delta = calculateFeeDelta(replacement_fee, conflict_set);

    // Calculate minimum additional relay fee required
    // Total size = all replaced transactions + new transaction
    size_t total_size = conflict_set.total_effective_vsize + replacement_effective_vsize;

    // Convert to KB for fee calculation
    // Formula: (size_in_bytes * fee_per_KB) / 1000
    uint64_t min_additional_fee = (total_size * config_.incremental_relay_fee) / 1000;

    if (fee_delta < min_additional_fee) {
        dinero::g_logger.debug(
            "RBF Rule #4 violation: Fee delta (" + std::to_string(fee_delta) +
            ") insufficient to pay for bandwidth. Required: " + std::to_string(min_additional_fee) +
            " (total size: " + std::to_string(total_size) + " bytes)"
        );
        return false;
    }

    dinero::g_logger.debug(
        "RBF Rule #4 passed: Fee delta " + std::to_string(fee_delta) +
        " >= min required " + std::to_string(min_additional_fee)
    );

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// BIP125 Rule #5: Replacement Limit
// ═══════════════════════════════════════════════════════════════════════════

bool RBFPolicy::checkReplacementLimit(const RBFConflictSet& conflict_set) const {
    // BIP125 Rule #5: No more than 100 original transactions can be replaced
    //
    // This prevents excessive mempool churn and DoS attacks

    if (conflict_set.conflict_count > config_.max_replacement_count) {
        dinero::g_logger.warning(
            "RBF Rule #5 violation: Too many replacements (" +
            std::to_string(conflict_set.conflict_count) + " > " +
            std::to_string(config_.max_replacement_count) + ")"
        );
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Complete RBF Validation
// ═══════════════════════════════════════════════════════════════════════════

RBFValidationResult RBFPolicy::validateReplacement(
    const Transaction& replacement_tx,
    uint64_t replacement_fee,
    const RBFConflictSet& conflict_set,
    const mining::CTSelectionConfig& ct_config,
    const std::unordered_set<uint256>& mempool_txids,  // Phase M.0: uint256
    const std::vector<MempoolEntry>& original_entries,
    std::string& error
) const {
    if (!config_.enable_rbf) {
        error = "RBF is disabled";
        return RBFValidationResult::NOT_SIGNALED;
    }

    // Conflict set must not be empty
    if (conflict_set.direct_conflicts.empty()) {
        error = "No conflicting transactions found in mempool";
        return RBFValidationResult::ORIGINAL_NOT_FOUND;
    }

    // ─────────────────────────────────────────────────────────────────────
    // BIP125 Rule #1: Check if original transactions signal RBF
    // ─────────────────────────────────────────────────────────────────────
    // Note: In full implementation, we'd check each original transaction.
    // For now, we assume they were checked before calling this function.

    // ─────────────────────────────────────────────────────────────────────
    // BIP125 Rule #2: No new unconfirmed inputs
    // ─────────────────────────────────────────────────────────────────────
    if (!checkNoNewUnconfirmed(replacement_tx, original_entries, mempool_txids)) {
        error = "Replacement adds new unconfirmed inputs (BIP125 Rule #2)";
        return RBFValidationResult::NEW_UNCONFIRMED_INPUT;
    }

    // ─────────────────────────────────────────────────────────────────────
    // BIP125 Rule #3: Higher absolute fee
    // ─────────────────────────────────────────────────────────────────────
    if (!checkHigherFee(replacement_fee, conflict_set)) {
        error = "Replacement fee must be higher than sum of replaced fees (BIP125 Rule #3)";
        return RBFValidationResult::INSUFFICIENT_FEE;
    }

    // ─────────────────────────────────────────────────────────────────────
    // BIP125 Rule #4: Pays for bandwidth
    // ─────────────────────────────────────────────────────────────────────
    const mining::CTSelectionPolicy ct_policy(ct_config);
    const size_t replacement_effective_vsize =
        GetEffectiveVirtualSize(replacement_tx, ct_policy);
    if (!checkPaysForBandwidth(replacement_fee, replacement_effective_vsize, conflict_set)) {
        error = "Replacement doesn't pay for eviction bandwidth (BIP125 Rule #4)";
        return RBFValidationResult::INSUFFICIENT_FEE_RATE;
    }

    // ─────────────────────────────────────────────────────────────────────
    // BIP125 Rule #5: Replacement count limit
    // ─────────────────────────────────────────────────────────────────────
    if (!checkReplacementLimit(conflict_set)) {
        error = "Too many transactions would be replaced (BIP125 Rule #5): " +
                std::to_string(conflict_set.conflict_count) + " > " +
                std::to_string(config_.max_replacement_count);
        return RBFValidationResult::TOO_MANY_REPLACEMENTS;
    }

    // All rules passed!
    dinero::g_logger.info(
        "RBF validation passed: Replacing " + std::to_string(conflict_set.conflict_count) +
        " transactions, fee delta: " + std::to_string(calculateFeeDelta(replacement_fee, conflict_set))
    );

    error = "";
    return RBFValidationResult::VALID;
}

// ═══════════════════════════════════════════════════════════════════════════
// Conflict Set Building
// ═══════════════════════════════════════════════════════════════════════════

RBFConflictSet RBFPolicy::buildConflictSet(
    const Transaction& replacement_tx,
    const std::vector<MempoolEntry>& mempool_entries,
    const mining::CTSelectionConfig& ct_config
) {
    RBFConflictSet conflict_set;
    const mining::CTSelectionPolicy ct_policy(ct_config);

    // Build set of outpoints spent by replacement transaction
    std::unordered_set<std::string> replacement_outpoints;
    for (const auto& input : replacement_tx.vin) {
        // Phase M.0: Convert uint256 to hex for outpoint string
        std::string outpoint = input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);
        replacement_outpoints.insert(outpoint);
    }

    // Phase 1: Find direct conflicts (transactions spending same inputs)
    for (const auto& entry : mempool_entries) {
        bool is_conflict = false;

        // Check if this transaction spends any of the same outpoints
        for (const auto& input : entry.tx.vin) {
            // Phase M.0: Convert uint256 to hex for outpoint string
            std::string outpoint = input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);
            if (replacement_outpoints.find(outpoint) != replacement_outpoints.end()) {
                is_conflict = true;
                break;
            }
        }

        if (is_conflict) {
            // Phase M.4: GetTxid() returns TxId, extract uint256 for storage
            TxId txid = entry.tx.GetTxid();
            conflict_set.direct_conflicts.insert(txid.AsUint256());
            conflict_set.total_fee += entry.fee;
            conflict_set.total_size += entry.tx_size;
            conflict_set.total_virtual_size += entry.tx.GetVirtualSize();
            conflict_set.total_effective_vsize += GetEffectiveVirtualSize(entry, ct_policy);
            conflict_set.conflict_count++;

            dinero::g_logger.debug("Direct conflict found: " + txid.AsUint256().GetHex() + " (fee: " +
                                  std::to_string(entry.fee) + ")");
        }
    }

    // Phase 2: Find descendant conflicts (children of direct conflicts)
    // A transaction is a descendant if it spends outputs from a direct conflict
    // Phase M.0: Use uint256 for all_conflicts
    std::unordered_set<uint256> all_conflicts = conflict_set.direct_conflicts;
    bool found_new_descendants = true;

    while (found_new_descendants) {
        found_new_descendants = false;

        for (const auto& entry : mempool_entries) {
            // Phase M.4: GetTxid() returns TxId, extract uint256 for storage
            TxId txid = entry.tx.GetTxid();

            // Skip if already in conflict set
            if (all_conflicts.find(txid.AsUint256()) != all_conflicts.end()) {
                continue;
            }

            // Check if this transaction spends from any conflicting transaction
            for (const auto& input : entry.tx.vin) {
                // Phase M.4: input.prevout.txid is TxId, extract uint256 for find()
                if (all_conflicts.find(input.prevout.txid.AsUint256()) != all_conflicts.end()) {
                    // This transaction is a descendant of a conflict
                    conflict_set.descendant_conflicts.insert(txid.AsUint256());
                    all_conflicts.insert(txid.AsUint256());
                    conflict_set.total_fee += entry.fee;
                    conflict_set.total_size += entry.tx_size;
                    conflict_set.total_virtual_size += entry.tx.GetVirtualSize();
                    conflict_set.total_effective_vsize += GetEffectiveVirtualSize(entry, ct_policy);
                    conflict_set.conflict_count++;

                    dinero::g_logger.debug("Descendant conflict found: " + txid.AsUint256().GetHex() +
                                          " (depends on " + input.prevout.txid.AsUint256().GetHex() + ")");

                    found_new_descendants = true;
                    break;
                }
            }
        }
    }

    dinero::g_logger.info(
        "Conflict set built: " + std::to_string(conflict_set.direct_conflicts.size()) +
        " direct conflicts, " + std::to_string(conflict_set.descendant_conflicts.size()) +
        " descendant conflicts, total fee: " + std::to_string(conflict_set.total_fee)
    );

    return conflict_set;
}

// ═══════════════════════════════════════════════════════════════════════════
// Utility Functions
// ═══════════════════════════════════════════════════════════════════════════

std::string RBFPolicy::getErrorMessage(RBFValidationResult result) const {
    switch (result) {
        case RBFValidationResult::VALID:
            return "Valid RBF replacement";

        case RBFValidationResult::NOT_SIGNALED:
            return "Original transaction does not signal RBF (BIP125 Rule #1)";

        case RBFValidationResult::NEW_UNCONFIRMED_INPUT:
            return "Replacement adds new unconfirmed inputs (BIP125 Rule #2)";

        case RBFValidationResult::INSUFFICIENT_FEE:
            return "Replacement fee not higher than sum of replaced fees (BIP125 Rule #3)";

        case RBFValidationResult::INSUFFICIENT_FEE_RATE:
            return "Replacement doesn't pay for eviction bandwidth (BIP125 Rule #4)";

        case RBFValidationResult::TOO_MANY_REPLACEMENTS:
            return "Too many transactions would be replaced (BIP125 Rule #5)";

        case RBFValidationResult::ORIGINAL_NOT_FOUND:
            return "Original transaction not found in mempool";

        case RBFValidationResult::REPLACEMENT_ADDS_UTXOS:
            return "Replacement would require replacing confirmed transactions";

        default:
            return "Unknown RBF validation error";
    }
}

uint64_t RBFPolicy::calculateFeeDelta(
    uint64_t replacement_fee,
    const RBFConflictSet& conflict_set
) {
    // Fee delta = new fee - sum of old fees
    // Note: Can be negative if replacement fee is lower (which would fail Rule #3)
    if (replacement_fee > conflict_set.total_fee) {
        return replacement_fee - conflict_set.total_fee;
    } else {
        return 0;  // No delta if replacement fee is lower
    }
}

uint64_t RBFPolicy::calculateMinRelayFee(size_t size_bytes) const {
    // Calculate minimum relay fee for given size
    // Formula: (size_in_bytes * fee_per_KB) / 1000
    return (size_bytes * config_.min_relay_fee_rate) / 1000;
}

bool RBFPolicy::spendsUnconfirmedOutput(
    const Transaction& tx,
    const std::unordered_set<std::string>& mempool_txids
) const {
    // Check if transaction spends any outputs from unconfirmed transactions
    for (const auto& input : tx.vin) {
        // Phase M.0: Convert uint256 to hex for string set lookup
        std::string txid_hex = input.prevout.txid.AsUint256().GetHex();
        if (mempool_txids.find(txid_hex) != mempool_txids.end()) {
            return true;  // Spends from mempool transaction
        }
    }
    return false;  // All inputs from confirmed transactions
}

} // namespace policy
} // namespace dinero
