#include "consensus/validation_queue.h"
#include "consensus/block_validation.h"
#include "consensus/transaction_validator.h"
#include "consensus/interfaces/iconsensus_utxo_set.h"  // Phase 2: Direct IConsensusUTXOSet
#include "consensus/outpoint.h"                   // v2.2.0: Consensus OutPoint type
#include "consensus/tx_parser.h"
#include "consensus/chainstate_guard.h"
#include "consensus/chainparams.h"             // GetActiveChain
#include "primitives/transaction.h"
#include "vault/vault_runtime.h"               // Track C: vault block-event hook
#include <iostream>
#include <chrono>
#include <algorithm>

namespace dinero {
namespace consensus {

using namespace std::chrono;

// ========== Config Factory Methods ==========

ValidationQueue::Config ValidationQueue::Config::forIBD() {
    Config config;
    config.max_in_flight_blocks = 32;      // Aggressive pipelining
    config.max_queued_blocks = 256;        // Large buffer
    config.enable_pipelining = true;
    config.enable_priority = true;         // Height-based priority
    config.worker_pool_threads = 0;        // Auto-detect (use all cores)
    return config;
}

ValidationQueue::Config ValidationQueue::Config::forNormalOperation() {
    Config config;
    config.max_in_flight_blocks = 8;       // Moderate pipelining
    config.max_queued_blocks = 64;
    config.enable_pipelining = true;
    config.enable_priority = false;        // FIFO order
    config.worker_pool_threads = 0;        // Auto-detect
    return config;
}

// ========== Metrics ==========

void ValidationQueue::Metrics::reset() {
    blocks_submitted.store(0);
    blocks_validated.store(0);
    blocks_connected.store(0);
    blocks_failed.store(0);
    blocks_cancelled.store(0);
    total_validation_time_ms.store(0);
    total_apply_time_ms.store(0);
}

std::string ValidationQueue::Metrics::toString() const {
    std::ostringstream oss;
    oss << "ValidationQueue::Metrics {\n";
    oss << "  Submitted:   " << blocks_submitted.load() << "\n";
    oss << "  Validated:   " << blocks_validated.load() << "\n";
    oss << "  Connected:   " << blocks_connected.load() << "\n";
    oss << "  Failed:      " << blocks_failed.load() << "\n";
    oss << "  Cancelled:   " << blocks_cancelled.load() << "\n";

    uint64_t total_val = total_validation_time_ms.load();
    uint64_t total_app = total_apply_time_ms.load();
    uint64_t validated = blocks_validated.load();

    if (validated > 0) {
        oss << "  Avg validation time: " << (total_val / validated) << " ms\n";
        oss << "  Avg apply time:      " << (total_app / validated) << " ms\n";
    }

    oss << "}";
    return oss.str();
}

// ========== Constructor / Destructor ==========

// Phase 2: Constructor takes IConsensusUTXOSet* directly (no adapter chain)
ValidationQueue::ValidationQueue(IConsensusUTXOSet* consensus_utxo_set, ChainstateGuard* chainstate_guard, const Config& config)
    : config_(config)
    , consensus_utxo_set_(consensus_utxo_set)
    , chainstate_guard_(chainstate_guard)
    , pending_queue_(
        [](const std::shared_ptr<BlockValidationJob>& a, const std::shared_ptr<BlockValidationJob>& b) {
            // Priority: Lower height = higher priority (for IBD)
            return a->height > b->height;
        }
    )
{
    if (!consensus_utxo_set_) {
        throw std::runtime_error("ValidationQueue: Consensus UTXO set cannot be null");
    }

    if (!chainstate_guard_) {
        throw std::runtime_error("ValidationQueue: Chainstate guard cannot be null");
    }

    block_validator_ = std::make_unique<BlockValidator>(consensus_utxo_set_);

    // Create worker pool
    ValidationWorkerPool::Config worker_config;
    if (config_.worker_pool_threads > 0) {
        worker_config.num_workers = config_.worker_pool_threads;
    } else {
        // Auto-detect based on queue config
        if (config_.max_in_flight_blocks >= 32) {
            worker_config = ValidationWorkerPool::Config::forIBD();
        } else {
            worker_config = ValidationWorkerPool::Config::forNormalOperation();
        }
    }

    worker_pool_ = std::make_unique<ValidationWorkerPool>(worker_config);
}

ValidationQueue::~ValidationQueue() {
    stop();
}

// ========== Lifecycle ==========

void ValidationQueue::start() {
    if (running_.load()) {
        return; // Already running
    }

    shutdown_.store(false);
    running_.store(true);

    // Start worker pool
    worker_pool_->start();

    // Start validation and applier threads
    validation_thread_ = std::thread(&ValidationQueue::validationThreadFunc, this);
    applier_thread_ = std::thread(&ValidationQueue::applierThreadFunc, this);

    std::cout << "[ValidationQueue] Started (max in-flight: " << config_.max_in_flight_blocks
              << ", workers: " << worker_pool_->getWorkerCount() << ")\n";
}

void ValidationQueue::stop() {
    if (!running_.load()) {
        return; // Not running
    }

    shutdown_.store(true);
    running_.store(false);

    // Wake up threads
    pending_cv_.notify_all();
    validated_cv_.notify_all();

    // Wait for threads
    if (validation_thread_.joinable()) {
        validation_thread_.join();
    }
    if (applier_thread_.joinable()) {
        applier_thread_.join();
    }

    // Stop worker pool
    worker_pool_->stop();

    auto reject_outstanding = [this](const std::string& reason) {
        std::vector<std::shared_ptr<BlockValidationJob>> outstanding;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            while (!pending_queue_.empty()) {
                auto job = pending_queue_.top();
                pending_queue_.pop();
                outstanding.push_back(std::move(job));
            }
        }

        {
            std::lock_guard<std::mutex> lock(validated_mutex_);
            while (!validated_queue_.empty()) {
                outstanding.push_back(std::move(validated_queue_.front()));
                validated_queue_.pop_front();
            }
        }

        for (const auto& job : outstanding) {
            if (!job) {
                continue;
            }
            job->state = BlockValidationJob::State::CANCELLED;
            job->error_msg = reason;
            job->final_result = BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED, reason);
            metrics_.blocks_cancelled.fetch_add(1);
            completeJob(job);
        }
    };

    reject_outstanding("Validation queue stopped");

    std::cout << "[ValidationQueue] Stopped. Final metrics:\n" << metrics_.toString() << "\n";
}

// ========== Block Submission ==========

bool ValidationQueue::submit(const Block& block, uint64_t height, const uint256& prev_hash) {
    if (!running_.load()) {
        std::cerr << "[ValidationQueue] Not running, rejecting block " << height << "\n";
        return false;
    }

    auto job = std::make_shared<BlockValidationJob>(block, height, prev_hash);
    job->state = BlockValidationJob::State::QUEUED;
    return enqueueJob(job);
}

BlockAcceptResult ValidationQueue::submitAndWait(const Block& block, uint64_t height, const uint256& prev_hash) {
    if (!running_.load()) {
        return BlockAcceptResult::Rejected(
            BlockRejectCode::CONNECT_FAILED,
            "Validation queue is not running",
            block.GetHash(),
            height
        );
    }

    auto promise = std::make_shared<std::promise<BlockAcceptResult>>();
    auto future = promise->get_future();

    auto job = std::make_shared<BlockValidationJob>(block, height, prev_hash);
    job->state = BlockValidationJob::State::QUEUED;
    job->completion = promise;

    if (!enqueueJob(job)) {
        return BlockAcceptResult::Rejected(
            BlockRejectCode::CONNECT_FAILED,
            "Validation queue is full",
            block.GetHash(),
            height
        );
    }

    return future.get();
}

bool ValidationQueue::enqueueJob(const std::shared_ptr<BlockValidationJob>& job) {
    // Check queue capacity
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_queue_.size() >= config_.max_queued_blocks) {
            std::cerr << "[ValidationQueue] Queue full (" << pending_queue_.size()
                      << "), rejecting block " << job->height << "\n";
            return false;
        }

        pending_queue_.push(job);
        metrics_.blocks_submitted.fetch_add(1);
    }

    pending_cv_.notify_one();
    return true;
}

// ========== Validation Thread ==========

void ValidationQueue::validationThreadFunc() {
    while (!shutdown_.load()) {
        std::shared_ptr<BlockValidationJob> job = popNextJob();

        if (!job) {
            // No work, wait
            std::unique_lock<std::mutex> lock(pending_mutex_);
            pending_cv_.wait_for(lock, milliseconds(100), [this] {
                return !pending_queue_.empty() || shutdown_.load();
            });
            continue;
        }

        // Check dependencies
        if (!areDependenciesMet(*job)) {
            // Re-queue (dependencies not met)
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_queue_.push(job);
            std::this_thread::sleep_for(milliseconds(10)); // Back off
            continue;
        }

        // Check in-flight limit
        if (in_flight_count_.load() >= config_.max_in_flight_blocks) {
            // Too many in-flight, re-queue
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_queue_.push(job);
            std::this_thread::sleep_for(milliseconds(50));
            continue;
        }

        // Validate block in parallel
        job->state = BlockValidationJob::State::VALIDATING;
        job->validate_start = steady_clock::now();
        in_flight_count_.fetch_add(1);

        bool valid = validateBlock(*job);

        job->validate_end = steady_clock::now();
        auto validation_time_ms = duration_cast<milliseconds>(job->validate_end - job->validate_start).count();
        metrics_.total_validation_time_ms.fetch_add(validation_time_ms);

        if (!valid) {
            job->state = BlockValidationJob::State::FAILED;
            metrics_.blocks_failed.fetch_add(1);
            in_flight_count_.fetch_sub(1);
            job->final_result = BlockAcceptResult::Rejected(
                BlockRejectCode::INVALID_TRANSACTION,
                job->error_msg,
                job->block.GetHash(),
                job->height
            );
            completeJob(job);

            if (on_block_failed_) {
                on_block_failed_(job->block, job->error_msg);
            }

            std::cerr << "[ValidationQueue] Block " << job->height << " failed: "
                      << job->error_msg << "\n";
            continue;
        }

        // Validation succeeded
        job->state = BlockValidationJob::State::VALIDATED;
        metrics_.blocks_validated.fetch_add(1);
        in_flight_count_.fetch_sub(1);

        // Move to validated queue
        {
            std::lock_guard<std::mutex> lock(validated_mutex_);
            validated_queue_.push_back(job);
        }

        validated_cv_.notify_one();
    }
}

// ========== Applier Thread ==========

void ValidationQueue::applierThreadFunc() {
    while (!shutdown_.load()) {
        std::shared_ptr<BlockValidationJob> job = popNextValidatedJob();

        if (!job) {
            // No validated blocks, wait
            std::unique_lock<std::mutex> lock(validated_mutex_);
            validated_cv_.wait_for(lock, milliseconds(100), [this] {
                return !validated_queue_.empty() || shutdown_.load();
            });
            continue;
        }

        // Apply block to chainstate (single-threaded, exclusive access)
        job->state = BlockValidationJob::State::APPLYING;
        auto apply_start = steady_clock::now();

        bool applied = applyBlockToChain(*job);

        auto apply_end = steady_clock::now();
        job->apply_end = apply_end;

        auto apply_time_ms = duration_cast<milliseconds>(apply_end - apply_start).count();
        metrics_.total_apply_time_ms.fetch_add(apply_time_ms);

        if (!applied) {
            job->state = BlockValidationJob::State::FAILED;
            metrics_.blocks_failed.fetch_add(1);
            completeJob(job);

            if (on_block_failed_) {
                on_block_failed_(job->block, job->error_msg);
            }

            std::cerr << "[ValidationQueue] Block " << job->height << " apply failed: "
                      << job->error_msg << "\n";
            continue;
        }

        // Success
        job->state = BlockValidationJob::State::CONNECTED;
        metrics_.blocks_connected.fetch_add(1);
        total_processed_.fetch_add(1);

        // Mark block as connected for dependency tracking
        markBlockAsConnected(job->height, job->block.GetHash());
        completeJob(job);

        // Notify callback
        if (on_block_connected_) {
            on_block_connected_(job->block, job->height);
        }

        // Log progress
        if (job->height % 100 == 0 || job->height < 10) {
            auto total_time = duration_cast<milliseconds>(job->apply_end - job->submit_time).count();
            std::cout << "[ValidationQueue] Block " << job->height << " connected"
                      << " (val: " << duration_cast<milliseconds>(job->validate_end - job->validate_start).count() << "ms"
                      << ", apply: " << apply_time_ms << "ms"
                      << ", total: " << total_time << "ms)\n";
        }
    }
}

// ========== Validation Logic ==========

bool ValidationQueue::validateBlock(BlockValidationJob& job) {
    // Acquire READ lock for UTXO access (shared, allows concurrent validation)
    auto read_lock = chainstate_guard_->readLock();

    // Skip coinbase (index 0)
    if (job.block.vtx.size() <= 1) {
        // Only coinbase, no validation needed
        return true;
    }

    // v7: freeze-fork gates removed along with ring/CT stack.

    // Small block optimization: Don't parallelize if < 10 transactions
    if (job.block.vtx.size() < 10) {
        // Use single-threaded validation for small blocks
        for (size_t i = 1; i < job.block.vtx.size(); ++i) {
            const Transaction& tx = job.block.vtx[i];

            auto result = TransactionValidator::ValidateTransaction(tx, consensus_utxo_set_, job.height);
            if (!result.valid) {
                job.error_msg = "Tx validation failed: " + result.error;
                return false;
            }
        }
        return true;
    }

    // Large block: Parallelize script verification and UTXO checks
    std::vector<ValidationTask> tasks;
    tasks.reserve(job.block.vtx.size() * 2); // Rough estimate

    for (size_t tx_idx = 1; tx_idx < job.block.vtx.size(); ++tx_idx) {
        const Transaction& tx = job.block.vtx[tx_idx];

        // Create UTXO existence check tasks for each input
        for (size_t input_idx = 0; input_idx < tx.vin.size(); ++input_idx) {
            const auto& input = tx.vin[input_idx];

            ValidationTask task;
            task.type = ValidationTask::Type::CHECK_INPUT_EXISTS;
            task.tx_index = tx_idx;
            task.input_index = input_idx;
            // Phase M.4: input.prevout.txid is TxId, extract uint256 for task.prev_txid
            task.prev_txid = input.prevout.txid.AsUint256();
            task.prev_vout = input.prevout.vout;

            // Phase 2: Use IConsensusUTXOSet interface directly (O(1) lookup)
            task.custom_func = [this, txid = input.prevout.txid, vout = input.prevout.vout]
                               (std::string& error) -> bool {
                OutPoint outpoint(txid, vout);
                if (consensus_utxo_set_->HaveCoin(outpoint)) {
                    return true;
                }
                error = "UTXO not found: " + txid.AsUint256().GetHex() + ":" + std::to_string(vout);
                return false;
            };

            tasks.push_back(std::move(task));
        }

        // Add script verification tasks for each input
        // SegWit transactions require signature verification
        for (size_t input_idx = 0; input_idx < tx.vin.size(); ++input_idx) {
            ValidationTask script_task;
            script_task.type = ValidationTask::Type::VERIFY_SCRIPT;
            script_task.tx_index = tx_idx;
            script_task.input_index = input_idx;
            script_task.tx = tx;  // Copy transaction for verification

            tasks.push_back(std::move(script_task));
        }
    }

    // Submit tasks to worker pool
    job.validation_futures = worker_pool_->submitBatch(std::move(tasks));

    // Wait for all tasks to complete
    for (auto& future : job.validation_futures) {
        try {
            bool result = future.get();
            if (!result) {
                job.error_msg = "Parallel validation task failed";
                return false;
            }
        } catch (const std::exception& e) {
            job.error_msg = std::string("Validation exception: ") + e.what();
            return false;
        }
    }

    return true;
}

bool ValidationQueue::applyBlockToChain(BlockValidationJob& job) {
    // Acquire WRITE lock for chainstate modification (exclusive)
    auto write_lock = chainstate_guard_->writeLock();

    if (apply_block_callback_) {
        job.final_result = apply_block_callback_(job.block);
        if (!job.final_result.accepted()) {
            job.error_msg = job.final_result.reason;
            return false;
        }
        return true;
    }

    // Phase 2: Use IConsensusUTXOSet directly (no adapter chain)
    try {
        BlockUndo undo;
        std::string error;

        // Compute block hash for undo tracking
        // Phase M.1: ConnectBlock now takes uint256 directly
        if (!block_validator_->ConnectBlock(job.block, static_cast<uint32_t>(job.height), job.block.GetHash(), undo, error)) {
            job.error_msg = "ConnectBlock failed: " + error;
            job.final_result = BlockAcceptResult::Rejected(
                BlockRejectCode::CONNECT_FAILED,
                job.error_msg,
                job.block.GetHash(),
                job.height
            );
            return false;
        }

        // Track C: Liquidity Vault block-event hook. Drives the
        // deposit-flow lifecycle, reorg detection, and withdrawal
        // settlement of every account-bound deposit/withdrawal the
        // vault is tracking. No-op when vault is disabled by config.
        dinero::vault::NotifyVaultTipConnected(static_cast<uint64_t>(job.height));

        // Store undo data for reorg support
        // Note: Full reorg support requires either:
        //   1. Keeping connected jobs in memory (limited depth), OR
        //   2. Persisting undo data to disk (see activate_best_chain.cpp)
        // For now, store in job for potential later use.
        job.undo_data = std::make_unique<BlockUndo>(undo);
        job.final_result = BlockAcceptResult::Accepted(job.block.GetHash(), job.height, true);

        return true;

    } catch (const std::exception& e) {
        job.error_msg = std::string("Apply exception: ") + e.what();
        job.final_result = BlockAcceptResult::Rejected(
            BlockRejectCode::CONNECT_FAILED,
            job.error_msg,
            job.block.GetHash(),
            job.height
        );
        return false;
    }
}

// ========== Dependency Tracking ==========

bool ValidationQueue::areDependenciesMet(const BlockValidationJob& job) const {
    // Genesis block has no dependencies
    if (job.height == 0 || job.prev_block_hash.IsNull()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(heights_mutex_);
    if (connected_block_hashes_.count(job.prev_block_hash) > 0) {
        return true;
    }

    return parent_ready_callback_ ? parent_ready_callback_(job.prev_block_hash) : false;
}

void ValidationQueue::markBlockAsConnected(uint64_t height, const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(heights_mutex_);
    connected_heights_.insert(height);
    connected_block_hashes_.insert(block_hash);
    connected_blocks_by_height_[height].push_back(block_hash);
}

void ValidationQueue::completeJob(const std::shared_ptr<BlockValidationJob>& job) {
    if (!job || !job->completion) {
        return;
    }

    try {
        job->completion->set_value(job->final_result);
    } catch (const std::future_error&) {
        // Promise already satisfied/cancelled — safe to ignore during shutdown races.
    }
}

// ========== Queue Management ==========

std::shared_ptr<BlockValidationJob> ValidationQueue::popNextJob() {
    std::lock_guard<std::mutex> lock(pending_mutex_);

    if (pending_queue_.empty()) {
        return nullptr;
    }

    auto job = pending_queue_.top();
    pending_queue_.pop();
    return job;
}

std::shared_ptr<BlockValidationJob> ValidationQueue::popNextValidatedJob() {
    std::lock_guard<std::mutex> lock(validated_mutex_);

    if (validated_queue_.empty()) {
        return nullptr;
    }

    auto job = validated_queue_.front();
    validated_queue_.pop_front();
    return job;
}

size_t ValidationQueue::getQueuedCount() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return pending_queue_.size();
}

size_t ValidationQueue::getInFlightCount() const {
    return in_flight_count_.load();
}

// ========== Reorg Support ==========

void ValidationQueue::cancelBlocksAboveHeight(uint64_t height) {
    size_t cancelled = 0;

    // Cancel pending blocks
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        std::vector<std::shared_ptr<BlockValidationJob>> retained;
        while (!pending_queue_.empty()) {
            auto job = pending_queue_.top();
            pending_queue_.pop();
            if (job->height > height) {
                job->state = BlockValidationJob::State::CANCELLED;
                job->error_msg = "Cancelled by reorg";
                job->final_result = BlockAcceptResult::Rejected(
                    BlockRejectCode::CONNECT_FAILED,
                    "Cancelled by reorg",
                    job->block.GetHash(),
                    job->height
                );
                completeJob(job);
                cancelled++;
                continue;
            }
            retained.push_back(std::move(job));
        }
        for (auto& job : retained) {
            pending_queue_.push(std::move(job));
        }
    }

    // Cancel validated blocks
    {
        std::lock_guard<std::mutex> lock(validated_mutex_);

        auto it = validated_queue_.begin();
        while (it != validated_queue_.end()) {
            if ((*it)->height > height) {
                (*it)->state = BlockValidationJob::State::CANCELLED;
                (*it)->error_msg = "Cancelled by reorg";
                (*it)->final_result = BlockAcceptResult::Rejected(
                    BlockRejectCode::CONNECT_FAILED,
                    "Cancelled by reorg",
                    (*it)->block.GetHash(),
                    (*it)->height
                );
                completeJob(*it);
                cancelled++;
                it = validated_queue_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Remove connected heights > height
    {
        std::lock_guard<std::mutex> lock(heights_mutex_);

        auto it = connected_heights_.begin();
        while (it != connected_heights_.end()) {
            if (*it > height) {
                it = connected_heights_.erase(it);
            } else {
                ++it;
            }
        }

        auto blocks_it = connected_blocks_by_height_.upper_bound(height);
        while (blocks_it != connected_blocks_by_height_.end()) {
            for (const auto& hash : blocks_it->second) {
                connected_block_hashes_.erase(hash);
            }
            blocks_it = connected_blocks_by_height_.erase(blocks_it);
        }
    }

    metrics_.blocks_cancelled.fetch_add(cancelled);

    std::cout << "[ValidationQueue] Cancelled " << cancelled << " blocks above height " << height << "\n";
}

void ValidationQueue::waitForCompletion() {
    while (getQueuedCount() > 0 || getInFlightCount() > 0) {
        std::this_thread::sleep_for(milliseconds(100));
    }

    // Also wait for validated blocks to be applied
    while (true) {
        bool empty = false;
        {
            std::lock_guard<std::mutex> lock(validated_mutex_);
            empty = validated_queue_.empty();
        }
        if (empty) {
            break;
        }
        std::this_thread::sleep_for(milliseconds(100));
    }
}

} // namespace consensus
} // namespace dinero
