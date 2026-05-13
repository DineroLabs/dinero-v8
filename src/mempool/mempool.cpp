#include "mempool/mempool.h"
#include "mempool/coins_view_mempool.h"
#include "consensus/tx_validation.h"
#include "consensus/coins_db.h"  // For CoinsViewCache complete type
#include <algorithm>
#include <ctime>
#include <deque>
#include <iostream>  // P2P debug: for std::cerr

namespace dinero {
namespace mempool {

// ============================================================================
// Phase 25: Mempool Implementation
// ============================================================================

const char* MempoolAcceptResultToString(MempoolAcceptResult result) {
    switch (result) {
        case MempoolAcceptResult::OK:
            return "OK";
        case MempoolAcceptResult::ALREADY_IN_MEMPOOL:
            return "Transaction already in mempool";
        case MempoolAcceptResult::ALREADY_IN_CHAIN:
            return "Transaction already in blockchain";
        case MempoolAcceptResult::INVALID_TX:
            return "Transaction failed consensus validation";
        case MempoolAcceptResult::INSUFFICIENT_FEE:
            return "Fee rate too low";
        case MempoolAcceptResult::MEMPOOL_FULL:
            return "Mempool at capacity";
        case MempoolAcceptResult::CONFLICTS_WITH_MEMPOOL:
            return "Double spend without RBF";
        case MempoolAcceptResult::RBF_REJECTED:
            return "RBF attempted but failed rules";
        case MempoolAcceptResult::TOO_MANY_ANCESTORS:
            return "Exceeds ancestor limit";
        case MempoolAcceptResult::TOO_MANY_DESCENDANTS:
            return "Exceeds descendant limit";
        case MempoolAcceptResult::MISSING_INPUTS:
            return "Parent transactions not found";
        case MempoolAcceptResult::SCRIPT_VERIFY_FAILED:
            return "Script validation failed";
        case MempoolAcceptResult::LOCKTIME_NOT_SATISFIED:
            return "Transaction not yet valid";

        // Phase C.2: Covenant-specific rejections
        case MempoolAcceptResult::COVENANT_ANCESTOR_MISSING:
            return "Covenant parent transaction not confirmed or in mempool";
        case MempoolAcceptResult::COVENANT_RBF_FORBIDDEN:
            return "Replace-by-fee forbidden for covenant transactions (policy)";
        case MempoolAcceptResult::TOO_MANY_COVENANT_INPUTS:
            return "Transaction exceeds maximum covenant input limit (DoS protection)";

        default:
            return "Unknown error";
    }
}

// ============================================================================
// Mempool Construction
// ============================================================================

Mempool::Mempool(const MempoolConfig& config)
    : config_(config)
    , total_size_(0)
    , total_fees_(0)
{
}

Mempool::~Mempool() {
    clear();
}

// ============================================================================
// Transaction Acceptance
// ============================================================================

MempoolAcceptResult Mempool::acceptTransaction(
    const Transaction& tx,
    const consensus::ChainStateView& coins_view,
    uint32_t current_height,
    uint64_t current_time
) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get transaction ID
    TxId txid = tx.GetTxid();  // Phase M.4.3-C: Returns TxId directly

    // 1. Check if already in mempool
    if (contains(txid)) {
        return MempoolAcceptResult::ALREADY_IN_MEMPOOL;
    }

    // F.9.8: Check invalid transaction cache (DoS protection)
    // Short-circuit before expensive validation if we've seen this invalid tx before
    auto cached_rejection = invalid_tx_cache_.lookup(txid, current_time);
    if (cached_rejection.has_value()) {
        // Return cached rejection reason (avoids re-validation)
        return MempoolAcceptResult::INVALID_TX;
    }

    // 2. Validate transaction with consensus rules
    consensus::TxValidationContext ctx;
    ctx.block_height = current_height;
    ctx.median_time_past = current_time;
    ctx.check_sequence_locks = true;

    consensus::TxValidationResult validation_result;
    if (!validateTransaction(tx, coins_view, current_height, current_time, validation_result)) {
        // F.9.8: Add to invalid transaction cache (DoS protection)
        std::string rejection_reason;
        MempoolAcceptResult result;

        // Map validation failure to mempool result + cache reason
        if (validation_result == consensus::TxValidationResult::SCRIPT_VERIFY_FAILED) {
            rejection_reason = "Script verification failed";
            result = MempoolAcceptResult::SCRIPT_VERIFY_FAILED;
        } else if (validation_result == consensus::TxValidationResult::INPUT_NOT_FOUND) {
            rejection_reason = "Missing inputs";
            result = MempoolAcceptResult::MISSING_INPUTS;
        } else if (validation_result == consensus::TxValidationResult::SEQUENCE_LOCK_FAIL) {
            rejection_reason = "Locktime not satisfied";
            result = MempoolAcceptResult::LOCKTIME_NOT_SATISFIED;
        } else {
            rejection_reason = "Invalid transaction (consensus rules)";
            result = MempoolAcceptResult::INVALID_TX;
        }

        // Cache the rejection (24 hour TTL)
        invalid_tx_cache_.add(txid, rejection_reason, current_time);
        return result;
    }

    // Phase C.2: Covenant Ancestor Safety Rules (POLICY-ONLY)
    // ────────────────────────────────────────────────────────────────────────
    // Rule: If transaction spends covenant-locked UTXO, enforce stricter rules
    // Goal: Prevent covenant bypass via mempool games
    // ────────────────────────────────────────────────────────────────────────
    bool has_covenant_input = false;
    uint32_t covenant_input_count = 0;

    for (const auto& input : tx.vin) {
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        auto coin_result = coins_view.getCoin(outpoint);
        if (!coin_result.ok()) {
            continue;  // Will be caught by MISSING_INPUTS later
        }

        const auto& coin = coin_result.value();

        // Phase C.2: Detect covenant inputs by script analysis
        // We check if scriptPubKey contains covenant opcodes
        // This is policy heuristic - consensus still validates properly
        bool is_covenant_locked = detectCovenantScript(coin.scriptPubKey);

        if (is_covenant_locked) {
            has_covenant_input = true;
            covenant_input_count++;

            // Policy Rule 1: DoS protection - limit covenant inputs per tx
            if (covenant_input_count > config_.max_covenant_inputs_per_tx) {
                return MempoolAcceptResult::TOO_MANY_COVENANT_INPUTS;
            }

            // Policy Rule 2: Covenant parent must be confirmed or in mempool
            // No speculative covenant satisfaction allowed
            TxId parent_txid = input.prevout.txid;  // Phase M: TxId identity

            // Check if parent is in mempool (unconfirmed)
            bool parent_in_mempool = contains(parent_txid);

            // Check if parent is confirmed (would be found via ChainStateView)
            // If coin exists in ChainStateView, it's either:
            // - Confirmed (in a block)
            // - In mempool (if mempool overlay is used)
            // For now, we require: either in mempool OR confirmed (coin exists)
            // More strict: require confirmed only (no chained covenant spends)
            if (!parent_in_mempool && coin.height == 0) {
                // Parent not in mempool and not confirmed
                // This is POLICY - prevents chained covenant satisfaction
                return MempoolAcceptResult::COVENANT_ANCESTOR_MISSING;
            }
        }
    }

    // 3. Calculate fee and fee rate
    uint64_t total_in = 0;
    uint64_t total_out = 0;

    for (const auto& input : tx.vin) {
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        auto coin_result = coins_view.getCoin(outpoint);
        if (!coin_result.ok()) {
            return MempoolAcceptResult::MISSING_INPUTS;
        }
        total_in += coin_result.value().value.GetUna();  // Policy math: convert AmountUna
    }

    for (const auto& output : tx.vout) {
        total_out += output.value.GetUna();  // Policy math: convert AmountUna
    }

    if (total_out > total_in) {
        return MempoolAcceptResult::INVALID_TX;
    }

    uint64_t fee = total_in - total_out;
    size_t vsize = calculateVirtualSize(tx);
    double fee_rate = static_cast<double>(fee) / static_cast<double>(vsize);

    // 4. Check minimum fee rate
    if (fee_rate < config_.min_fee_rate) {
        return MempoolAcceptResult::INSUFFICIENT_FEE;
    }

    // 5. Check for conflicts (double spends)
    std::vector<TxId> conflicts;  // Phase M: TxId identity
    if (!checkConflicts(tx, conflicts)) {
        // Conflict detected - check if RBF is possible
        if (!config_.enable_rbf || !signalsRBF(tx)) {
            return MempoolAcceptResult::CONFLICTS_WITH_MEMPOOL;
        }

        // Check RBF rules
        if (!checkRBFRules(tx, conflicts, fee)) {
            return MempoolAcceptResult::RBF_REJECTED;
        }

        // Remove conflicting transactions
        for (const auto& conflict_txid : conflicts) {
            removeTransaction(conflict_txid, true);
        }
    }

    // 6. Create mempool entry
    auto entry = std::make_shared<MempoolEntry>();
    entry->tx = tx;
    entry->txid = txid;
    entry->fee = fee;
    entry->base_fee = fee;
    entry->size = tx.Serialize().size();
    entry->vsize = vsize;
    entry->fee_rate = fee_rate;
    entry->time_added = current_time;
    entry->height = current_height;
    entry->signals_rbf = signalsRBF(tx);

    // Initialize ancestor/descendant tracking
    entry->ancestor_fee = fee;
    entry->ancestor_size = vsize;
    entry->ancestor_count = 0;
    entry->descendant_fee = fee;
    entry->descendant_size = vsize;
    entry->descendant_count = 0;

    // Phase C.2: Store covenant metadata (computed earlier)
    entry->has_covenant_input = has_covenant_input;
    entry->covenant_count = covenant_input_count;

    // 7. Build dependency graph
    for (const auto& input : tx.vin) {
        const TxId& parent_txid = input.prevout.txid;  // Phase M: TxId identity

        // Check if parent is in mempool
        if (contains(parent_txid)) {
            entry->parents.insert(parent_txid);

            // Add this transaction as a child to parent
            auto parent_entry = entries_[parent_txid];
            parent_entry->children.insert(txid);
        }
    }

    // 8. F.9.6: Compute ancestor set (BOUNDED BFS - done ONCE at admission)
    // This is the ONLY traversal - never repeated during removal/mining/queries
    std::unordered_set<TxId> ancestors_set;  // Phase M.4.3-C: Type-safe (cannot be WTxId)
    std::deque<TxId> q;  // Phase M.4.3-C: Type-safe

    // Start with immediate parents
    for (const auto& parent_txid : entry->parents) {
        q.push_back(parent_txid);
    }

    // BFS to discover ALL ancestors (grandparents, great-grandparents, etc.)
    while (!q.empty()) {
        TxId cur = q.front();  // Phase M.4.3-C: Cannot be WTxId by construction
        q.pop_front();

        // Try to insert (returns false if already visited)
        if (ancestors_set.insert(cur).second) {
            // Enforce ancestor limit (25 in Bitcoin Core)
            if (ancestors_set.size() > config_.max_ancestors) {
                return MempoolAcceptResult::TOO_MANY_ANCESTORS;
            }

            // Add cur's parents to queue
            auto cur_entry = entries_[cur];
            for (const auto& p : cur_entry->parents) {
                q.push_back(p);
            }
        }
    }

    // 9. Store ancestor set on entry (KEY to O(A) removal)
    // Convert set to sorted vector for determinism
    entry->ancestors.assign(ancestors_set.begin(), ancestors_set.end());
    std::sort(entry->ancestors.begin(), entry->ancestors.end());

    // 10. Compute aggregates ONCE (no re-traversal later)
    entry->ancestor_count = entry->ancestors.size();
    entry->ancestor_size = vsize;   // Start with self
    entry->ancestor_fee = fee;      // Start with self

    for (const auto& a : entry->ancestors) {
        auto a_entry = entries_[a];
        entry->ancestor_size += a_entry->vsize;
        entry->ancestor_fee += a_entry->fee;
    }

    // 11. Check ancestor size limit (101 KB in Bitcoin Core)
    size_t max_ancestor_size = config_.max_ancestor_size_kb * 1024;
    if (entry->ancestor_size > max_ancestor_size) {
        return MempoolAcceptResult::TOO_MANY_ANCESTORS;
    }

    // 12. Incremental update: Update ALL ancestors' descendant stats
    // Use cached ancestor vector - no graph traversal
    // Descendant count excludes self (Bitcoin Core convention)
    for (const auto& a : entry->ancestors) {
        auto a_entry = entries_[a];
        a_entry->descendant_count += 1;
        a_entry->descendant_size += vsize;
        a_entry->descendant_fee += fee;
    }

    // 12. Check mempool size limit
    size_t max_size_bytes = config_.max_size_mb * 1024 * 1024;
    if (total_size_ + vsize > max_size_bytes) {
        // Try to evict low-fee transactions
        evictTransactions();

        // Check again
        if (total_size_ + vsize > max_size_bytes) {
            return MempoolAcceptResult::MEMPOOL_FULL;
        }
    }

    // 13. Add to mempool
    entries_[txid] = entry;
    addToIndexes(txid, *entry);

    // Track spent outpoints
    for (const auto& input : tx.vin) {
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        spent_outpoints_[outpoint] = txid;
    }

    // Update totals
    total_size_ += vsize;
    total_fees_ += fee;

    // Done - descendant stats already updated in step 11
    return MempoolAcceptResult::OK;
}

MempoolAcceptResult Mempool::submitTransaction(
    const Transaction& tx,
    const consensus::ChainStateView& coins_view,
    uint32_t current_height,
    uint64_t current_time,
    MempoolSubmitMode mode
) {
    // TEST_ONLY mode: Skip script/signature verification for policy testing
    // This enables mempool policy testing without Phase 34 (transaction signing)
    //
    // CRITICAL: ALL consensus rules are enforced (coinbase maturity, value checks, etc.)
    // ONLY script execution and signature verification are skipped
    //
    // Enforces:
    // - Coinbase maturity (100 confirmations)
    // - Duplicate input detection
    // - Output value validation
    // - Input/output value balance
    // - Sequence locks (BIP 68)
    // - UTXO existence
    // - Fee rules (policy)
    // - Conflict detection (policy)
    // - Ancestor/descendant limits (policy)
    //
    // Skips:
    // - Script execution (P2PKH, P2SH, SegWit, Taproot)
    // - Signature verification (ECDSA, Schnorr)

    if (mode == MempoolSubmitMode::TEST_ONLY) {
        std::lock_guard<std::mutex> lock(mutex_);

        TxId txid = tx.GetTxid();  // Phase M.4.3-C: Returns TxId directly

        // 1. Check if already in mempool
        if (contains(txid)) {
            return MempoolAcceptResult::ALREADY_IN_MEMPOOL;
        }

        // F.9.8: Check invalid transaction cache (DoS protection)
        auto cached_rejection = invalid_tx_cache_.lookup(txid, current_time);
        if (cached_rejection.has_value()) {
            return MempoolAcceptResult::INVALID_TX;
        }

        // 2. Validate transaction with consensus rules (but skip script verification)
        //    CRITICAL: Consensus rules MUST be enforced even in TEST_ONLY mode
        //    This includes: coinbase maturity, duplicate inputs, value checks, etc.
        //    Only script execution and signature verification are skipped.

        // Cast to CoinsViewCache for validation (safe - read-only operation)
        auto& mutable_view = const_cast<consensus::ChainStateView&>(coins_view);
        auto& cache_view = static_cast<consensus::CoinsViewCache&>(mutable_view);

        // Phase P: Add mempool outputs to cache so child transactions can see parent outputs
        // (Fixes bug where TEST_ONLY mode couldn't find mempool parents)
        for (const auto& [txid_key, entry] : entries_) {
            for (size_t i = 0; i < entry->tx.vout.size(); i++) {
                OutPoint out(txid_key, static_cast<uint32_t>(i));
                consensus::UTXOEntry utxo;
                utxo.value = entry->tx.vout[i].value;
                utxo.scriptPubKey = entry->tx.vout[i].scriptPubKey;
                utxo.height = entry->height;
                utxo.isCoinbase = false;  // Mempool transactions are never coinbase
                cache_view.addCoin(out, utxo);
            }
        }

        // Create validation context with script verification disabled
        consensus::TxValidationContext ctx;
        ctx.block_height = current_height;
        ctx.median_time_past = current_time;
        ctx.check_sequence_locks = true;
        ctx.skip_script_verification = true;  // TEST_ONLY: Skip expensive script execution

        // Call consensus validation
        auto validation_output = consensus::validateTransaction(tx, cache_view, ctx, false);
        consensus::TxValidationResult validation_result = validation_output.result;

        if (validation_result != consensus::TxValidationResult::OK) {
            // F.9.8: Add to invalid transaction cache (DoS protection)
            std::string rejection_reason;
            MempoolAcceptResult result;

            // Map consensus validation failures to mempool results
            if (validation_result == consensus::TxValidationResult::COINBASE_MATURITY_VIOLATION) {
                rejection_reason = "Immature coinbase spend (< 100 confirmations)";
                result = MempoolAcceptResult::INVALID_TX;
            } else if (validation_result == consensus::TxValidationResult::INPUT_NOT_FOUND) {
                rejection_reason = "Missing inputs";
                result = MempoolAcceptResult::MISSING_INPUTS;
            } else if (validation_result == consensus::TxValidationResult::SEQUENCE_LOCK_FAIL) {
                rejection_reason = "Locktime not satisfied";
                result = MempoolAcceptResult::LOCKTIME_NOT_SATISFIED;
            } else if (validation_result == consensus::TxValidationResult::SCRIPT_VERIFY_FAILED) {
                // Script verification should not fail in TEST_ONLY mode since we skip it
                // But if it does (bug), treat as invalid
                rejection_reason = "Script verification failed (unexpected in TEST_ONLY mode)";
                result = MempoolAcceptResult::SCRIPT_VERIFY_FAILED;
            } else {
                rejection_reason = "Invalid transaction (consensus rules)";
                result = MempoolAcceptResult::INVALID_TX;
            }

            // Cache the rejection (24 hour TTL)
            invalid_tx_cache_.add(txid, rejection_reason, current_time);
            return result;
        }

        // 3. Calculate fee and fee rate (still required for policy)
        //    Use CoinsViewMempool to see outputs from parent transactions in mempool
        CoinsViewMemPool mempool_view(&coins_view);

        // Add outputs from existing mempool transactions
        for (const auto& [txid, entry] : entries_) {
            for (size_t i = 0; i < entry->tx.vout.size(); i++) {
                OutPoint out(txid, static_cast<uint32_t>(i));
                consensus::UTXOEntry utxo;
                utxo.value = entry->tx.vout[i].value;
                utxo.scriptPubKey = entry->tx.vout[i].scriptPubKey;
                utxo.height = entry->height;
                utxo.isCoinbase = false;  // Mempool transactions are never coinbase
                mempool_view.addCoin(out, utxo);
            }
        }

        uint64_t total_in = 0;
        uint64_t total_out = 0;

        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            auto coin_result = mempool_view.getCoin(outpoint);
            if (!coin_result.ok()) {
                return MempoolAcceptResult::MISSING_INPUTS;
            }
            total_in += coin_result.value().value.GetUna();  // Policy math: convert AmountUna
        }

        for (const auto& output : tx.vout) {
            total_out += output.value.GetUna();  // Policy math: convert AmountUna
        }

        if (total_out > total_in) {
            return MempoolAcceptResult::INVALID_TX;
        }

        uint64_t fee = total_in - total_out;
        size_t vsize = calculateVirtualSize(tx);
        double fee_rate = static_cast<double>(fee) / static_cast<double>(vsize);

        // 4. Check minimum fee rate
        if (fee_rate < config_.min_fee_rate) {
            return MempoolAcceptResult::INSUFFICIENT_FEE;
        }

        // 5. Check for conflicts (double spends)
        std::vector<TxId> conflicts;  // Phase M: TxId identity
        if (!checkConflicts(tx, conflicts)) {
            if (!config_.enable_rbf || !signalsRBF(tx)) {
                return MempoolAcceptResult::CONFLICTS_WITH_MEMPOOL;
            }

            if (!checkRBFRules(tx, conflicts, fee)) {
                return MempoolAcceptResult::RBF_REJECTED;
            }

            for (const auto& conflict_txid : conflicts) {
                removeTransaction(conflict_txid, true);
            }
        }

        // 6. Create mempool entry
        auto entry = std::make_shared<MempoolEntry>();
        entry->tx = tx;
        entry->txid = txid;
        entry->fee = fee;
        entry->base_fee = fee;
        entry->size = tx.Serialize().size();
        entry->vsize = vsize;
        entry->fee_rate = fee_rate;
        entry->time_added = current_time;
        entry->height = current_height;
        entry->signals_rbf = signalsRBF(tx);

        entry->ancestor_fee = fee;
        entry->ancestor_size = vsize;
        entry->ancestor_count = 0;
        entry->descendant_fee = fee;
        entry->descendant_size = vsize;
        entry->descendant_count = 0;

        // 7. Build dependency graph
        for (const auto& input : tx.vin) {
            const TxId& parent_txid = input.prevout.txid;  // Phase M: TxId identity

            if (contains(parent_txid)) {
                entry->parents.insert(parent_txid);

                auto parent_entry = entries_[parent_txid];
                parent_entry->children.insert(txid);
            }
        }

        // 8. F.9.6: Compute ancestor set (BOUNDED BFS - done ONCE)
        std::unordered_set<TxId> ancestors_set;  // Phase M: TxId identity
        std::deque<TxId> q;  // Phase M: TxId identity

        for (const auto& parent_txid : entry->parents) {
            q.push_back(parent_txid);
        }

        while (!q.empty()) {
            TxId cur = q.front();  // Phase M: TxId identity
            q.pop_front();

            if (ancestors_set.insert(cur).second) {
                if (ancestors_set.size() > config_.max_ancestors) {
                    return MempoolAcceptResult::TOO_MANY_ANCESTORS;
                }

                auto cur_entry = entries_[cur];
                for (const auto& p : cur_entry->parents) {
                    q.push_back(p);
                }
            }
        }

        // 9. Store ancestor set (KEY to O(A) removal)
        entry->ancestors.assign(ancestors_set.begin(), ancestors_set.end());
        std::sort(entry->ancestors.begin(), entry->ancestors.end());

        // 10. Compute aggregates ONCE
        entry->ancestor_count = entry->ancestors.size();
        entry->ancestor_size = vsize;
        entry->ancestor_fee = fee;

        for (const auto& a : entry->ancestors) {
            auto a_entry = entries_[a];
            entry->ancestor_size += a_entry->vsize;
            entry->ancestor_fee += a_entry->fee;
        }

        // 11. Check ancestor size limit
        size_t max_ancestor_size = config_.max_ancestor_size_kb * 1024;
        if (entry->ancestor_size > max_ancestor_size) {
            return MempoolAcceptResult::TOO_MANY_ANCESTORS;
        }

        // 12. Incremental update: Update ALL ancestors' descendant stats
        // Descendant count excludes self (Bitcoin Core convention)
        for (const auto& a : entry->ancestors) {
            auto a_entry = entries_[a];
            a_entry->descendant_count += 1;
            a_entry->descendant_size += vsize;
            a_entry->descendant_fee += fee;
        }

        // 12. Check mempool size limit
        size_t max_size_bytes = config_.max_size_mb * 1024 * 1024;
        if (total_size_ + vsize > max_size_bytes) {
            evictTransactions();

            if (total_size_ + vsize > max_size_bytes) {
                return MempoolAcceptResult::MEMPOOL_FULL;
            }
        }

        // 13. Add to mempool
        entries_[txid] = entry;
        addToIndexes(txid, *entry);

        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            spent_outpoints_[outpoint] = txid;
        }

        total_size_ += vsize;
        total_fees_ += fee;

        // Done - descendant stats already updated in step 11
        return MempoolAcceptResult::OK;
    }

    // NORMAL mode: Full validation
    return acceptTransaction(tx, coins_view, current_height, current_time);
}

// Phase M.1: Removed submitTransactionTestOnly(ChainDB*) - violates ChainStateView abstraction
// Migration pattern for call sites:
//   OLD: mempool.submitTransactionTestOnly(tx, chain_db, height, time);
//   NEW: CoinsViewCache view(chain_db);
//        mempool.submitTransaction(tx, view, height, time, MempoolSubmitMode::TEST_ONLY);

bool Mempool::removeTransaction(const TxId& txid, bool recursive) {  // Phase M.4.3-C: Type-safe
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(txid);
    if (it == entries_.end()) {
        return false;
    }

    auto entry = it->second;

    // Recursively remove descendants if requested
    if (recursive) {
        std::vector<TxId> children(entry->children.begin(), entry->children.end());  // Phase M.4.3-C: Type-safe
        for (const auto& child_txid : children) {
            removeTransaction(child_txid, true);
        }
    }

    // F.9.6: O(A) removal using cached ancestor set
    // NO BFS - use the ancestor vector computed once at admission
    // This is the critical fix that prevents O(N²) removal
    for (const auto& a : entry->ancestors) {
        auto a_it = entries_.find(a);
        if (a_it != entries_.end()) {
            // Decrement descendant stats (descendant count excludes self)
            a_it->second->descendant_count -= 1;
            a_it->second->descendant_size -= entry->vsize;
            a_it->second->descendant_fee -= entry->fee;
        }
    }

    // Remove from parent's children sets
    for (const auto& parent_txid : entry->parents) {
        auto parent_it = entries_.find(parent_txid);
        if (parent_it != entries_.end()) {
            parent_it->second->children.erase(txid);
        }
    }

    // Remove spent outpoints
    for (const auto& input : entry->tx.vin) {
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        spent_outpoints_.erase(outpoint);
    }

    // Remove from indexes
    removeFromIndexes(txid);

    // Update totals
    total_size_ -= entry->vsize;
    total_fees_ -= entry->fee;

    // Remove from entries
    entries_.erase(it);

    return true;
}

void Mempool::removeForBlock(const std::vector<Transaction>& block_txs) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (const auto& tx : block_txs) {
        TxId txid = tx.GetTxid();  // Phase M.4.3-C: Returns TxId directly
        removeTransaction(txid, false);
    }
}

// ============================================================================
// Queries
// ============================================================================

bool Mempool::contains(const TxId& txid) const {  // Phase M.4.3-C: Type-safe
    return entries_.find(txid) != entries_.end();
}

std::shared_ptr<const MempoolEntry> Mempool::getEntry(const TxId& txid) const {  // Phase M.4.3-C: Type-safe
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = entries_.find(txid);
    if (it == entries_.end()) {
        return nullptr;
    }

    return it->second;
}

std::vector<std::shared_ptr<const MempoolEntry>> Mempool::getAllEntries() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<const MempoolEntry>> result;
    result.reserve(entries_.size());

    for (const auto& pair : entries_) {
        result.push_back(pair.second);
    }

    return result;
}

std::vector<std::shared_ptr<const MempoolEntry>> Mempool::getEntriesByFeeRate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<const MempoolEntry>> result;
    result.reserve(entries_.size());

    for (const auto& pair : entries_) {
        result.push_back(pair.second);
    }

    // Sort by fee rate (descending)
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) {
            return a->fee_rate > b->fee_rate;
        }
    );

    return result;
}

std::vector<std::shared_ptr<const MempoolEntry>> Mempool::getEntriesByAncestorScore() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<const MempoolEntry>> result;
    result.reserve(entries_.size());

    for (const auto& pair : entries_) {
        result.push_back(pair.second);
    }

    // Sort by ancestor score (ancestor_fee / ancestor_size, descending)
    std::sort(result.begin(), result.end(),
        [](const auto& a, const auto& b) {
            double score_a = static_cast<double>(a->ancestor_fee) / static_cast<double>(a->ancestor_size);
            double score_b = static_cast<double>(b->ancestor_fee) / static_cast<double>(b->ancestor_size);
            return score_a > score_b;
        }
    );

    return result;
}

// Phase 34: Block assembly - Get transactions for block inclusion
std::vector<std::shared_ptr<const MempoolEntry>> Mempool::GetTransactionsForBlock(
    size_t max_weight
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<const MempoolEntry>> result;

    // Empty mempool → empty block (coinbase only)
    if (entries_.empty()) {
        return result;
    }

    // Get all entries sorted by ancestor score (fee/size, descending)
    // This automatically handles topological ordering:
    // - Parents have higher ancestor scores than children
    // - Sorting by ancestor score ensures parents come first
    std::vector<std::shared_ptr<const MempoolEntry>> candidates = getEntriesByAncestorScore();

    // Track accumulated weight (coinbase weight will be added by block assembler)
    // Reserve space for coinbase: ~400 WU typical
    size_t accumulated_weight = 0;
    const size_t COINBASE_WEIGHT_ESTIMATE = 400;
    size_t remaining_weight = max_weight - COINBASE_WEIGHT_ESTIMATE;

    // Select transactions up to weight limit
    for (const auto& entry : candidates) {
        // Calculate transaction weight
        size_t tx_weight = entry->tx.GetWeight();

        // Check if transaction fits
        if (accumulated_weight + tx_weight > remaining_weight) {
            // Transaction doesn't fit, skip it
            // (Bitcoin Core behavior: try next transaction)
            continue;
        }

        // Add transaction to block
        result.push_back(entry);
        accumulated_weight += tx_weight;
    }

    return result;
}

size_t Mempool::getSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_size_;
}

size_t Mempool::getCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

uint64_t Mempool::getTotalFees() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_fees_;
}

// ============================================================================
// Fee Estimation
// ============================================================================

double Mempool::estimateFeeRate(size_t target_blocks) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (entries_.empty()) {
        return config_.min_fee_rate;
    }

    // Simple fee estimation: use median fee rate of top transactions
    // that would fit in the next N blocks

    // Assume 1 MB blocks, 10 minute intervals
    size_t target_size = target_blocks * 1024 * 1024;

    // Get entries sorted by fee rate
    std::vector<double> fee_rates;
    fee_rates.reserve(entries_.size());

    size_t cumulative_size = 0;
    for (const auto& pair : by_fee_rate_) {
        fee_rates.push_back(pair.first);

        auto entry = entries_.at(pair.second);
        cumulative_size += entry->vsize;

        if (cumulative_size >= target_size) {
            break;
        }
    }

    if (fee_rates.empty()) {
        return config_.min_fee_rate;
    }

    // Return median
    std::sort(fee_rates.begin(), fee_rates.end());
    size_t median_idx = fee_rates.size() / 2;
    return fee_rates[median_idx];
}

// ============================================================================
// Maintenance
// ============================================================================

Mempool::EvictionStats Mempool::evictTransactions() {
    // F.9.7: Mempool expiry enforcement
    // Called periodically (every 60 seconds) by daemon maintenance task
    //
    // Eviction strategy:
    // 1. Remove expired transactions (> config_.expiry_hours = 336h = 14 days)
    // 2. Remove lowest fee rate transactions if over size limit
    // 3. Recursive removal (descendants removed automatically via removeTransaction)

    EvictionStats stats;
    stats.expired_count = 0;
    stats.expired_fees = 0;
    stats.size_evicted_count = 0;
    stats.size_evicted_fees = 0;

    uint64_t current_time = static_cast<uint64_t>(std::time(nullptr));
    uint64_t expiry_time = config_.expiry_hours * 3600;

    // Phase 1: Remove expired transactions
    std::vector<std::pair<TxId, uint64_t>> expired;  // Phase M: TxId, fee
    for (const auto& pair : entries_) {
        if (current_time - pair.second->time_added > expiry_time) {
            expired.push_back({pair.first, pair.second->fee});
        }
    }

    for (const auto& [txid, fee] : expired) {
        bool removed = removeTransaction(txid, true);  // Recursive removal
        if (removed) {
            stats.expired_count++;
            stats.expired_fees += fee;
        }
    }

    // Phase 2: If still over limit, evict low-fee transactions
    size_t max_size_bytes = config_.max_size_mb * 1024 * 1024;
    size_t target_size = max_size_bytes * 9 / 10;  // Evict to 90% capacity

    while (total_size_ > target_size && !by_fee_rate_.empty()) {
        // Remove lowest fee rate transaction
        auto lowest = by_fee_rate_.begin();
        TxId txid = lowest->second;  // Phase M: TxId identity

        auto it = entries_.find(txid);
        if (it != entries_.end()) {
            uint64_t fee = it->second->fee;
            bool removed = removeTransaction(txid, true);
            if (removed) {
                stats.size_evicted_count++;
                stats.size_evicted_fees += fee;
            }
        }
    }

    return stats;
}

void Mempool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    entries_.clear();
    by_fee_rate_.clear();
    by_time_.clear();
    spent_outpoints_.clear();
    total_size_ = 0;
    total_fees_ = 0;
}

Mempool::Stats Mempool::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.count = entries_.size();
    stats.size_bytes = total_size_;
    stats.total_fees = total_fees_;
    stats.min_fee_rate = config_.min_fee_rate;
    stats.max_fee_rate = 0.0;
    stats.median_fee_rate = 0.0;

    if (entries_.empty()) {
        return stats;
    }

    // Collect fee rates
    std::vector<double> fee_rates;
    fee_rates.reserve(entries_.size());

    for (const auto& pair : entries_) {
        fee_rates.push_back(pair.second->fee_rate);
    }

    std::sort(fee_rates.begin(), fee_rates.end());

    stats.min_fee_rate = fee_rates.front();
    stats.max_fee_rate = fee_rates.back();
    stats.median_fee_rate = fee_rates[fee_rates.size() / 2];

    return stats;
}

// Phase E.2.a: Explicit memory accounting for DoS hardening
Mempool::MemoryStats Mempool::getMemoryStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    MemoryStats mem;
    mem.tx_count = entries_.size();
    mem.total_bytes = total_size_;
    mem.max_bytes = config_.max_size_mb * 1024 * 1024;
    mem.available_bytes = mem.max_bytes > mem.total_bytes ?
                          mem.max_bytes - mem.total_bytes : 0;
    mem.usage_percent = mem.max_bytes > 0 ?
                        (static_cast<double>(mem.total_bytes) / mem.max_bytes) * 100.0 : 0.0;

    // Per-transaction size stats
    mem.largest_tx_bytes = 0;
    mem.smallest_tx_bytes = SIZE_MAX;
    mem.avg_tx_bytes = mem.tx_count > 0 ? mem.total_bytes / mem.tx_count : 0;

    for (const auto& pair : entries_) {
        size_t tx_bytes = pair.second->vsize;
        if (tx_bytes > mem.largest_tx_bytes) {
            mem.largest_tx_bytes = tx_bytes;
        }
        if (tx_bytes < mem.smallest_tx_bytes) {
            mem.smallest_tx_bytes = tx_bytes;
        }
    }

    if (mem.tx_count == 0) {
        mem.smallest_tx_bytes = 0;
    }

    // Memory breakdown (estimates)
    // MempoolEntry overhead: ~200 bytes per entry (conservative estimate)
    // Index overhead: ~64 bytes per entry (two std::set entries)
    constexpr size_t ENTRY_OVERHEAD = 200;
    constexpr size_t INDEX_OVERHEAD = 64;

    mem.tx_data_bytes = mem.total_bytes;
    mem.metadata_bytes = mem.tx_count * ENTRY_OVERHEAD;
    mem.index_bytes = mem.tx_count * INDEX_OVERHEAD;

    return mem;
}

// ============================================================================
// Helper Functions
// ============================================================================

bool Mempool::validateTransaction(
    const Transaction& tx,
    const consensus::ChainStateView& coins_view,
    uint32_t current_height,
    uint64_t current_time,
    consensus::TxValidationResult& result
) {
    consensus::TxValidationContext ctx;
    ctx.block_height = current_height;
    ctx.median_time_past = current_time;
    ctx.check_sequence_locks = true;

    // Phase M.1: validateTransaction() requires CoinsViewCache& (mutable), but coins_view is const
    // Safe cast: validateTransaction() only reads UTXOs, doesn't mutate the view in validation mode
    // We know coins_view is actually a CoinsViewCache (enforced by submitTransaction signature)
    auto& mutable_view = const_cast<consensus::ChainStateView&>(coins_view);
    auto& cache_view = static_cast<consensus::CoinsViewCache&>(mutable_view);

    auto validation_output = consensus::validateTransaction(tx, cache_view, ctx, false);
    result = validation_output.result;

    return result == consensus::TxValidationResult::OK;
}

bool Mempool::checkConflicts(
    const Transaction& tx,
    std::vector<TxId>& conflicts  // Phase M: TxId identity
) const {
    conflicts.clear();

    for (const auto& input : tx.vin) {
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);

        auto it = spent_outpoints_.find(outpoint);
        if (it != spent_outpoints_.end()) {
            conflicts.push_back(it->second);
        }
    }

    return conflicts.empty();
}

bool Mempool::checkRBFRules(
    const Transaction& new_tx,
    const std::vector<TxId>& conflicts,  // Phase M: TxId identity
    uint64_t new_fee
) const {
    // BIP 125: Replace-By-Fee rules
    //
    // 1. All conflicting transactions must signal RBF
    // 2. New transaction must pay higher fee rate
    // 3. New transaction must pay for its own bandwidth (absolute fee increase)
    // 4. No new unconfirmed inputs

    uint64_t old_fees = 0;
    size_t old_size = 0;

    for (const auto& conflict_txid : conflicts) {
        auto it = entries_.find(conflict_txid);
        if (it == entries_.end()) {
            return false;
        }

        auto entry = it->second;

        // Rule 1: Must signal RBF
        if (!entry->signals_rbf) {
            return false;
        }

        old_fees += entry->fee;
        old_size += entry->vsize;
    }

    // Rule 2: Higher fee rate
    size_t new_vsize = calculateVirtualSize(new_tx);
    double old_fee_rate = static_cast<double>(old_fees) / static_cast<double>(old_size);
    double new_fee_rate = static_cast<double>(new_fee) / static_cast<double>(new_vsize);

    if (new_fee_rate <= old_fee_rate) {
        return false;
    }

    // Rule 3: Absolute fee increase (min 1 sat/vbyte increase)
    uint64_t min_fee_increase = old_size;
    if (new_fee < old_fees + min_fee_increase) {
        return false;
    }

    return true;
}

// F.9.6: Debug/testing helper - NEVER call in production paths
//
// ⚠️ WARNING: This function does graph traversal (O(N) work).
// It exists ONLY for:
// - Testing (verify cached stats match ground truth)
// - Post-reorg full rebuild (cold path, explicit call)
//
// Bitcoin Core never calls this during normal operation.
// If you find yourself calling this in hot paths, you have a bug.
//
#ifdef DEBUG_MEMPOOL_STATS
void Mempool::updateAncestorState(const uint256& txid) {  // Phase M.0: uint256 identity
    // Recalculate ancestor state from scratch (testing only)
    auto it = entries_.find(txid);
    if (it == entries_.end()) {
        return;
    }

    auto entry = it->second;

    // BFS to discover full ancestor set
    std::unordered_set<TxId> ancestors_set;  // Phase M: TxId identity
    std::deque<TxId> q;  // Phase M: TxId identity

    for (const auto& parent_txid : entry->parents) {
        q.push_back(parent_txid);
    }

    while (!q.empty()) {
        TxId cur = q.front();  // Phase M: TxId identity
        q.pop_front();

        if (ancestors_set.insert(cur).second) {
            auto cur_entry = entries_[cur];
            for (const auto& p : cur_entry->parents) {
                q.push_back(p);
            }
        }
    }

    // Recalculate from scratch
    entry->ancestors.assign(ancestors_set.begin(), ancestors_set.end());
    std::sort(entry->ancestors.begin(), entry->ancestors.end());
    entry->ancestor_count = entry->ancestors.size();
    entry->ancestor_size = entry->vsize;
    entry->ancestor_fee = entry->fee;

    for (const auto& a : entry->ancestors) {
        auto a_entry = entries_[a];
        entry->ancestor_size += a_entry->vsize;
        entry->ancestor_fee += a_entry->fee;
    }
}
#endif  // DEBUG_MEMPOOL_STATS

void Mempool::addToIndexes(const TxId& txid, const MempoolEntry& entry) {  // Phase M.0: uint256 identity
    by_fee_rate_.insert({entry.fee_rate, txid});
    by_time_.insert({entry.time_added, txid});
}

void Mempool::removeFromIndexes(const TxId& txid) {  // Phase M.0: uint256 identity
    auto it = entries_.find(txid);
    if (it == entries_.end()) {
        return;
    }

    auto entry = it->second;
    by_fee_rate_.erase({entry->fee_rate, txid});
    by_time_.erase({entry->time_added, txid});
}

size_t Mempool::calculateVirtualSize(const Transaction& tx) const {
    return tx.GetVirtualSize();
}

bool Mempool::signalsRBF(const Transaction& tx) const {
    // BIP 125: A transaction signals RBF if any input has nSequence < 0xfffffffe

    for (const auto& input : tx.vin) {
        if (input.sequence < 0xfffffffe) {
            return true;
        }
    }

    return false;
}

// Phase C.2: Covenant Detection Helper (POLICY HEURISTIC)
// ────────────────────────────────────────────────────────────────────────
// Detects if a scriptPubKey contains covenant opcodes
//
// This is a POLICY heuristic for early detection, NOT consensus validation.
// Consensus validation happens in script interpreter during block validation.
//
// Goal: Identify covenant-locked outputs so mempool can apply stricter policy
// ────────────────────────────────────────────────────────────────────────
bool Mempool::detectCovenantScript(const std::vector<uint8_t>& scriptPubKey) const {
    // Phase C.2: Simple heuristic - check for covenant opcode bytes
    // Note: This is conservative - may have false positives (safe for policy)

    // Covenant opcodes (canonical values from consensus/script.h):
    // OP_CHECKTEMPLATEVERIFY = 0xb3 (was OP_NOP4)
    // OP_CHECKSIGFROMSTACK = 0xbb
    // OP_CHECKSIGFROMSTACKVERIFY = 0xbc
    // OP_TXHASH = 0xbd
    // OP_CHECKCONTRACTVERIFY = 0xbe

    const uint8_t OP_CHECKTEMPLATEVERIFY = 0xb3;
    const uint8_t OP_CHECKSIGFROMSTACK = 0xbb;
    const uint8_t OP_CHECKSIGFROMSTACKVERIFY = 0xbc;
    const uint8_t OP_TXHASH = 0xbd;
    const uint8_t OP_CHECKCONTRACTVERIFY = 0xbe;

    for (const auto& byte : scriptPubKey) {
        if (byte == OP_CHECKTEMPLATEVERIFY ||
            byte == OP_CHECKSIGFROMSTACK ||
            byte == OP_CHECKSIGFROMSTACKVERIFY ||
            byte == OP_TXHASH ||
            byte == OP_CHECKCONTRACTVERIFY) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// Reorg Reconciliation
// ============================================================================

size_t Mempool::ReconcileAfterReorg(
    const std::vector<Transaction>& disconnected_txs,
    const std::vector<Transaction>& connected_txs,
    const consensus::ChainStateView& coins_view,
    uint32_t current_height,
    uint64_t current_time
) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Build set of txids in connected blocks (to filter out)
    std::unordered_set<TxId> connected_txids;
    for (const auto& tx : connected_txs) {
        connected_txids.insert(tx.GetTxid());
    }

    // Also filter out coinbase transactions (can't go back to mempool)
    // and transactions already in mempool
    size_t restored_count = 0;
    size_t skipped_connected = 0;
    size_t skipped_coinbase = 0;
    size_t skipped_already_in_mempool = 0;
    size_t validation_failed = 0;

    for (const auto& tx : disconnected_txs) {
        TxId txid = tx.GetTxid();

        // Skip coinbase transactions
        if (tx.IsCoinbase()) {
            skipped_coinbase++;
            continue;
        }

        // Skip if transaction is in the new connected chain
        if (connected_txids.count(txid) > 0) {
            skipped_connected++;
            continue;
        }

        // Skip if already in mempool
        if (entries_.count(txid) > 0) {
            skipped_already_in_mempool++;
            continue;
        }

        // Re-validate and add to mempool
        // Must release lock during validation (may call back into mempool)
        mutex_.unlock();
        auto result = acceptTransaction(tx, coins_view, current_height, current_time);
        mutex_.lock();

        if (result == MempoolAcceptResult::OK) {
            restored_count++;
        } else {
            validation_failed++;
            // Log failures for debugging (transactions may have become invalid)
            // This is expected for:
            // - Transactions spending UTXOs that no longer exist
            // - Transactions with locktime not yet satisfied
            // - Transactions that conflict with connected block txs
        }
    }

    // Log reconciliation stats
    if (restored_count > 0 || validation_failed > 0) {
        std::cerr << "[Mempool] Reorg reconciliation complete:" << std::endl;
        std::cerr << "  Restored to mempool: " << restored_count << std::endl;
        std::cerr << "  Skipped (in new chain): " << skipped_connected << std::endl;
        std::cerr << "  Skipped (coinbase): " << skipped_coinbase << std::endl;
        std::cerr << "  Skipped (already in mempool): " << skipped_already_in_mempool << std::endl;
        std::cerr << "  Validation failed: " << validation_failed << std::endl;
    }

    return restored_count;
}

} // namespace mempool
} // namespace dinero
