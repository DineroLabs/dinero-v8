#include "consensus/validation_worker_pool.h"
#include "consensus/script_verify.h"
#include "consensus/script_validation.h"  // For ValidateP2WPKHSpend
#include "consensus/script_interpreter.h"  // For SignatureHashTaproot, CheckSchnorrSignature
#include "consensus/tx_parser.h"
#include "consensus/interfaces/iutxo_provider.h"  // v2.2.0: Consensus UTXO interface
#include "consensus/outpoint.h"                   // v2.2.0: Consensus OutPoint type
#include "consensus/crypto/sighash_bip143.h"  // Sighash computation (consensus)
#include "crypto/evp_secp256k1.h"  // DineroSecpIllegalCallback — non-aborting illegal_arg handler
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace dinero {
namespace consensus {

using namespace std::chrono;

// ========== Config Factory Methods ==========

ValidationWorkerPool::Config ValidationWorkerPool::Config::autoDetect() {
    Config config;
    unsigned int hw_threads = std::thread::hardware_concurrency();

    if (hw_threads == 0) hw_threads = 4; // Fallback

    // Use N-1 cores (leave one for main thread + network I/O)
    config.num_workers = std::max(1u, hw_threads - 1);
    config.max_queue_size = config.num_workers * 1000; // 1000 tasks per worker
    config.enable_work_stealing = true;
    config.enable_metrics = true;

    return config;
}

ValidationWorkerPool::Config ValidationWorkerPool::Config::forIBD() {
    Config config = autoDetect();

    // Use ALL cores for IBD (aggressive parallelism)
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 4;
    config.num_workers = std::max(2u, hw_threads);

    // Larger queue for IBD (blocks arrive fast)
    config.max_queue_size = config.num_workers * 2000;
    config.enable_work_stealing = true; // Important for load balancing
    config.enable_metrics = true;

    return config;
}

ValidationWorkerPool::Config ValidationWorkerPool::Config::forNormalOperation() {
    Config config = autoDetect();

    // Conservative: Use half of cores for normal operation
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) hw_threads = 4;
    config.num_workers = std::max(1u, hw_threads / 2);

    config.max_queue_size = config.num_workers * 500;
    config.enable_work_stealing = false; // Less contention
    config.enable_metrics = true;

    return config;
}

ValidationWorkerPool::Config ValidationWorkerPool::Config::forLowResource() {
    Config config;
    config.num_workers = 1; // Single validation thread
    config.max_queue_size = 100;
    config.enable_work_stealing = false;
    config.enable_metrics = false; // Reduce overhead

    return config;
}

// ========== Metrics ==========

void ValidationWorkerPool::Metrics::reset() {
    tasks_completed.store(0);
    tasks_failed.store(0);
    total_validation_time_us.store(0);
    script_verifications.store(0);
    utxo_checks.store(0);
}

std::string ValidationWorkerPool::Metrics::toString() const {
    std::ostringstream oss;
    oss << "ValidationWorkerPool::Metrics {\n";
    oss << "  Tasks completed: " << tasks_completed.load() << "\n";
    oss << "  Tasks failed: " << tasks_failed.load() << "\n";
    oss << "  Script verifications: " << script_verifications.load() << "\n";
    oss << "  UTXO checks: " << utxo_checks.load() << "\n";

    uint64_t total_time = total_validation_time_us.load();
    uint64_t completed = tasks_completed.load();
    if (completed > 0) {
        double avg_time_ms = (total_time / 1000.0) / completed;
        oss << "  Avg task time: " << avg_time_ms << " ms\n";
    }

    oss << "}";
    return oss.str();
}

// ========== Constructor / Destructor ==========

ValidationWorkerPool::ValidationWorkerPool(const Config& config)
    : config_(config)
{
    if (config_.num_workers == 0) {
        config_ = Config::autoDetect();
    }

    // Initialize per-worker queues if work-stealing enabled
    if (config_.enable_work_stealing) {
        worker_queues_.reserve(config_.num_workers);
        for (size_t i = 0; i < config_.num_workers; ++i) {
            worker_queues_.emplace_back(std::make_unique<WorkerQueue>());
        }
    }
}

ValidationWorkerPool::~ValidationWorkerPool() {
    stop();
}

// ========== Lifecycle ==========

void ValidationWorkerPool::start() {
    if (running_.load()) {
        return; // Already running
    }

    shutdown_.store(false);
    running_.store(true);

    workers_.reserve(config_.num_workers);
    for (size_t i = 0; i < config_.num_workers; ++i) {
        workers_.emplace_back(&ValidationWorkerPool::workerThreadFunc, this, i);
    }

    std::cout << "[ValidationWorkerPool] Started " << config_.num_workers << " worker threads\n";
}

void ValidationWorkerPool::stop() {
    if (!running_.load()) {
        return; // Not running
    }

    shutdown_.store(true);
    running_.store(false);

    // Wake up all workers
    queue_cv_.notify_all();

    // Wait for workers to finish
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    workers_.clear();

    // Clear remaining tasks
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!task_queue_.empty()) {
            task_queue_.pop();
        }
    }

    std::cout << "[ValidationWorkerPool] Stopped. Final metrics:\n" << metrics_.toString() << "\n";
}

// ========== Task Submission ==========

std::future<bool> ValidationWorkerPool::submitTask(ValidationTask&& task) {
    std::future<bool> result = task.result_promise.get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        if (task_queue_.size() >= config_.max_queue_size) {
            // Queue full - reject task
            std::promise<bool> failed_promise;
            failed_promise.set_value(false);
            return failed_promise.get_future();
        }

        task_queue_.push(std::move(task));
        pending_tasks_.fetch_add(1);
    }

    queue_cv_.notify_one();
    return result;
}

std::vector<std::future<bool>> ValidationWorkerPool::submitBatch(std::vector<ValidationTask>&& tasks) {
    std::vector<std::future<bool>> results;
    results.reserve(tasks.size());

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);

        for (auto& task : tasks) {
            results.push_back(task.result_promise.get_future());

            if (task_queue_.size() < config_.max_queue_size) {
                task_queue_.push(std::move(task));
                pending_tasks_.fetch_add(1);
            } else {
                // Queue full - reject remaining tasks
                break;
            }
        }
    }

    queue_cv_.notify_all(); // Wake all workers for batch
    return results;
}

// ========== High-Level Helpers ==========

std::future<bool> ValidationWorkerPool::verifyScript(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& prev_spk,
    uint64_t prev_value,
    const std::vector<uint64_t>& all_input_amounts,
    const std::vector<std::vector<uint8_t>>& all_input_scriptpubkeys,
    const std::vector<uint8_t>& all_input_confidential_flags,
    const std::vector<std::vector<uint8_t>>& all_input_commitments)
{
    ValidationTask task;
    task.type = ValidationTask::Type::VERIFY_SCRIPT;
    task.tx = tx;
    task.input_index = input_index;
    task.prev_scriptPubKey = prev_spk;
    task.prev_value = prev_value;
    task.all_input_amounts = all_input_amounts;
    task.all_input_scriptpubkeys = all_input_scriptpubkeys;
    task.all_input_confidential_flags = all_input_confidential_flags;
    task.all_input_commitments = all_input_commitments;

    return submitTask(std::move(task));
}

std::future<bool> ValidationWorkerPool::checkInputExists(
    const uint256& txid,
    uint32_t vout,
    IUTXOProvider* utxo_provider)
{
    ValidationTask task;
    task.type = ValidationTask::Type::CHECK_INPUT_EXISTS;
    task.prev_txid = txid;  // Phase M.0: uint256 identity
    task.prev_vout = vout;

    // v2.2.0: Use consensus IUTXOProvider interface (O(1) lookup, not O(n) scan)
    task.custom_func = [txid, vout, utxo_provider](std::string& error) -> bool {
        // Create consensus OutPoint and check via interface
        OutPoint outpoint(TxId(txid), vout);

        if (utxo_provider->HasUTXO(outpoint)) {
            return true;
        }

        error = "UTXO not found: " + txid.GetHex() + ":" + std::to_string(vout);
        return false;
    };

    return submitTask(std::move(task));
}

// ========== Worker Thread ==========

void ValidationWorkerPool::workerThreadFunc(size_t worker_id) {
    while (!shutdown_.load()) {
        ValidationTask task;

        // Try to get task from main queue
        if (!popTask(task)) {
            // Try work-stealing if enabled
            if (config_.enable_work_stealing && !tryStealWork(worker_id, task)) {
                // No work available, wait
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !task_queue_.empty() || shutdown_.load();
                });
                continue;
            } else if (!config_.enable_work_stealing) {
                // No work available and work-stealing disabled, wait
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !task_queue_.empty() || shutdown_.load();
                });
                continue;
            }
        }

        // Execute task
        auto start_time = high_resolution_clock::now();
        bool success = executeTask(task);
        auto end_time = high_resolution_clock::now();

        // Update metrics
        if (config_.enable_metrics) {
            auto duration_us = duration_cast<microseconds>(end_time - start_time).count();
            metrics_.total_validation_time_us.fetch_add(duration_us);

            if (success) {
                metrics_.tasks_completed.fetch_add(1);
            } else {
                metrics_.tasks_failed.fetch_add(1);
            }
        }

        // Set result
        task.result_promise.set_value(success);

        // Decrement pending count
        pending_tasks_.fetch_sub(1);
        completion_cv_.notify_all();
    }
}

// ========== Task Execution ==========

bool ValidationWorkerPool::executeTask(ValidationTask& task) {
    switch (task.type) {
        case ValidationTask::Type::VERIFY_SCRIPT:
            return executeScriptVerification(task);

        case ValidationTask::Type::CHECK_INPUT_EXISTS:
            return executeInputExistenceCheck(task);

        case ValidationTask::Type::CUSTOM:
            if (task.custom_func) {
                return task.custom_func(task.error_msg);
            }
            return false;

        default:
            task.error_msg = "Unknown task type";
            return false;
    }
}

bool ValidationWorkerPool::executeScriptVerification(ValidationTask& task) {
    if (config_.enable_metrics) {
        metrics_.script_verifications.fetch_add(1);
    }

    // Basic script verification for SegWit transactions
    // Full Bitcoin-style script interpreter can be added later

    // Validate input index
    if (task.input_index >= task.tx.vin.size()) {
        task.error_msg = "Invalid input index for script verification";
        return false;
    }

    const auto& input = task.tx.vin[task.input_index];

    // Skip verification for coinbase transactions (no inputs to verify)
    if (task.tx.IsCoinbase()) {
        return true;
    }

    // For SegWit transactions (witness_version >= 0), check witness data
    if (task.tx.witness_version == 0) {
        // P2WPKH or P2WSH - witness data should exist
        if (input.witness.empty()) {
            task.error_msg = "SegWit transaction missing witness data for input " +
                            std::to_string(task.input_index);
            return false;
        }

        // P2WPKH witness: [signature, pubkey] (2 items)
        // P2WSH witness: [signature(s), ..., script] (2+ items)
        if (input.witness.size() < 2) {
            task.error_msg = "Insufficient witness items for input " +
                            std::to_string(task.input_index);
            return false;
        }

        // Basic signature format check (DER encoding starts with 0x30)
        const auto& signature = input.witness[0];
        if (!signature.empty() && signature[0] != 0x30) {
            task.error_msg = "Invalid signature format for input " +
                            std::to_string(task.input_index);
            return false;
        }

        // Basic pubkey format check (compressed: 0x02/0x03, uncompressed: 0x04)
        const auto& pubkey = input.witness[1];
        if (pubkey.size() != 33 && pubkey.size() != 65) {
            task.error_msg = "Invalid pubkey size for input " +
                            std::to_string(task.input_index);
            return false;
        }
    }

    // For Taproot (witness_version == 1), check Schnorr signature
    if (task.tx.witness_version == 1) {
        if (input.witness.empty()) {
            task.error_msg = "Taproot transaction missing witness data for input " +
                            std::to_string(task.input_index);
            return false;
        }

        // Taproot key-spend: [64-byte Schnorr signature]
        // Taproot script-spend: [signature(s), script, control]
        const auto& witness_item = input.witness[0];
        if (witness_item.empty()) {
            task.error_msg = "Empty witness item for Taproot input " +
                            std::to_string(task.input_index);
            return false;
        }
    }

    // ========================================================================
    // CRYPTOGRAPHIC SIGNATURE VERIFICATION
    // ========================================================================

    // P2WPKH (SegWit v0, 22-byte scriptPubKey: OP_0 <20-byte-hash>)
    if (task.prev_scriptPubKey.size() == 22 &&
        task.prev_scriptPubKey[0] == 0x00 &&
        task.prev_scriptPubKey[1] == 0x14) {

        const auto& sig_with_hashtype = input.witness[0];
        const auto& pubkey = input.witness[1];

        if (sig_with_hashtype.size() < 9) {  // Minimum DER + sighash byte
            task.error_msg = "Signature too short for P2WPKH input " +
                            std::to_string(task.input_index);
            return false;
        }

        // Extract sighash type (last byte of signature)
        uint8_t sighash_type = sig_with_hashtype.back();
        std::vector<uint8_t> sig_der(sig_with_hashtype.begin(), sig_with_hashtype.end() - 1);

        // Build scriptCode for BIP143: OP_DUP OP_HASH160 <pubkey_hash> OP_EQUALVERIFY OP_CHECKSIG
        // pubkey_hash is bytes 2-22 of the scriptPubKey
        std::vector<uint8_t> script_code = {0x76, 0xa9, 0x14};  // OP_DUP OP_HASH160 PUSH20
        script_code.insert(script_code.end(),
                          task.prev_scriptPubKey.begin() + 2,
                          task.prev_scriptPubKey.end());
        script_code.push_back(0x88);  // OP_EQUALVERIFY
        script_code.push_back(0xac);  // OP_CHECKSIG

        // Compute BIP143 sighash (consensus layer)
        std::vector<uint8_t> sighash = SighashBIP143::ComputeSighash(
            task.tx, task.input_index, script_code, task.prev_value, sighash_type);

        if (sighash.size() != 32) {
            task.error_msg = "Failed to compute BIP143 sighash for input " +
                            std::to_string(task.input_index);
            return false;
        }

        // Verify ECDSA signature using secp256k1.
        // Register the non-aborting illegal_callback so adversarial /
        // malformed wire data hits a logged-rejection path instead of
        // aborting the daemon (the default libsecp256k1 callback calls
        // abort()/__fastfail). See evp_secp256k1.h for rationale.
        secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
        if (ctx) {
            secp256k1_context_set_illegal_callback(
                ctx, dinero::crypto::DineroSecpIllegalCallback, nullptr);
        }
        if (!ctx) {
            task.error_msg = "Failed to create secp256k1 context";
            return false;
        }

        // Parse public key
        secp256k1_pubkey pk;
        if (!secp256k1_ec_pubkey_parse(ctx, &pk, pubkey.data(), pubkey.size())) {
            secp256k1_context_destroy(ctx);
            task.error_msg = "Invalid public key for input " + std::to_string(task.input_index);
            return false;
        }

        // Parse DER signature
        secp256k1_ecdsa_signature sig_parsed;
        if (!secp256k1_ecdsa_signature_parse_der(ctx, &sig_parsed, sig_der.data(), sig_der.size())) {
            secp256k1_context_destroy(ctx);
            task.error_msg = "Invalid DER signature for input " + std::to_string(task.input_index);
            return false;
        }

        // Normalize signature (Bitcoin requires low-S)
        secp256k1_ecdsa_signature sig_normalized;
        secp256k1_ecdsa_signature_normalize(ctx, &sig_normalized, &sig_parsed);

        // Verify signature
        int verify_result = secp256k1_ecdsa_verify(ctx, &sig_normalized, sighash.data(), &pk);
        secp256k1_context_destroy(ctx);

        if (verify_result != 1) {
            task.error_msg = "ECDSA signature verification failed for input " +
                            std::to_string(task.input_index);
            return false;
        }

        return true;
    }

    // P2WSH (SegWit v0, 34-byte scriptPubKey: OP_0 <32-byte-hash>)
    // Full P2WSH verification requires script interpreter - structural check only for now
    if (task.prev_scriptPubKey.size() == 34 &&
        task.prev_scriptPubKey[0] == 0x00 &&
        task.prev_scriptPubKey[1] == 0x20) {
        // P2WSH structural validation passed above
        // Full script execution would be implemented with a complete script interpreter
        return true;
    }

    // ========================================================================
    // BIP341 TAPROOT KEY-PATH SPENDING (with BIP340 Schnorr Verification)
    // ========================================================================
    // P2TR (Taproot) outputs: OP_1 <32-byte x-only pubkey>
    // Key-path spending: Single 64-byte (or 65-byte with sighash) Schnorr sig
    // Script-path spending: Requires control block + script (not yet supported)
    // ========================================================================
    if (task.prev_scriptPubKey.size() == 34 &&
        task.prev_scriptPubKey[0] == 0x51 &&  // OP_1 (witness version 1)
        task.prev_scriptPubKey[1] == 0x20) {  // PUSH32

        // Extract x-only public key from scriptPubKey (bytes 2-33)
        std::vector<uint8_t> x_only_pubkey(
            task.prev_scriptPubKey.begin() + 2,
            task.prev_scriptPubKey.end()
        );

        // Key-path spend: witness has exactly 1 element (the signature)
        if (input.witness.size() == 1) {
            const auto& schnorr_sig = input.witness[0];

            // BIP340: Signature must be 64 bytes, or 65 bytes with sighash type
            if (schnorr_sig.size() != 64 && schnorr_sig.size() != 65) {
                task.error_msg = "Invalid Taproot key-path signature size (" +
                                std::to_string(schnorr_sig.size()) +
                                " bytes, expected 64 or 65) for input " +
                                std::to_string(task.input_index);
                return false;
            }

            // Extract sighash type (default = 0x00 = SIGHASH_DEFAULT)
            uint8_t sighash_type = 0x00;  // SIGHASH_DEFAULT
            if (schnorr_sig.size() == 65) {
                sighash_type = schnorr_sig[64];
                // BIP341: Valid sighash types are 0x00-0x03, 0x81-0x83
                // (SIGHASH_DEFAULT, ALL, NONE, SINGLE, and ANYONECANPAY variants)
                uint8_t base_type = sighash_type & 0x7F;
                if (base_type > 0x03) {
                    task.error_msg = "Invalid Taproot sighash type (0x" +
                                    std::to_string(sighash_type) +
                                    ") for input " + std::to_string(task.input_index);
                    return false;
                }
            }

            // Build execution context for BIP341 sighash computation
            // Need amounts and scriptPubKeys for ALL inputs (BIP341 requirement)
            std::vector<uint64_t> all_amounts;
            std::vector<std::vector<uint8_t>> all_scriptpubkeys;
            std::vector<uint8_t> all_confidential_flags;
            std::vector<std::vector<uint8_t>> all_input_commitments;

            if (!task.all_input_amounts.empty() &&
                task.all_input_amounts.size() == task.tx.vin.size() &&
                task.all_input_scriptpubkeys.size() == task.tx.vin.size() &&
                task.all_input_confidential_flags.size() == task.tx.vin.size() &&
                task.all_input_commitments.size() == task.tx.vin.size()) {
                all_amounts = task.all_input_amounts;
                all_scriptpubkeys = task.all_input_scriptpubkeys;
                all_confidential_flags = task.all_input_confidential_flags;
                all_input_commitments = task.all_input_commitments;
            } else {
                task.error_msg = "Taproot prevout context incomplete for input " +
                                std::to_string(task.input_index) +
                                " (need amounts, scripts, flags, and commitments for all prevouts)";
                return false;
            }

            // Create script execution context for Taproot sighash
            ScriptExecutionContext ctx(
                &task.tx,
                static_cast<uint32_t>(task.input_index),
                task.prev_value,
                SCRIPT_VERIFY_TAPROOT,
                all_amounts,
                all_scriptpubkeys,
                all_confidential_flags,
                all_input_commitments
            );

            // Compute BIP341 Taproot sighash (key-path: empty leaf_hash)
            std::vector<uint8_t> empty_leaf_hash;  // Key-path = no script
            std::vector<uint8_t> empty_annex;      // No annex
            std::vector<uint8_t> sighash = SignatureHashTaproot(
                ctx, sighash_type, empty_leaf_hash, empty_annex);

            if (sighash.size() != 32) {
                task.error_msg = "Failed to compute Taproot sighash for input " +
                                std::to_string(task.input_index);
                return false;
            }

            // BIP340 Schnorr signature verification
            if (!CheckSchnorrSignature(schnorr_sig, x_only_pubkey, sighash,
                                       SCRIPT_VERIFY_TAPROOT | SCRIPT_VERIFY_STRICTENC)) {
                task.error_msg = "Taproot key-path signature verification failed for input " +
                                std::to_string(task.input_index);
                return false;
            }

            // Signature valid
            return true;
        }

        // ====================================================================
        // SCRIPT-PATH SPENDING: Explicitly not supported (by design)
        // ====================================================================
        // Taproot script-path spends have witness.size() >= 2:
        //   - Last element: control block (33+ bytes)
        //   - Second-to-last: script being executed
        //   - Remaining: script arguments
        //
        // This is a POLICY decision, not a bug:
        //   - Key-path: ✅ Supported (BIP340 Schnorr verification)
        //   - Script-path: ❌ Not supported (requires tapscript interpreter)
        //
        // To enable script-path spending in the future:
        //   1. Implement BIP342 tapscript interpreter
        //   2. Parse control block and verify Merkle proof
        //   3. Execute script with tapscript rules
        // ====================================================================
        if (input.witness.size() >= 2) {
            task.error_msg = "POLICY: Taproot script-path spending is not supported "
                            "(key-path only). Input " + std::to_string(task.input_index) +
                            " has " + std::to_string(input.witness.size()) +
                            " witness elements (script-path requires tapscript interpreter)";
            return false;
        }

        // Empty witness for Taproot is invalid
        task.error_msg = "Empty witness for Taproot output at input " +
                        std::to_string(task.input_index);
        return false;
    }

    // Unknown/unsupported script type
    task.error_msg = "Unsupported script type for input " + std::to_string(task.input_index);
    return false;
}

bool ValidationWorkerPool::executeInputExistenceCheck(ValidationTask& task) {
    if (config_.enable_metrics) {
        metrics_.utxo_checks.fetch_add(1);
    }

    // This should already be handled by custom_func
    if (task.custom_func) {
        return task.custom_func(task.error_msg);
    }

    task.error_msg = "No custom function provided for INPUT_EXISTS check";
    return false;
}

// ========== Queue Management ==========

bool ValidationWorkerPool::popTask(ValidationTask& task) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    if (task_queue_.empty()) {
        return false;
    }

    task = std::move(task_queue_.front());
    task_queue_.pop();
    return true;
}

bool ValidationWorkerPool::tryStealWork(size_t worker_id, ValidationTask& task) {
    if (!config_.enable_work_stealing || worker_queues_.empty()) {
        return false;
    }

    // Try to steal from other workers (round-robin)
    for (size_t i = 1; i < worker_queues_.size(); ++i) {
        size_t target_id = (worker_id + i) % worker_queues_.size();
        auto& target_queue = worker_queues_[target_id];

        std::lock_guard<std::mutex> lock(target_queue->mutex);
        if (!target_queue->tasks.empty()) {
            task = std::move(target_queue->tasks.back());
            target_queue->tasks.pop_back();
            return true;
        }
    }

    return false;
}

size_t ValidationWorkerPool::getPendingTaskCount() const {
    return pending_tasks_.load();
}

void ValidationWorkerPool::waitForCompletion() {
    std::unique_lock<std::mutex> lock(completion_mutex_);
    completion_cv_.wait(lock, [this] {
        return pending_tasks_.load() == 0;
    });
}

} // namespace consensus
} // namespace dinero
