#include "daemon/validation.h"
#include "daemon/tx_mempool.h"
#include "consensus/freeze_fork_activation.h"    // V5 Freeze Fork
#include "daemon/gui_websocket_events.hpp"
#include "common/logger.h"
#include <chrono>
#include <algorithm>

namespace dinero {

// AcceptToMemoryPool implementation

ATMPOutcome AcceptToMemoryPool(
    const Transaction& tx,
    TxMempool& pool,
    const MemPoolPolicy& policy,
    const UTXOView& utxos,
    bool test_accept,
    bool bypass_limits
) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Create validator
    MempoolValidator validator(policy);
    
    // Create validation context
    ValidationContext ctx;
    ctx.test_accept = test_accept;
    ctx.bypass_limits = bypass_limits;
    ctx.validation_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Validate transaction
    auto outcome = validator.ValidateTransaction(tx, pool, utxos, test_accept);
    
    // If validation passed and not test mode, add to mempool
    if (outcome.IsAccepted() && !test_accept) {
        TxMempoolEntry entry(tx, outcome.fee, ctx.validation_time);
        entry.height = utxos.GetHeight();
        
        if (!pool.AddUnchecked(entry)) {
            outcome.result = ATMPResult::Rejected;
            outcome.reason = "Failed to add to mempool";
            pool.IncrementRejects("mempool_add_failed");
        } else {
            dinero::g_logger.info("Accepted transaction to mempool: " + tx.GetTxId() +
                                 " (fee: " + std::to_string(outcome.fee) +
                                 ", feerate: " + std::to_string(outcome.feerate) + ")");

            // Broadcast newTransactions event to GUI WebSocket subscribers
            dinero_daemon::gui_events::BroadcastNewTransaction(
                tx.GetTxId(), outcome.fee, outcome.feerate, entry.GetSize());

            // Broadcast mempool stats update to GUI WebSocket subscribers
            // Note: This is a simplified implementation broadcasting after each tx acceptance
            // For accurate stats, pool.size() and pool.totalBytes() methods would be needed
            dinero_daemon::gui_events::BroadcastMempoolUpdate(1, entry.GetSize(), outcome.feerate);
        }
    }

    // Update statistics
    if (outcome.IsRejected()) {
        pool.IncrementRejects(outcome.reason);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    dinero::g_logger.debug("ATMP validation took " + std::to_string(duration.count()) + "ms for " + tx.GetTxId());
    
    return outcome;
}

// MempoolValidator implementation

MempoolValidator::MempoolValidator(const MemPoolPolicy& policy) 
    : policy_(policy) {
    tx_validator_ = std::make_unique<consensus::TransactionValidator>();
}

ATMPOutcome MempoolValidator::ValidateTransaction(
    const Transaction& tx,
    const TxMempool& pool,
    const UTXOView& utxos,
    bool test_accept
) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();
    
    // Step 1: Basic validation
    outcome = CheckBasicValidation(tx);
    if (outcome.IsRejected()) return outcome;

    // Step 2: Check if already exists
    if (pool.Exists(tx.GetTxId())) {
        outcome.result = ATMPResult::AlreadyExists;
        outcome.reason = "Transaction already in mempool";
        return outcome;
    }

    // Step 2.1: V5 Freeze Fork output gates.
    // Spec: docs/consensus/V5_FREEZE_FORK_SPEC.md
    // Evaluate against the height the tx would enter the chain at (next block).
    // These gates mirror the block-acceptance gates so miners don't build
    // templates containing txs that will fail validation.
    {
        uint32_t next_block_height = utxos.GetHeight() + 1;
        Chain chain = GetActiveChain();
        if (consensus::FreezeForkActivationParams::IsFreezeForkActive(next_block_height, chain)) {
            for (size_t out_idx = 0; out_idx < tx.vout.size(); out_idx++) {
                const auto& out = tx.vout[out_idx];

                // Gate 1: No confidential outputs.
                if (out.is_confidential) {
                    outcome.result = ATMPResult::InvalidTransaction;
                    outcome.reason = "freeze-fork: confidential output at vout " +
                                     std::to_string(out_idx) + " rejected at or above activation height";
                    return outcome;
                }

                // Gate 3: Taproot-only outputs (OP_RETURN permitted).
                if (!consensus::IsFreezeForkAllowedScript(out.scriptPubKey)) {
                    outcome.result = ATMPResult::InvalidTransaction;
                    outcome.reason = "freeze-fork: non-Taproot / non-OP_RETURN output at vout " +
                                     std::to_string(out_idx) + " rejected at or above activation height";
                    return outcome;
                }
            }
        }
    }

    // Legacy confidential lane has been excised from the active chain.
    // Ring / ring-covenant tx formats are rejected by version range below.
    if (tx.HasConfidentialOutputs()) {
        outcome.result = ATMPResult::InvalidTransaction;
        outcome.reason = "legacy private lane removed";
        return outcome;
    }

    // Step 3: Input validation
    outcome = CheckInputs(tx, utxos);
    if (outcome.IsRejected()) return outcome;
    
    // Step 4: Fee validation
    outcome = CheckFees(tx, utxos);
    if (outcome.IsRejected()) return outcome;
    
    // Step 5: Policy validation
    outcome = CheckPolicy(tx, pool);
    if (outcome.IsRejected()) return outcome;
    
    // Step 6: Conflict detection
    outcome = CheckConflicts(tx, pool);
    if (outcome.IsRejected()) return outcome;
    
    // Step 7: Package limits (if not bypassed)
    outcome = CheckPackageLimits(tx, pool);
    if (outcome.IsRejected()) return outcome;
    
    // Step 8: RBF validation (if applicable)
    if (policy_.rbf_enabled) {
        outcome = CheckRBF(tx, pool);
        if (outcome.IsRejected()) return outcome;
    }
    
    // All checks passed
    outcome.result = ATMPResult::Accepted;
    outcome.reason = "Accepted";
    
    return outcome;
}

ATMPOutcome MempoolValidator::CheckBasicValidation(const Transaction& tx) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();
    
    // Check transaction format
    if (tx.GetTxId().empty()) {
        outcome.result = ATMPResult::InvalidTransaction;
        outcome.reason = "Missing transaction ID";
        return outcome;
    }
    
    if (tx.vin.empty()) {
        outcome.result = ATMPResult::InvalidTransaction;
        outcome.reason = "Transaction has no inputs";
        return outcome;
    }
    
    if (tx.vout.empty()) {
        outcome.result = ATMPResult::InvalidTransaction;
        outcome.reason = "Transaction has no outputs";
        return outcome;
    }
    
    // Check transaction size (estimate)
    TxMempoolEntry temp_entry(tx, 0, 0);
    if (temp_entry.GetSize() > 100000) {  // 100KB max
        outcome.result = ATMPResult::ExceedsLimits;
        outcome.reason = "Transaction too large";
        return outcome;
    }
    
    // Check for standard transaction
    if (!IsStandardTransaction(tx)) {
        outcome.result = ATMPResult::NonStandard;
        outcome.reason = "Non-standard transaction";
        return outcome;
    }
    
    // Check dust outputs
    if (!CheckDustOutputs(tx)) {
        outcome.result = ATMPResult::Policy;
        outcome.reason = "Transaction contains dust outputs";
        return outcome;
    }
    
    outcome.result = ATMPResult::Accepted;
    return outcome;
}

ATMPOutcome MempoolValidator::CheckInputs(const Transaction& tx, const UTXOView& utxos) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();

    uint64_t total_input_value = 0;

    // Check all inputs
    for (const auto& input : tx.vin) {
        // Check if UTXO exists
        if (!utxos.HaveUTXO(input.prevout.txid, input.prevout.vout)) {
            outcome.result = ATMPResult::MissingInputs;
            outcome.reason = "Input UTXO not found: " + input.prevout.txid + ":" + std::to_string(input.prevout.vout);
            return outcome;
        }

        // Get UTXO value (only for transparent inputs)
        uint64_t value;
        std::string script;
        if (!utxos.GetUTXO(input.prevout.txid, input.prevout.vout, value, script)) {
            outcome.result = ATMPResult::MissingInputs;
            outcome.reason = "Failed to get UTXO: " + input.prevout.txid + ":" + std::to_string(input.prevout.vout);
            return outcome;
        }

        total_input_value += value;
    }

    // Calculate total output value (only transparent outputs)
    uint64_t total_output_value = 0;
    for (const auto& output : tx.vout) {
        if (!output.is_confidential) {
            total_output_value += output.value;
        }
    }

    // For mixed or transparent transactions, calculate fee from transparent values
    // For fully confidential transactions, use explicit fee
    if (tx.HasExplicitFee()) {
        // Confidential transaction with explicit fee
        outcome.fee = tx.GetExplicitFee();
    } else {
        // Transparent transaction - calculate fee normally
        if (total_input_value < total_output_value) {
            outcome.result = ATMPResult::InvalidTransaction;
            outcome.reason = "Insufficient input value";
            return outcome;
        }
        outcome.fee = total_input_value - total_output_value;
    }

    TxMempoolEntry temp_entry(tx, outcome.fee, 0);
    outcome.feerate = static_cast<double>(outcome.fee) / temp_entry.GetVSize();

    outcome.result = ATMPResult::Accepted;
    return outcome;
}

ATMPOutcome MempoolValidator::CheckFees(const Transaction& tx, const UTXOView& utxos) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();
    
    // Fee should already be calculated in CheckInputs
    if (outcome.fee == 0) {
        // Recalculate if needed
        uint64_t total_input_value = 0;
        uint64_t total_output_value = 0;
        
        for (const auto& input : tx.vin) {
            uint64_t value;
            std::string script;
            if (utxos.GetUTXO(input.prevout.txid, input.prevout.vout, value, script)) {
                total_input_value += value;
            }
        }
        
        for (const auto& output : tx.vout) {
            total_output_value += output.value;
        }
        
        outcome.fee = total_input_value - total_output_value;
        TxMempoolEntry temp_entry(tx, outcome.fee, 0);
        outcome.feerate = static_cast<double>(outcome.fee) / temp_entry.GetVSize();
    }
    
    // Check minimum fee
    uint64_t min_fee = CalculateMinimumFee(tx);
    if (outcome.fee < min_fee) {
        outcome.result = ATMPResult::FeeTooLow;
        outcome.reason = "Fee too low. Required: " + std::to_string(min_fee) + ", provided: " + std::to_string(outcome.fee);
        return outcome;
    }
    
    // Check fee rate
    double min_feerate = static_cast<double>(policy_.min_relay_feerate) / 1000.0;  // Convert sat/kB to sat/B
    if (outcome.feerate < min_feerate) {
        outcome.result = ATMPResult::FeeTooLow;
        outcome.reason = "Fee rate too low. Required: " + std::to_string(min_feerate) + ", provided: " + std::to_string(outcome.feerate);
        return outcome;
    }
    
    outcome.result = ATMPResult::Accepted;
    return outcome;
}

ATMPOutcome MempoolValidator::CheckPolicy(const Transaction& tx, const TxMempool& pool) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();
    
    // Check mempool size limits
    TxMempoolEntry temp_entry(tx, 0, 0);
    uint64_t tx_size = temp_entry.GetVSize();
    if (pool.Bytes() + tx_size > policy_.max_size_bytes) {
        outcome.result = ATMPResult::ExceedsLimits;
        outcome.reason = "Mempool full";
        return outcome;
    }
    
    // Additional policy checks would go here
    
    outcome.result = ATMPResult::Accepted;
    return outcome;
}

ATMPOutcome MempoolValidator::CheckConflicts(const Transaction& tx, const TxMempool& pool) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();
    
    // Check for double spending
    auto conflicts = GetConflictingTransactions(tx, pool);
    if (!conflicts.empty()) {
        outcome.result = ATMPResult::Conflict;
        outcome.reason = "Conflicts with mempool transaction: " + conflicts[0];
        return outcome;
    }
    
    outcome.result = ATMPResult::Accepted;
    return outcome;
}

ATMPOutcome MempoolValidator::CheckPackageLimits(const Transaction& tx, const TxMempool& pool) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();
    
    // For now, just accept - package limit checking would be more complex
    // and require dependency graph analysis
    
    outcome.result = ATMPResult::Accepted;
    return outcome;
}

ATMPOutcome MempoolValidator::CheckRBF(const Transaction& tx, const TxMempool& pool) const {
    ATMPOutcome outcome;
    outcome.txid = tx.GetTxId();
    
    // Basic RBF check - more sophisticated implementation would check
    // fee increments and signaling
    
    outcome.result = ATMPResult::Accepted;
    return outcome;
}

void MempoolValidator::UpdatePolicy(const MemPoolPolicy& policy) {
    policy_ = policy;
}

// Private helper methods

bool MempoolValidator::IsStandardTransaction(const Transaction& tx) const {
    // Basic standardness checks

    // v7: only transparent (v1, v2) and shielded txs are standard.
    // v3 (ring) and v4 (ring-covenant) were excised on Apr 17 2026.
    if (tx.version != 1 && tx.version != 2 &&
        !Transaction::IsShieldedVersion(tx.version)) {
        return false;
    }

    // Check inputs
    for (const auto& input : tx.vin) {
        // Check script size
        if (input.scriptSig.size() > 1650) {  // 1650 byte limit
            return false;
        }
    }

    // Check outputs
    for (const auto& output : tx.vout) {
        // Skip dust check for confidential outputs (Phase H)
        if (output.is_confidential) {
            // Confidential outputs have minimum checked via consensus rules
            continue;
        }

        // Check for dust (transparent outputs only)
        if (output.value < policy_.dust_threshold) {
            return false;
        }

        // Check script size
        if (output.scriptPubKey.size() > 10000) {  // 10KB limit
            return false;
        }
    }

    return true;
}

bool MempoolValidator::CheckDustOutputs(const Transaction& tx) const {
    for (const auto& output : tx.vout) {
        // Skip confidential outputs (Phase H)
        if (output.is_confidential) {
            continue;
        }

        // Check dust threshold for transparent outputs
        if (output.value < policy_.dust_threshold) {
            return false;
        }
    }
    return true;
}

uint64_t MempoolValidator::CalculateMinimumFee(const Transaction& tx) const {
    TxMempoolEntry temp_entry(tx, 0, 0);
    uint64_t size = temp_entry.GetVSize();
    return (size * policy_.min_relay_feerate) / 1000;  // Convert from sat/kB
}

std::vector<std::string> MempoolValidator::GetConflictingTransactions(const Transaction& tx, const TxMempool& pool) const {
    std::vector<std::string> conflicts;
    
    // Check if any input is already spent by mempool transactions
    for (const auto& input : tx.vin) {
        for (const auto& entry : pool.GetEntries()) {
            for (const auto& mempool_input : entry.tx.vin) {
                if (mempool_input.prevout.txid == input.prevout.txid && 
                    mempool_input.prevout.vout == input.prevout.vout) {
                    conflicts.push_back(entry.txid);
                }
            }
        }
    }
    
    return conflicts;
}

// BatchValidator implementation

BatchValidator::BatchValidator(TxMempool& pool, const UTXOView& utxos, const MemPoolPolicy& policy)
    : pool_(pool), utxos_(utxos), validator_(policy) {
}

std::vector<ValidationResult> BatchValidator::ValidateBatch(const std::vector<Transaction>& txs) {
    std::vector<ValidationResult> results;
    
    // Sort transactions by dependency order
    auto sorted_txs = SortByDependencies(txs);
    
    // Validate each transaction
    for (const auto& tx : sorted_txs) {
        ValidationResult result;
        result.txid = tx.GetTxId();
        
        auto outcome = validator_.ValidateTransaction(tx, pool_, utxos_, false);
        result.result = outcome.result;
        result.reason = outcome.reason;
        result.fee = outcome.fee;
        result.feerate = outcome.feerate;
        
        results.push_back(result);
    }
    
    return results;
}

std::vector<Transaction> BatchValidator::SortByDependencies(const std::vector<Transaction>& txs) const {
    // For now, just return as-is
    // A full implementation would build a dependency graph and topologically sort
    return txs;
}

// Mempool sync utilities

namespace mempool_sync {

din::Json ExportMempoolState(const TxMempool& pool) {
    din::Json state = din::obj();
    
    auto entries = pool.GetEntries();
    din::Json tx_array = din::arr();
    
    for (const auto& entry : entries) {
        din::Json tx_obj = din::obj();
        tx_obj["txid"] = entry.txid;
        tx_obj["size"] = static_cast<din::Json::Int64>(entry.size);
        tx_obj["vsize"] = static_cast<din::Json::Int64>(entry.vsize);
        tx_obj["fee"] = static_cast<din::Json::Int64>(entry.fee);
        tx_obj["feerate"] = entry.feerate;
        tx_obj["time"] = static_cast<din::Json::Int64>(entry.time);
        
        tx_array.append(tx_obj);
    }
    
    state["transactions"] = tx_array;
    state["count"] = static_cast<din::Json::Int64>(entries.size());
    state["bytes"] = static_cast<din::Json::Int64>(pool.Bytes());
    state["total_fees"] = static_cast<din::Json::Int64>(pool.GetTotalFees());
    
    return state;
}

std::vector<ValidationResult> ImportMempoolState(
    const din::Json& state, 
    TxMempool& pool, 
    const UTXOView& utxos,
    const MemPoolPolicy& policy
) {
    std::vector<ValidationResult> results;
    
    if (!state.isMember("transactions") || !state["transactions"].isArray()) {
        return results;
    }
    
    MempoolValidator validator(policy);
    
    for (const auto& tx_obj : state["transactions"]) {
        ValidationResult result;
        
        if (tx_obj.isMember("txid")) {
            result.txid = tx_obj["txid"].asString();
            
            // For import, we would need to reconstruct the full transaction
            // This is a simplified version
            result.result = ATMPResult::Accepted;
            result.reason = "Imported from state";
        }
        
        results.push_back(result);
    }
    
    return results;
}

din::Json GetMempoolDiff(const TxMempool& pool, const std::vector<std::string>& known_txids) {
    din::Json diff = din::obj();
    
    auto current_txids = pool.GetTxIds();
    std::set<std::string> known_set(known_txids.begin(), known_txids.end());
    std::set<std::string> current_set(current_txids.begin(), current_txids.end());
    
    // Find new transactions
    din::Json new_txs = din::arr();
    for (const auto& txid : current_txids) {
        if (known_set.find(txid) == known_set.end()) {
            new_txs.append(txid);
        }
    }
    
    // Find removed transactions
    din::Json removed_txs = din::arr();
    for (const auto& txid : known_txids) {
        if (current_set.find(txid) == current_set.end()) {
            removed_txs.append(txid);
        }
    }
    
    diff["added"] = new_txs;
    diff["removed"] = removed_txs;
    
    return diff;
}

} // namespace mempool_sync

} // namespace dinero
