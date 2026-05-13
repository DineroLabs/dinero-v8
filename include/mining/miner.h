#pragma once

#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <cstdint>
#include "mining/gpu/compute_backend.h"
#include "mining/gpu/gpu_device_manager.h"

// Forward declarations
namespace dinero {
    class BlockAssembler;
    class ChainDB;  // ChainDB for context injection
    struct ChainParams;
    struct MiningJob;
    struct Block;
}

// Include BlockHeader definition
#include "mining/block_assembler.h"

namespace dinero {

/**
 * @brief Mining statistics for monitoring
 */
struct MiningStats {
    std::atomic<uint64_t> total_hashes{0};
    std::atomic<uint64_t> blocks_found{0};
    std::atomic<uint64_t> jobs_processed{0};
    std::atomic<double> current_hashrate{0.0};
    std::atomic<uint32_t> active_threads{0};
    std::atomic<bool> is_mining{false};
    
    // Timing
    std::atomic<uint64_t> mining_start_time{0};
    std::atomic<uint64_t> last_block_time{0};
    
    // Current job info
    std::string current_job_id;
    uint32_t current_height{0};
    uint32_t current_difficulty{0};
    std::string mining_phase;
};

/**
 * @brief CPU miner implementation
 * 
 * This class manages multiple CPU mining threads that work on
 * mining jobs created by the BlockAssembler. It handles:
 * - Thread management and work distribution
 * - Nonce space allocation
 * - Hash rate monitoring
 * - Block submission when solutions are found
 */
class Miner {
public:
    explicit Miner(ChainDB* chain_db);
    ~Miner();

    /**
     * @brief Start mining with specified number of threads
     * 
     * @param num_threads Number of mining threads to start
     * @param mining_address Address to receive mining rewards
     * @return bool True if mining started successfully
     */
    bool StartMining(uint32_t num_threads, const std::string& mining_address);

    /**
     * @brief Stop all mining threads
     */
    void StopMining();

    /**
     * @brief Check if mining is currently active
     * 
     * @return bool True if mining is active
     */
    bool IsMining() const { return stats_.is_mining.load(); }

    /**
     * @brief Get current mining statistics
     * 
     * @return MiningStats& Reference to current mining statistics
     */
    MiningStats& GetStats() const;

    /**
     * @brief Get formatted mining information
     * 
     * @return std::string Human-readable mining information
     */
    std::string GetMiningInfo() const;

    /**
     * @brief Set mining address for rewards
     * 
     * @param address Mining address (din1... format)
     */
    void SetMiningAddress(const std::string& address);

    /**
     * @brief Get current mining address
     * 
     * @return std::string Current mining address
     */
    std::string GetMiningAddress() const { return mining_address_; }

    /**
     * @brief Force refresh of current mining job
     *
     * Called when blockchain state changes or periodically
     * to ensure miners are working on current data.
     */
    void RefreshMiningJob();

    // Phase 39: ChainManager methods removed (ChainManager deleted)

    // Configuration
    void SetHashrateReportInterval(uint32_t seconds) { hashrate_report_interval_ = seconds; }
    void SetJobRefreshInterval(uint32_t seconds) { job_refresh_interval_ = seconds; }
    void SetNonceRangeSize(uint32_t range_size) { nonce_range_size_ = range_size; }

private:
    /**
     * @brief Main mining thread function
     * 
     * @param thread_id Unique thread identifier
     */
    void MinerThread(uint32_t thread_id);

    /**
     * @brief Mine a specific nonce range
     * 
     * @param job Mining job to work on
     * @param start_nonce Starting nonce value
     * @param end_nonce Ending nonce value
     * @param thread_id Thread identifier
     * @return bool True if solution found
     */
    bool MineNonceRange(std::shared_ptr<MiningJob> job, uint32_t start_nonce, 
                       uint32_t end_nonce, uint32_t thread_id);

    /**
     * @brief Check if hash meets target difficulty
     * 
     * @param hash Block hash to check
     * @param target Target difficulty string
     * @return bool True if hash meets target
     */
    bool CheckHashTarget(const std::string& hash, const std::string& target) const;

    /**
     * @brief Submit found block to blockchain
     * 
     * @param job Mining job that found solution
     * @param nonce Winning nonce value
     * @param thread_id Thread that found solution
     * @return bool True if block was accepted
     */
    bool SubmitBlock(std::shared_ptr<MiningJob> job, uint32_t nonce, uint32_t thread_id);

    /**
     * @brief Get next nonce range for thread
     * 
     * @param start_nonce Output: starting nonce
     * @param end_nonce Output: ending nonce
     * @return bool True if range available
     */
    bool GetNextNonceRange(uint32_t& start_nonce, uint32_t& end_nonce);

    /**
     * @brief Update hash rate statistics
     * 
     * @param hashes Number of hashes completed
     * @param elapsed_ms Time elapsed in milliseconds
     */
    void UpdateHashrate(uint64_t hashes, uint64_t elapsed_ms);

    /**
     * @brief Create new mining job
     * 
     * @return std::shared_ptr<MiningJob> New mining job or nullptr if failed
     */
    std::shared_ptr<MiningJob> CreateNewJob();

    /**
     * @brief Broadcast mining statistics
     * 
     * Sends mining stats to WebSocket clients and logs
     */
    void BroadcastMiningStats();

    /**
     * @brief Job management thread
     * 
     * Handles job creation, refresh, and distribution
     */
    void JobManagerThread();

private:
    ChainDB* chain_db_;  // ChainDB for block submission
    // Phase 39: chain_manager_ removed (ChainManager deleted)
    std::unique_ptr<BlockAssembler> block_assembler_;

    // Mining state
    std::atomic<bool> mining_enabled_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::string mining_address_;
    
    // Threading
    std::vector<std::thread> mining_threads_;
    std::thread job_manager_thread_;
    uint32_t num_threads_{0};
    
    // Job management
    std::shared_ptr<MiningJob> current_job_;
    std::mutex job_mutex_;
    std::condition_variable job_condition_;
    std::atomic<bool> job_available_{false};
    std::atomic<bool> job_refresh_needed_{false};
    
    // Nonce management
    std::atomic<uint32_t> next_nonce_start_{0};
    std::mutex nonce_mutex_;
    
    // Statistics
    mutable MiningStats stats_;
    mutable std::mutex stats_mutex_;
    
    // Configuration
    uint32_t hashrate_report_interval_{10};  // seconds
    uint32_t job_refresh_interval_{30};      // seconds
    uint32_t nonce_range_size_{0x10000};     // 64K nonces per thread

    // Timing
    std::chrono::steady_clock::time_point last_hashrate_report_;
    std::chrono::steady_clock::time_point last_job_refresh_;

    // GPU mining support
    std::unique_ptr<gpu::IComputeBackend> gpu_backend_;
    gpu::GPUDeviceManager gpu_device_manager_;
    std::thread gpu_mining_thread_;
    std::atomic<bool> gpu_available_{false};
    std::atomic<double> gpu_hashrate_{0.0};
    static constexpr uint32_t GPU_NONCE_BATCH_SIZE = 0x1000000; // 16M nonces per GPU batch

    void GPUMinerThread();
    bool InitGPU();
};

/**
 * @brief Dinero-specific proof-of-work algorithm
 * 
 * Implements the mining algorithm used by Dinero, which is
 * double SHA-256 (same as Bitcoin) but with Dinero-specific
 * difficulty adjustment and target calculation.
 */
class DineroPoW {
public:
    /**
     * @brief Calculate block hash for mining
     * 
     * @param header Block header to hash
     * @return std::string Block hash as hex string
     */
    static std::string CalculateBlockHash(const BlockHeader& header);

    /**
     * @brief Check if hash meets target
     * 
     * @param hash Block hash
     * @param target_bits Compact difficulty target
     * @return bool True if hash meets target
     */
    static bool MeetsTarget(const std::string& hash, uint32_t target_bits);

    /**
     * @brief Convert compact bits to target hex string
     * 
     * @param bits Compact difficulty bits
     * @return std::string Target as hex string
     */
    static std::string BitsToTargetHex(uint32_t bits);

    /**
     * @brief Get mining algorithm name
     * 
     * @return std::string Algorithm name
     */
    static std::string GetAlgorithmName() { return "SHA256d"; }
};

} // namespace dinero
