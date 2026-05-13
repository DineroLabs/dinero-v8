// SPDX-License-Identifier: MIT
// Dinero - Mining Manager Header

#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include "mining/block_assembler.h"

// Forward declarations (Week 9 - GPU Mining Integration)
#ifdef ENABLE_GPU_MINING
namespace dinero {
    namespace gpu {
        class IComputeBackend;
    }
}
#endif

namespace dinero {

struct MiningInfo {
    bool is_mining = false;
    int thread_count = 0;
    double hashrate = 0.0;
    std::string mining_address;
    int blocks_mined = 0;
    int last_block_time = 0;
    double difficulty = 0.0;
    double network_hashrate = 0.0;

    // GPU mining fields
    bool gpu_mining_enabled = false;
    bool gpu_available = false;
    int gpu_device_count = 0;
    double gpu_hashrate = 0.0;
    std::string gpu_device_name;
    std::string gpu_backend;  // "OpenCL", "CUDA", or "None"
};

class MiningManager {
public:
    static MiningManager& getInstance();
    
    // Destructor must be public for unique_ptr
    ~MiningManager();
    
    // Mining control
    bool startMining(int threads = 0);
    bool stopMining();
    bool isMining() const { return is_mining_.load(); }
    
    // Block template management
    std::shared_ptr<MiningJob> getCurrentJob() const;
    void refreshJob();
    bool submitBlock(const Block& block);
    
    // Mining configuration
    void setMiningAddress(const std::string& address);
    std::string getMiningAddress() const;
    
    // Mining information
    MiningInfo getMiningInfo() const;
    int getOptimalThreadCount() const;
    
    
    // Week 5: ChainDB for context injection
    void setChainDB(class ChainDB* chain_db);
    class ChainDB* getChainDB() const { return chain_db_; }

    // ChainManager for activation layer (SINGLE SOURCE OF TRUTH)
    void setChainManager(class ChainManager* chain_manager);
    class ChainManager* getChainManager() const { return chain_manager_; }

    // Week 7: Mempool for transaction selection
    void setMempool(class Mempool* mempool);
    class Mempool* getMempool() const { return mempool_; }
    
    // Thread management
    void setThreadCount(int threads);
    int getThreadCount() const { return thread_count_.load(); }
    
private:
    MiningManager();
    
    // Mining state
    std::atomic<bool> is_mining_{false};
    std::atomic<int> thread_count_{0};
    std::atomic<double> current_hashrate_{0.0};
    std::string mining_address_;
    mutable std::mutex address_mutex_;
    
    // Block template management
    std::shared_ptr<MiningJob> current_job_;
    mutable std::mutex job_mutex_;
    std::unique_ptr<BlockAssembler> block_assembler_;
    class ChainDB* chain_db_ = nullptr;  // Week 5: ChainDB for context injection
    class ChainManager* chain_manager_ = nullptr;  // ChainManager for activation layer (SINGLE SOURCE OF TRUTH)
    class Mempool* mempool_ = nullptr;  // Week 7: Mempool for transaction selection
    
    // Mining threads
    std::vector<std::thread> mining_threads_;
    std::atomic<bool> should_stop_{false};

    // GPU mining (Week 9 - GPU Mining Integration)
#ifdef ENABLE_GPU_MINING
    std::unique_ptr<dinero::gpu::IComputeBackend> gpu_backend_;
    std::thread gpu_mining_thread_;
    std::atomic<bool> gpu_enabled_{false};
    std::atomic<double> gpu_hashrate_{0.0};
    mutable std::mutex gpu_mutex_;
#endif

    // Statistics
    std::atomic<int> blocks_mined_{0};
    std::atomic<int> last_block_time_{0};
    std::atomic<uint64_t> total_hashes_{0};
    std::atomic<uint64_t> last_hashrate_time_{0};
    
    // Mining loop
    void miningLoop(int thread_id);
    void gpuMiningLoop();  // GPU mining thread worker
    void updateHashrate();
    
    // Block mining helpers
    bool tryNonce(std::shared_ptr<MiningJob> job, uint32_t nonce, uint32_t thread_id);
    std::string calculateBlockHash(const BlockHeader& header) const;
    bool meetsTarget(const std::string& hash, const std::string& target) const;
    void onBlockFound(std::shared_ptr<MiningJob> job, uint32_t winning_nonce);
    
    // Singleton
    static std::unique_ptr<MiningManager> instance_;
    static std::mutex instance_mutex_;
};

} // namespace dinero
