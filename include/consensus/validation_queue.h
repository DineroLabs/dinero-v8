#pragma once

/**
 * Phase 6B: Validation Queue for Block Pipelining
 *
 * ValidationQueue - Manages parallel block validation with dependency ordering
 *
 * Key Features:
 * - **Parallel block validation** for independent blocks
 * - **Dependency tracking** (block N+1 waits for block N)
 * - **Priority queue** (higher blocks processed first during IBD)
 * - **Backpressure handling** (limits in-flight blocks)
 * - **Graceful reorg support** (cancels invalidated work)
 *
 * Architecture:
 *   Network Thread → ValidationQueue::submit(block)
 *                       ↓
 *              [Dependency Check]
 *                       ↓
 *              [Worker Pool] → Parallel validation
 *                       ↓
 *         [Canonical Apply Callback] → BlockAcceptor/Chainstate update (serialized)
 *                       ↓
 *              Notify: block_connected
 *
 * Thread Safety:
 * - Multiple submitters allowed (network threads)
 * - Single applier thread (chainstate mutations)
 * - Worker pool handles parallelizable validation
 */

#include "primitives/block.h"
#include "consensus/block_undo.h"
#include "consensus/validation_worker_pool.h"
#include "consensus/utreexo_accumulator.h"  // v0.14.0.4: Utreexo enforcement
#include "daemon/interfaces/ingress_types.h"
#include <queue>
#include <map>
#include <set>
#include <deque>
#include <future>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_set>

namespace dinero {
namespace consensus {

// Forward declarations
class IConsensusUTXOSet;
class ChainstateGuard;
class BlockValidator;
/**
 * BlockValidationJob - Represents a block undergoing validation
 */
struct BlockValidationJob {
    enum class State {
        QUEUED,              // Waiting for dependencies
        VALIDATING,          // Script/UTXO checks in progress
        VALIDATED,           // All checks passed, ready to apply
        APPLYING,            // Connecting to chainstate
        CONNECTED,           // Successfully added to chain
        FAILED,              // Validation failed
        CANCELLED            // Cancelled due to reorg
    };

    Block block;
    uint64_t height;
    uint256 prev_block_hash;  // Phase M.0: uint256 identity

    State state;
    std::string error_msg;

    // Timing metrics
    std::chrono::steady_clock::time_point submit_time;
    std::chrono::steady_clock::time_point validate_start;
    std::chrono::steady_clock::time_point validate_end;
    std::chrono::steady_clock::time_point apply_end;

    // Validation results
    std::vector<std::future<bool>> validation_futures;
    std::unique_ptr<BlockUndo> undo_data;
    std::shared_ptr<std::promise<BlockAcceptResult>> completion;
    BlockAcceptResult final_result;

    BlockValidationJob(const Block& b, uint64_t h, const uint256& prev_hash)
        : block(b), height(h), prev_block_hash(prev_hash),
          state(State::QUEUED),
          final_result(BlockAcceptResult::Rejected(BlockRejectCode::CONNECT_FAILED, "Block not processed"))
    {
        submit_time = std::chrono::steady_clock::now();
    }

    ~BlockValidationJob() = default;

    // Disable copy to prevent double-delete
    BlockValidationJob(const BlockValidationJob&) = delete;
    BlockValidationJob& operator=(const BlockValidationJob&) = delete;

    // Allow move
    BlockValidationJob(BlockValidationJob&& other) = default;
    BlockValidationJob& operator=(BlockValidationJob&& other) = default;
};

/**
 * ValidationQueue - Coordinates parallel block validation with dependency ordering
 */
class ValidationQueue {
public:
    struct Config {
        size_t max_in_flight_blocks = 16;    // Max blocks validating concurrently
        size_t max_queued_blocks = 128;      // Max blocks waiting
        bool enable_pipelining = true;       // Allow overlapping validation
        bool enable_priority = true;          // Prioritize by height (IBD)
        size_t worker_pool_threads = 0;      // 0 = auto (from ValidationWorkerPool)

        static Config forIBD();
        static Config forNormalOperation();
    };

    using BlockConnectedCallback = std::function<void(const Block&, uint64_t height)>;
    using BlockFailedCallback = std::function<void(const Block&, const std::string& error)>;
    using BlockApplyCallback = std::function<BlockAcceptResult(const Block&)>;
    using ParentReadyCallback = std::function<bool(const uint256&)>;

    // Phase 2: Takes IConsensusUTXOSet* directly (no adapter chain)
    explicit ValidationQueue(
        IConsensusUTXOSet* consensus_utxo_set,
        ChainstateGuard* chainstate_guard,
        const Config& config = Config::forNormalOperation()
    );
    ~ValidationQueue();

    // Lifecycle
    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    // Block submission (from network/RPC) - Phase M.0: uint256 identity
    bool submit(const Block& block, uint64_t height, const uint256& prev_hash);
    BlockAcceptResult submitAndWait(const Block& block, uint64_t height, const uint256& prev_hash);

    // Callbacks
    void setBlockConnectedCallback(BlockConnectedCallback cb) { on_block_connected_ = cb; }
    void setBlockFailedCallback(BlockFailedCallback cb) { on_block_failed_ = cb; }
    void setBlockApplyCallback(BlockApplyCallback cb) { apply_block_callback_ = std::move(cb); }
    void setParentReadyCallback(ParentReadyCallback cb) { parent_ready_callback_ = std::move(cb); }
    // Queue management
    size_t getQueuedCount() const;
    size_t getInFlightCount() const;
    size_t getTotalProcessed() const { return total_processed_.load(); }

    // Reorg support
    void cancelBlocksAboveHeight(uint64_t height);

    // Wait for all pending blocks to complete
    void waitForCompletion();

    // Metrics
    struct Metrics {
        std::atomic<uint64_t> blocks_submitted{0};
        std::atomic<uint64_t> blocks_validated{0};
        std::atomic<uint64_t> blocks_connected{0};
        std::atomic<uint64_t> blocks_failed{0};
        std::atomic<uint64_t> blocks_cancelled{0};

        std::atomic<uint64_t> total_validation_time_ms{0};
        std::atomic<uint64_t> total_apply_time_ms{0};

        void reset();
        std::string toString() const;
    };

    const Metrics& getMetrics() const { return metrics_; }
    void resetMetrics() { metrics_.reset(); }

private:
    // Thread entry points
    void validationThreadFunc();  // Dispatches blocks to worker pool
    void applierThreadFunc();     // Applies validated blocks to chainstate

    // Validation logic
    bool validateBlock(BlockValidationJob& job);
    bool applyBlockToChain(BlockValidationJob& job);
    void completeJob(const std::shared_ptr<BlockValidationJob>& job);

    // Dependency tracking
    bool areDependenciesMet(const BlockValidationJob& job) const;
    void markBlockAsConnected(uint64_t height, const uint256& block_hash);

    // Queue management
    std::shared_ptr<BlockValidationJob> popNextJob();
    std::shared_ptr<BlockValidationJob> popNextValidatedJob();
    bool enqueueJob(const std::shared_ptr<BlockValidationJob>& job);

    Config config_;
    Metrics metrics_;

    // Phase 2: Direct IConsensusUTXOSet (owns forest, no adapter needed)
    IConsensusUTXOSet* consensus_utxo_set_;

    // Chainstate guard for thread-safe UTXO access
    ChainstateGuard* chainstate_guard_;

    // Worker pool for parallel script/UTXO checks
    std::unique_ptr<ValidationWorkerPool> worker_pool_;
    std::unique_ptr<BlockValidator> block_validator_;

    // Thread pool
    std::thread validation_thread_;
    std::thread applier_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};

    // Job queues
    std::priority_queue<
        std::shared_ptr<BlockValidationJob>,
        std::vector<std::shared_ptr<BlockValidationJob>>,
        std::function<bool(const std::shared_ptr<BlockValidationJob>&,
                           const std::shared_ptr<BlockValidationJob>&)>
    > pending_queue_;  // Blocks waiting for validation

    std::deque<std::shared_ptr<BlockValidationJob>> validated_queue_; // Blocks validated, waiting to apply

    mutable std::mutex pending_mutex_;
    mutable std::mutex validated_mutex_;

    std::condition_variable pending_cv_;
    std::condition_variable validated_cv_;

    // Completed heights (for dependency tracking)
    std::set<uint64_t> connected_heights_;
    std::unordered_set<uint256> connected_block_hashes_;
    std::map<uint64_t, std::vector<uint256>> connected_blocks_by_height_;
    mutable std::mutex heights_mutex_;

    // Callbacks
    BlockConnectedCallback on_block_connected_;
    BlockFailedCallback on_block_failed_;
    BlockApplyCallback apply_block_callback_;
    ParentReadyCallback parent_ready_callback_;

    // Counters
    std::atomic<size_t> in_flight_count_{0};
    std::atomic<uint64_t> total_processed_{0};
};

} // namespace consensus
} // namespace dinero
