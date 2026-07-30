#include "consensus/parallel_block_validator.h"
#include "consensus/covenants.h"
#include "consensus/transaction_validator.h"
#include "consensus/interfaces/iconsensus_utxo_set.h"  // Phase 2: Direct IConsensusUTXOSet
#include "consensus/outpoint.h"  // OutPoint type
#include <algorithm>
#include "consensus/tx_parser.h"
#include "consensus/block_index.h"      // F.7.1: For FindBlockIndex, CBlockIndex
#include "consensus/block_lifecycle.h"  // Phase M.0: For BLOCK_HAVE_UNDO
#include "consensus/script_cache.h"     // F.8.3: Script execution cache
#include "consensus/signature_cache.h"  // F.8.4: Signature cache
#include "consensus/crypto/sighash_bip143.h"  // Sighash computation (consensus)
#include "dinero/core/consensus/commitment.h"   // F.7.1: For uint256
#include "common/status.h"              // F.7.1: For Status, StatusToString
#include "consensus/chainparams.h"               // GetActiveChain()
#include "primitives/transaction.h"
#include <iostream>
#include <chrono>
#include <secp256k1.h>                  // F.8.5: ECDSA verification

namespace dinero {
namespace consensus {

using namespace std::chrono;

// ========== Config Factory Methods ==========

ParallelBlockValidator::Config ParallelBlockValidator::Config::forIBD() {
    Config config;
    config.enable_parallel = true;
    config.parallel_threshold = 5;  // Lower threshold for IBD (parallelize more)
    config.enable_script_cache = true;
    config.worker_threads = 0; // Auto-detect (use all cores)
    return config;
}

ParallelBlockValidator::Config ParallelBlockValidator::Config::forNormalOperation() {
    Config config;
    config.enable_parallel = true;
    config.parallel_threshold = 10; // Higher threshold for mainnet
    config.enable_script_cache = true;
    config.worker_threads = 0; // Auto-detect
    return config;
}

ParallelBlockValidator::Config ParallelBlockValidator::Config::forLowResource() {
    Config config;
    config.enable_parallel = false; // Disable parallelization
    config.parallel_threshold = 1000; // Effectively disable
    config.enable_script_cache = false;
    config.worker_threads = 1;
    return config;
}

// ========== Metrics ==========

void ParallelBlockValidator::Metrics::reset() {
    blocks_validated.store(0);
    blocks_applied.store(0);
    parallel_validations.store(0);
    serial_validations.store(0);
    total_validation_time_us.store(0);
    total_apply_time_us.store(0);
}

std::string ParallelBlockValidator::Metrics::toString() const {
    std::ostringstream oss;
    oss << "ParallelBlockValidator::Metrics {\n";
    oss << "  Blocks validated: " << blocks_validated.load() << "\n";
    oss << "  Blocks applied:   " << blocks_applied.load() << "\n";
    oss << "  Parallel validations: " << parallel_validations.load() << "\n";
    oss << "  Serial validations:   " << serial_validations.load() << "\n";

    uint64_t total_val = total_validation_time_us.load();
    uint64_t total_app = total_apply_time_us.load();
    uint64_t validated = blocks_validated.load();

    if (validated > 0) {
        oss << "  Avg validation time: " << (total_val / validated / 1000.0) << " ms\n";
        oss << "  Avg apply time:      " << (total_app / validated / 1000.0) << " ms\n";
    }

    oss << "}";
    return oss.str();
}

// ========== Constructor / Destructor ==========

// Phase 2: Constructor takes IConsensusUTXOSet* directly (no adapter chain)
ParallelBlockValidator::ParallelBlockValidator(
    IConsensusUTXOSet* consensus_utxo_set,
    ChainstateGuard* chainstate_guard,
    BlockStorage* block_storage,
    const Config& config)
    : config_(config)
    , consensus_utxo_set_(consensus_utxo_set)
    , chainstate_guard_(chainstate_guard)
    , block_storage_(block_storage)
{
    if (!consensus_utxo_set_) {
        throw std::runtime_error("ParallelBlockValidator: ConsensusUTXOSet cannot be null");
    }

    if (!chainstate_guard_) {
        throw std::runtime_error("ParallelBlockValidator: Chainstate guard cannot be null");
    }

    // Phase 2: Create block validator directly with IConsensusUTXOSet (no adapter)
    block_validator_ = std::make_unique<BlockValidator>(consensus_utxo_set_);

    // Create worker pool if parallel validation enabled
    if (config_.enable_parallel) {
        ValidationWorkerPool::Config worker_config;

        if (config_.worker_threads > 0) {
            worker_config.num_workers = config_.worker_threads;
        } else {
            // Auto-detect based on validation config
            if (config_.parallel_threshold <= 5) {
                worker_config = ValidationWorkerPool::Config::forIBD();
            } else {
                worker_config = ValidationWorkerPool::Config::forNormalOperation();
            }
        }

        worker_pool_ = std::make_unique<ValidationWorkerPool>(worker_config);
        worker_pool_->start();

        std::cout << "[ParallelBlockValidator] Started with " << worker_pool_->getWorkerCount()
                  << " workers (threshold: " << config_.parallel_threshold << " tx)\n";
    }
}

ParallelBlockValidator::~ParallelBlockValidator() {
    if (worker_pool_) {
        worker_pool_->stop();
    }
}

// ========== Validation ==========

bool ParallelBlockValidator::validateBlock(const Block& block, uint64_t height, std::string& error) {
    auto start_time = high_resolution_clock::now();

    // Acquire READ lock (shared, allows concurrent validation)
    auto read_lock = chainstate_guard_->readLock();

    // ═════════════════════════════════════════════════════════════════════════
    // V5 Freeze Fork: three consensus gates applied uniformly to both the
    // parallel and serial dispatch paths. Spec: docs/consensus/V5_FREEZE_FORK_SPEC.md
    //
    // TransactionValidator::CheckStructure already rejects tx.version > 2
    // (transaction_validator.cpp:62), so Gate 2 is inherent on the serial path.
    // We still emit an explicit freeze-fork error message here so the failure
    // reason matches across all entry points.
    // ═════════════════════════════════════════════════════════════════════════
    // v7: freeze-fork gates removed along with ring/CT stack.

    bool valid = false;

    // Decide: parallel or serial validation?
    if (config_.enable_parallel && block.vtx.size() >= config_.parallel_threshold) {
        valid = validateBlockParallel(block, height, error);
        metrics_.parallel_validations.fetch_add(1);
    } else {
        valid = validateBlockSerial(block, height, error);
        metrics_.serial_validations.fetch_add(1);
    }

    auto end_time = high_resolution_clock::now();
    auto duration_us = duration_cast<microseconds>(end_time - start_time).count();
    metrics_.total_validation_time_us.fetch_add(duration_us);

    if (valid) {
        metrics_.blocks_validated.fetch_add(1);
    }

    return valid;
}

bool ParallelBlockValidator::validateBlockParallel(const Block& block, uint64_t height, std::string& error) {
    // Skip coinbase (index 0)
    if (block.vtx.size() <= 1) {
        return true; // Only coinbase
    }

    // BIP113: Compute Median Time Past for locktime validation
    // MTP is computed from the PREVIOUS block (the one we're connecting to)
    uint64_t median_time_past = 0;
    uint256 prev_hash = block.header.prev_block_hash;
    if (!prev_hash.IsNull()) {
        // Phase M.0: prevBlockHash is already uint256
        CBlockIndex* pprev = FindBlockIndex(prev_hash);
        if (pprev) {
            median_time_past = pprev->GetMedianTimePast();
        }
    }

    // F.8.5: Per-INPUT parallelization (Bitcoin Core approach)
    // Step 1: Extract UTXO data for all inputs (single-threaded, before parallelization)
    // This ensures:
    //   - No shared mutable state in workers
    //   - Thread-safe execution
    //   - Deterministic failure ordering

    struct ScriptCheck {
        size_t tx_index;
        size_t input_index;
        const Transaction* tx;       // Pointer to block-owned tx (immutable, safe)
        std::vector<uint8_t> scriptPubKey;  // From UTXO
        uint64_t value;              // From UTXO
        uint32_t script_flags;       // Consensus flags
        uint32_t height;             // Block height (for height-based CLTV)
        uint64_t median_time_past;   // BIP113: For time-based CLTV
        // BIP341: Full UTXO context for Taproot sighash (all inputs' amounts and scripts)
        std::vector<uint64_t> all_input_amounts;
        std::vector<std::vector<uint8_t>> all_input_scriptpubkeys;
        std::vector<uint8_t> all_input_confidential_flags;
        std::vector<std::vector<uint8_t>> all_input_commitments;
        std::shared_ptr<const PrecomputedTransactionData>
            covenant_precomputed;
    };

    std::vector<ScriptCheck> script_checks;
    script_checks.reserve(block.vtx.size() * 2);  // Estimate

    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        const Transaction& tx = block.vtx[tx_idx];

        // BIP341: First, collect ALL UTXO data for this transaction
        // Required for correct Taproot sighash computation
        std::vector<uint64_t> tx_all_amounts;
        std::vector<std::vector<uint8_t>> tx_all_scriptpubkeys;
        std::vector<uint8_t> tx_all_confidential_flags;
        std::vector<std::vector<uint8_t>> tx_all_input_commitments;
        std::vector<UTXOEntry> tx_input_utxos;
        tx_all_amounts.reserve(tx.vin.size());
        tx_all_scriptpubkeys.reserve(tx.vin.size());
        tx_all_confidential_flags.reserve(tx.vin.size());
        tx_all_input_commitments.reserve(tx.vin.size());
        tx_input_utxos.reserve(tx.vin.size());

        // First pass: collect all UTXO data for this transaction
        for (size_t input_idx = 0; input_idx < tx.vin.size(); ++input_idx) {
            const auto& input = tx.vin[input_idx];

            // Get UTXO for this input via adapter (consensus interface)
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            auto utxo_opt = consensus_utxo_set_->GetUTXO(outpoint);
            if (!utxo_opt.has_value()) {
                // Phase M.0: Convert uint256 to hex for logging
                error = "Tx " + std::to_string(tx_idx) + " input " + std::to_string(input_idx) +
                       ": UTXO not found " + input.prevout.txid.AsUint256().GetHex().substr(0, 16) + ":" +
                       std::to_string(input.prevout.vout);
                return false;
            }

            const UTXOEntry& utxo = utxo_opt.value();
            tx_all_amounts.push_back(utxo.value.GetUna());
            tx_all_scriptpubkeys.push_back(utxo.scriptPubKey);
            tx_all_confidential_flags.push_back(utxo.is_confidential ? 1 : 0);
            tx_all_input_commitments.push_back(utxo.commitment);
            tx_input_utxos.push_back(utxo);
        }

        const auto covenant_precomputed =
            std::make_shared<const PrecomputedTransactionData>(
                tx, tx_input_utxos);

        // Second pass: create ScriptCheck for each input with full UTXO context
        for (size_t input_idx = 0; input_idx < tx.vin.size(); ++input_idx) {
            // Create ScriptCheck with all required data (no shared mutable state)
            ScriptCheck check;
            check.tx_index = tx_idx;
            check.input_index = input_idx;
            check.tx = &tx;              // Pointer to block-owned tx (safe - block lives through validation)
            check.scriptPubKey = tx_all_scriptpubkeys[input_idx];  // This input's scriptPubKey
            check.value = tx_all_amounts[input_idx];               // This input's value
            check.script_flags = 0;         // No special consensus flags yet
            check.height = static_cast<uint32_t>(height);  // Block height for height-based CLTV
            check.median_time_past = median_time_past;      // BIP113: For time-based CLTV
            // BIP341: Full UTXO context for Taproot sighash
            check.all_input_amounts = tx_all_amounts;
            check.all_input_scriptpubkeys = tx_all_scriptpubkeys;
            check.all_input_confidential_flags = tx_all_confidential_flags;
            check.all_input_commitments = tx_all_input_commitments;
            check.covenant_precomputed = covenant_precomputed;

            script_checks.push_back(std::move(check));
        }
    }

    // Step 2: Create parallel tasks for each input (thread-safe)
    std::vector<ValidationTask> tasks;
    tasks.reserve(script_checks.size());

    for (auto& check : script_checks) {
        ValidationTask task;
        task.type = ValidationTask::Type::VERIFY_SCRIPT;
        task.tx_index = check.tx_index;
        task.input_index = check.input_index;

        // Capture ALL data by value (no shared mutable state)
        // tx pointer is safe because block lives through validation
        task.custom_func = [check](std::string& err) -> bool {
            // ====================================================================
            // F.8.5: Per-input verification using canonical VerifyInput engine
            // ====================================================================
            // Integrates with:
            //   - F.8.3: Script cache (transaction-level caching)
            //   - F.8.4: Signature cache (crypto-level caching)
            // No shared mutable state access
            // Thread-safe execution
            // ====================================================================

            auto result = TransactionValidator::VerifyInput(
                *check.tx,
                check.input_index,
                check.scriptPubKey,
                check.value,
                check.script_flags,
                check.height,
                check.median_time_past,
                check.all_input_amounts,        // BIP341: Full UTXO context
                check.all_input_scriptpubkeys,  // BIP341: Full UTXO context
                check.all_input_confidential_flags,
                check.all_input_commitments,
                check.covenant_precomputed.get()
            );

            if (!result.valid) {
                err = result.error;
                return false;
            }

            return true;
        };

        tasks.push_back(std::move(task));
    }

    // Step 3: Submit all tasks to worker pool and track failures
    auto futures = worker_pool_->submitBatch(std::move(tasks));

    // ========================================================================
    // F.8.5: Deterministic Failure Ordering (MANDATORY for consensus)
    // ========================================================================
    struct ValidationFailure {
        size_t tx_index;
        size_t input_index;
        std::string error_msg;
    };

    std::vector<ValidationFailure> failures;

    // Wait for all tasks to complete and collect failures
    for (size_t i = 0; i < futures.size(); ++i) {
        try {
            bool result = futures[i].get();
            if (!result) {
                // Task failed - record failure with indices from script_checks
                ValidationFailure failure;
                failure.tx_index = script_checks[i].tx_index;
                failure.input_index = script_checks[i].input_index;
                failure.error_msg = "Validation failed for tx " +
                                   std::to_string(failure.tx_index) +
                                   " input " + std::to_string(failure.input_index);
                failures.push_back(failure);
            }
        } catch (const std::exception& e) {
            // Exception during validation - record failure
            ValidationFailure failure;
            failure.tx_index = script_checks[i].tx_index;
            failure.input_index = script_checks[i].input_index;
            failure.error_msg = std::string("Validation exception: ") + e.what();
            failures.push_back(failure);
        }
    }

    // If any failures occurred, sort them deterministically and report the first
    if (!failures.empty()) {
        // Sort by (tx_index, input_index) - MANDATORY for consensus determinism
        std::sort(failures.begin(), failures.end(),
            [](const ValidationFailure& a, const ValidationFailure& b) {
                if (a.tx_index != b.tx_index) {
                    return a.tx_index < b.tx_index;
                }
                return a.input_index < b.input_index;
            });

        // Report the first failure (deterministic)
        error = failures[0].error_msg;
        return false;
    }

    return true;
}

bool ParallelBlockValidator::validateBlockSerial(const Block& block, uint64_t height, std::string& error) {
    // Skip coinbase (index 0)
    if (block.vtx.size() <= 1) {
        return true;
    }

    // Serial validation using TransactionValidator
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const Transaction& tx = block.vtx[i];

        auto result = TransactionValidator::ValidateTransaction(tx, consensus_utxo_set_, height);
        if (!result.valid) {
            error = "Tx " + std::to_string(i) + " validation failed: " + result.error;
            return false;
        }
    }

    return true;
}

// ========== Script-Only Validation (Two-Phase) ==========

bool ParallelBlockValidator::validateScriptsOnly(const Block& block, std::string& error) {
    auto start_time = high_resolution_clock::now();

    // Skip coinbase (index 0) - no inputs to verify
    if (block.vtx.size() <= 1) {
        return true;
    }

    bool valid = false;

    // Decide: parallel or serial validation?
    if (config_.enable_parallel && block.vtx.size() >= config_.parallel_threshold) {
        valid = validateScriptsOnlyParallel(block, error);
        metrics_.parallel_validations.fetch_add(1);
    } else {
        valid = validateScriptsOnlySerial(block, error);
        metrics_.serial_validations.fetch_add(1);
    }

    auto end_time = high_resolution_clock::now();
    auto duration_us = duration_cast<microseconds>(end_time - start_time).count();
    metrics_.total_validation_time_us.fetch_add(duration_us);

    if (valid) {
        metrics_.blocks_validated.fetch_add(1);
    }

    return valid;
}

bool ParallelBlockValidator::validateScriptsOnlyParallel(const Block& block, std::string& error) {
    // Create script verification tasks (no UTXO lookups)
    std::vector<ValidationTask> tasks;
    tasks.reserve(block.vtx.size() * 5);

    for (size_t tx_idx = 1; tx_idx < block.vtx.size(); ++tx_idx) {
        const Transaction& tx = block.vtx[tx_idx];

        // Basic structure validation
        if (tx.vin.empty()) {
            error = "Tx " + std::to_string(tx_idx) + " has no inputs";
            return false;
        }
        if (tx.vout.empty()) {
            error = "Tx " + std::to_string(tx_idx) + " has no outputs";
            return false;
        }

        // F.8.5: Add script verification task
        // Note: Script verification requires UTXO data (for scriptPubKey and value)
        // so this method assumes UTXO set is available
        ValidationTask task;
        task.type = ValidationTask::Type::VERIFY_SCRIPT;
        task.tx_index = tx_idx;
        task.tx = tx;

        task.custom_func = [this, tx, tx_idx](std::string& err) -> bool {
            // Verify signatures using TransactionValidator (with caching)
            auto result = TransactionValidator::ValidateTransaction(tx, consensus_utxo_set_, 0);
            if (!result.valid) {
                err = "Tx " + std::to_string(tx_idx) + " script verification failed: " + result.error;
                return false;
            }
            return true;
        };

        tasks.push_back(std::move(task));
    }

    // If no tasks were created (shouldn't happen if block has transactions)
    if (tasks.empty()) {
        return true;
    }

    // Submit all tasks to worker pool
    auto futures = worker_pool_->submitBatch(std::move(tasks));

    // Wait for all tasks to complete
    for (auto& future : futures) {
        try {
            bool result = future.get();
            if (!result) {
                error = "Parallel script verification failed";
                return false;
            }
        } catch (const std::exception& e) {
            error = std::string("Script verification exception: ") + e.what();
            return false;
        }
    }

    return true;
}

bool ParallelBlockValidator::validateScriptsOnlySerial(const Block& block, std::string& error) {
    // Skip coinbase (index 0)
    for (size_t i = 1; i < block.vtx.size(); ++i) {
        const Transaction& tx = block.vtx[i];

        // Basic structure validation
        if (tx.vin.empty()) {
            error = "Tx " + std::to_string(i) + " has no inputs";
            return false;
        }
        if (tx.vout.empty()) {
            error = "Tx " + std::to_string(i) + " has no outputs";
            return false;
        }

        // ========================================================================
        // CRITICAL: Script verification (same logic as parallel, but sequential)
        // ========================================================================
        // Previously this was a stub that only checked structure - consensus-breaking!
        // Now we verify signatures using TransactionValidator (with caching)
        // ========================================================================
        auto result = TransactionValidator::ValidateTransaction(tx, consensus_utxo_set_, 0);
        if (!result.valid) {
            error = "Tx " + std::to_string(i) + " script verification failed: " + result.error;
            return false;
        }
    }

    return true;
}

// ========== Block Connection ==========

bool ParallelBlockValidator::connectBlock(const Block& block, uint64_t height, BlockUndo& undo, std::string& error) {
    auto start_time = high_resolution_clock::now();

    // Acquire WRITE lock (exclusive)
    auto write_lock = chainstate_guard_->writeLock();

    // Use underlying BlockValidator to connect (Phase 6B: Re-enabled)
    // Need to compute block hash for undo tracking
    // Phase M.1: ConnectBlock now takes uint256 directly
    bool connected = block_validator_->ConnectBlock(block, static_cast<uint32_t>(height), block.GetHash(), undo, error);

    // ========================================================================
    // F.7.1: Persist undo data after successful connection
    // ========================================================================
    if (connected && block_storage_) {
        // Serialize undo data
        std::vector<uint8_t> undo_bytes = undo.Serialize();

        // Write to rev*.dat file
        // Phase M.0: block.GetHash() already returns uint256
        uint256 block_hash_u256 = block.GetHash();
        auto undo_pos_result = block_storage_->writeUndo(block_hash_u256, undo_bytes);

        if (undo_pos_result.status() != Status::Ok) {
            error = "Failed to write undo data: " + std::string(StatusToString(undo_pos_result.status()));
            return false;
        }

        // Phase P.2: Update CBlockIndex with undo disk positions
        // Phase M.0: Use block_hash_u256 computed above
        CBlockIndex* pindex = FindBlockIndex(block_hash_u256);
        if (pindex) {
            FilePosition undo_file_pos = undo_pos_result.value();
            pindex->undo_file = undo_file_pos.file_number;
            pindex->undo_pos = undo_file_pos.offset;
            pindex->undo_size = undo_file_pos.size;
            pindex->status |= BLOCK_HAVE_UNDO;
        } else {
            error = "Failed to find BlockIndex for undo persistence";
            return false;
        }
    }

    auto end_time = high_resolution_clock::now();
    auto duration_us = duration_cast<microseconds>(end_time - start_time).count();
    metrics_.total_apply_time_us.fetch_add(duration_us);

    if (connected) {
        metrics_.blocks_applied.fetch_add(1);
    }

    return connected;
}

// ========== Combined Validation + Connection ==========

bool ParallelBlockValidator::validateAndConnect(const Block& block, uint64_t height, BlockUndo& undo, std::string& error) {
    // Step 1: Validate (read-only, parallel)
    if (!validateBlock(block, height, error)) {
        return false;
    }

    // Step 2: Connect (write, exclusive)
    if (!connectBlock(block, height, undo, error)) {
        return false;
    }

    return true;
}

} // namespace consensus
} // namespace dinero
