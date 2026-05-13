#pragma once

#include "mining/miner_engine.h"
#include "primitives/block.h"
#include "daemon/daemon_context.h"
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <map>

namespace dinero {

// Forward declarations
class ChainDB;

namespace mining {

// ============================================================================
// Mining Job (Stratum-compatible)
// ============================================================================

/**
 * Mining job distributed to workers
 *
 * Compatible with Stratum V1/V2 and internal miners.
 * Contains all information needed to start hashing.
 */
struct MiningJob {
    std::string job_id;                  // Unique job identifier
    uint32_t version;                    // Block version
    std::string prev_hash;               // Previous block hash (hex)
    std::string merkle_root;             // Merkle root (hex)
    uint64_t timestamp;                  // Block timestamp
    uint32_t bits;                       // Difficulty target (compact form)
    uint32_t height;                     // Block height

    // Extranonce handling (Stratum)
    std::string extranonce1;             // Per-session extranonce
    size_t extranonce2_size;             // Size of extranonce2 (bytes)
    std::vector<std::string> merkle_branch;  // Merkle branch for root calculation

    // Coinbase transaction (for merkle root updates)
    std::string coinbase_tx_hex;         // Coinbase transaction (hex) - full coinbase for non-Stratum
    std::string coinbase1;               // Stratum V1: coinbase prefix (up to extranonce)
    std::string coinbase2;               // Stratum V1: coinbase suffix (after extranonce)
    size_t coinbase_value;               // Coinbase value (una)

    // Difficulty management
    double share_difficulty;             // Pool difficulty (for shares)
    double network_difficulty;           // Network difficulty (for blocks)

    // Block transactions (excluding coinbase - coinbase is reconstructed with extranonces)
    std::vector<Transaction> transactions;  // Non-coinbase transactions for this block

    // Metadata
    uint64_t created_at;                 // Job creation timestamp
    bool clean_jobs;                     // Abandon previous jobs?
};

// ============================================================================
// Share Submission (from workers)
// ============================================================================

/**
 * Share submitted by a worker
 */
struct ShareSubmission {
    std::string job_id;                  // Job ID
    std::string worker_name;             // Worker identifier
    std::string extranonce2;             // Worker's extranonce2 (hex)
    uint64_t timestamp;                  // Timestamp used
    uint32_t nonce;                      // Nonce found
    std::string version_bits;            // Version bits (optional, BIP 9)

    // Computed by validator
    std::string block_hash;              // Hash of the block header
    bool meets_network_target;           // True if this is a valid block
    bool meets_share_target;             // True if this meets pool difficulty
};

// ============================================================================
// Mining Coordinator
// ============================================================================

/**
 * Mining Coordinator
 *
 * Central orchestrator for all mining activities:
 * - Builds block templates
 * - Distributes jobs to workers (CPU/GPU/Stratum)
 * - Validates shares
 * - Submits blocks
 *
 * This is the "mining pool coordinator" design used by professional pools.
 */
class MiningCoordinator {
public:
    /**
     * Worker types
     */
    enum class WorkerType {
        CPU,           // Internal CPU miner
        GPU,           // Internal GPU miner
        STRATUM_V1,    // Stratum V1 client
        STRATUM_V2,    // Stratum V2 client
        EXTERNAL       // Other external miner
    };

    /**
     * Worker statistics
     */
    struct WorkerStats {
        std::string worker_id;
        WorkerType type;
        std::string extranonce1;       // Per-session extranonce1 (Stratum V1)
        uint64_t shares_accepted;
        uint64_t shares_rejected;
        uint64_t blocks_found;
        double hashrate;               // Current hashrate (H/s)
        double difficulty;             // Current share difficulty
        uint64_t last_share_time;      // Last share timestamp
    };

    MiningCoordinator(DaemonContext* daemon_ctx);
    ~MiningCoordinator();

    // ========================================================================
    // Template Management
    // ========================================================================

    /**
     * Create new mining job
     *
     * @param mining_address    Address for coinbase
     * @return                  Mining job
     */
    std::shared_ptr<MiningJob> createJob(const std::string& mining_address);

    /**
     * Get current job
     */
    std::shared_ptr<MiningJob> getCurrentJob() const;

    /**
     * Force job refresh (e.g., new block arrived)
     */
    void refreshJobs();

    // ========================================================================
    // Share Validation & Submission
    // ========================================================================

    /**
     * Validate and submit share
     *
     * @param share             Share submission
     * @param worker_id         Worker identifier
     * @param worker_type       Worker type
     * @return                  True if share is valid
     */
    bool submitShare(
        ShareSubmission& share,
        const std::string& worker_id,
        WorkerType worker_type
    );

    /**
     * Submit block to network
     *
     * @param block             Block to submit
     * @return                  True if accepted
     */
    bool submitBlock(const Block& block);

    // ========================================================================
    // Worker Management
    // ========================================================================

    /**
     * Register worker
     *
     * @param worker_id         Worker identifier
     * @param type              Worker type
     */
    void registerWorker(const std::string& worker_id, WorkerType type);

    /**
     * Unregister worker
     */
    void unregisterWorker(const std::string& worker_id);

    /**
     * Get worker statistics
     */
    std::vector<WorkerStats> getWorkerStats() const;

    // ========================================================================
    // Difficulty Management
    // ========================================================================

    /**
     * Set share difficulty for worker
     *
     * @param worker_id         Worker identifier
     * @param difficulty        Difficulty
     */
    void setWorkerDifficulty(const std::string& worker_id, double difficulty);

    /**
     * Get recommended difficulty for worker
     *
     * @param worker_id         Worker identifier
     * @param hashrate          Worker hashrate (H/s)
     * @return                  Recommended difficulty
     */
    double calculateRecommendedDifficulty(
        const std::string& worker_id,
        double hashrate
    );

    // ========================================================================
    // Statistics
    // ========================================================================

    struct CoordinatorStats {
        uint64_t total_shares;
        uint64_t total_blocks;
        double total_hashrate;
        int active_workers;
        std::string current_job_id;
        uint32_t current_height;
    };

    CoordinatorStats getStats() const;

private:
    DaemonContext* daemon_ctx_;

    std::shared_ptr<MiningJob> current_job_;
    mutable std::mutex job_mutex_;
    std::atomic<uint64_t> job_counter_;

    // Worker tracking
    std::map<std::string, WorkerStats> workers_;
    mutable std::mutex workers_mutex_;

    // Statistics
    std::atomic<uint64_t> total_shares_;
    std::atomic<uint64_t> total_blocks_;

    // Helper methods
    std::string generateJobId();
    std::string generateExtranonce1(const std::string& worker_id);
    bool validateShare(ShareSubmission& share, const MiningJob& job, const std::string& extranonce1);
    bool checkProofOfWork(const std::string& block_hash, uint32_t bits);
    Block reconstructBlock(const ShareSubmission& share, const MiningJob& job, const std::string& extranonce1);
};

} // namespace mining
} // namespace dinero
