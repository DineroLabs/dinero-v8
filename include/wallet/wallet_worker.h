#pragma once

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>
#include "wallet/transaction.h"  // Need full definition for std::vector<Transaction>
#include "primitives/block.h"

namespace dinero {

// Forward declarations
class ChainDB;  // Phase W.1.1: For blockchain rescan

// ═══════════════════════════════════════════════════════════════════════════
// BLOCKCHAIN → WALLET NOTIFICATION TYPES
// ═══════════════════════════════════════════════════════════════════════════

struct BlockRef {
    uint32_t height;
    std::string hash;  // Using std::string for hash (hex-encoded)

    BlockRef() : height(0), hash("") {}
    BlockRef(uint32_t h, const std::string& hash_hex) : height(h), hash(hash_hex) {}
};

struct ReorgDiff {
    std::vector<BlockRef> disconnect;  // tip → fork+1 (descending order)
    std::vector<BlockRef> connect;     // fork+1' → new tip (ascending order)
};

// ═══════════════════════════════════════════════════════════════════════════
// WALLET JOB QUEUE
// ═══════════════════════════════════════════════════════════════════════════

enum class JobType {
    Connect,  // Process new block connection
    Disconnect, // Process block disconnect
    Reorg     // Process blockchain reorganization
};

struct WalletJob {
    JobType type;
    uint32_t height;
    std::string hash;
    ReorgDiff diff;
    std::vector<Transaction> transactions;  // Transaction data for block scanning
    Block block;                            // Full block for disconnect rollback

    // Constructors for different job types
    static WalletJob MakeConnect(uint32_t h, const std::string& hash_hex,
                                  const std::vector<Transaction>& txs) {
        WalletJob job;
        job.type = JobType::Connect;
        job.height = h;
        job.hash = hash_hex;
        job.transactions = txs;
        return job;
    }

    static WalletJob MakeReorg(const ReorgDiff& reorg_diff) {
        WalletJob job;
        job.type = JobType::Reorg;
        job.height = 0;
        job.diff = reorg_diff;
        return job;
    }

    static WalletJob MakeDisconnect(uint32_t h, const Block& disconnected_block) {
        WalletJob job;
        job.type = JobType::Disconnect;
        job.height = h;
        job.block = disconnected_block;
        return job;
    }

    // Default constructor (public for BlockingQueue compatibility)
    WalletJob() : type(JobType::Connect), height(0) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// BLOCKING QUEUE (Thread-safe)
// ═══════════════════════════════════════════════════════════════════════════

template<typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;

    void push(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
        lock.unlock();
        cv_.notify_one();
    }

    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || stop_; });

        if (stop_ && queue_.empty()) {
            // Return empty job when stopped
            return T{};
        }

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    void stop() {
        std::unique_lock<std::mutex> lock(mutex_);
        stop_ = true;
        lock.unlock();
        cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool stop_ = false;
};

// ═══════════════════════════════════════════════════════════════════════════
// WALLET WORKER THREAD
// ═══════════════════════════════════════════════════════════════════════════

class WalletWorker {
public:
    // Constructor accepts UTXOIndex* and WalletManager* for wallet scanning (dependency injection)
    explicit WalletWorker(class UTXOIndex* utxo_index = nullptr,
                         class WalletManager* wallet_manager = nullptr);
    ~WalletWorker();

    // Start the background worker thread
    void Start();

    // Stop the background worker thread (blocks until thread exits)
    void Stop();

    // Queue a block connection job
    void QueueBlockConnected(uint32_t height, const std::string& hash,
                             const std::vector<Transaction>& transactions);

    // Queue a reorg job
    void QueueReorg(const ReorgDiff& diff);

    // Queue a block disconnect job
    void QueueBlockDisconnected(uint32_t height, const Block& block);

    // Check if worker is running
    bool IsRunning() const { return running_.load(); }

    // Run a deterministic synchronous rescan through WalletManager.
    bool RescanSynchronously(class ChainDB* chain_db, int start_height, std::string* error = nullptr);

private:
    void WorkerThread();
    void ProcessConnect(uint32_t height, const std::string& hash,
                        const std::vector<Transaction>& transactions);
    void ProcessDisconnect(uint32_t height, const Block& block);
    void ProcessReorg(const ReorgDiff& diff);

    BlockingQueue<WalletJob> job_queue_;
    std::atomic<bool> running_;
    std::thread worker_thread_;
    class UTXOIndex* utxo_index_;  // Injected dependency (not owned)
    class WalletManager* wallet_manager_;  // Injected dependency for database persistence (not owned)
};

// ═══════════════════════════════════════════════════════════════════════════
// WALLET NOTIFICATION INTERFACE (called by blockchain layer)
// ═══════════════════════════════════════════════════════════════════════════

namespace WalletNotify {
    // Called when a new block is connected to the chain
    void OnBlockConnected(uint32_t height, const std::string& hash,
                          const std::vector<Transaction>& transactions);

    // Called when a blockchain reorganization occurs
    void OnReorg(const ReorgDiff& diff);

    // Called when a block is disconnected from the chain
    void OnBlockDisconnected(uint32_t height, const Block& block);

    // Initialize the wallet notification system (with UTXOIndex and WalletManager injection)
    void Initialize(class UTXOIndex* utxo_index = nullptr,
                   class WalletManager* wallet_manager = nullptr);

    // Shutdown the wallet notification system
    void Shutdown();

    // Phase W.1.1: Rescan blockchain from start_height to current tip
    // This is a synchronous operation that replays blocks through the wallet
    bool RescanBlockchain(ChainDB* chain_db, int start_height);

    // Priority 3 FIX: Validate wallet UTXOs against consensus
    // Removes phantom UTXOs (wallet thinks unspent, but consensus doesn't have them)
    // Should be called after reorg completes or at startup
    // Returns count of phantom UTXOs removed
    size_t ValidateUTXOs(ChainDB* chain_db);
}

} // namespace dinero
