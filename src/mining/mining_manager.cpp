// SPDX-License-Identifier: MIT
// Dinero - Mining Manager Implementation

#include "mining/mining_manager.h"
#include "common/logger.h"
#include "daemon/mempool.h"  // Week 7: Mempool for transaction selection
#include "storage/chain_db.h"  // Week 5: ChainDB for context injection
#include "primitives/block.h"
#include "crypto/sha256.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <iomanip>
#include <sstream>

// GPU mining headers (Week 9 - GPU Mining Integration)
#ifdef ENABLE_GPU_MINING
#include "mining/gpu/gpu_device_manager.h"
#include "mining/gpu/compute_backend.h"
#include "consensus/consensus.hpp"  // For governance params
#endif

namespace dinero {

std::unique_ptr<MiningManager> MiningManager::instance_;
std::mutex MiningManager::instance_mutex_;

MiningManager::MiningManager()
{
    // Initialize default state
}

MiningManager& MiningManager::getInstance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (!instance_) {
        instance_ = std::unique_ptr<MiningManager>(new MiningManager());
    }
    return *instance_;
}

MiningManager::~MiningManager() {
    stopMining();
}

bool MiningManager::startMining(int threads) {
    if (is_mining_.load()) {
        dinero::g_logger.warning("Mining already running");
        return false;
    }
    
    if (mining_address_.empty()) {
        dinero::g_logger.error("Cannot start mining: no mining address set");
        return false;
    }
    
    if (!chain_db_) {
        dinero::g_logger.error("Cannot start mining: ChainDB not set");
        return false;
    }
    
    // Initialize block assembler if needed
    if (!block_assembler_ && chain_db_ && chain_manager_) {
        // Phase 39: BlockAssembler constructor updated (ChainManager parameter removed)
        block_assembler_ = std::make_unique<BlockAssembler>(chain_db_);
        // CRITICAL: Set mining address on newly created BlockAssembler
        block_assembler_->SetMiningAddress(mining_address_);
        dinero::g_logger.info("Initialized BlockAssembler with mining address: " + mining_address_);
    } else if (!block_assembler_ && !chain_manager_) {
        dinero::g_logger.error("Cannot initialize BlockAssembler: ChainManager not set");
        return false;
    }

    // Create initial mining job
    refreshJob();
    if (!current_job_) {
        dinero::g_logger.error("Failed to create initial mining job");
        return false;
    }
    
    // Determine thread count
    if (threads <= 0) {
        threads = getOptimalThreadCount();
    }
    
    thread_count_.store(threads);
    should_stop_.store(false);
    is_mining_.store(true);
    
    // Reset statistics
    total_hashes_.store(0);
    last_hashrate_time_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    // Start mining threads
    mining_threads_.clear();
    mining_threads_.reserve(threads);
    
    for (int i = 0; i < threads; i++) {
        mining_threads_.emplace_back(&MiningManager::miningLoop, this, i);
    }

    dinero::g_logger.info("Started mining with " + std::to_string(threads) + " threads, address: " + mining_address_);

    // GPU mining initialization (Week 9 - GPU Mining Integration)
    #ifdef ENABLE_GPU_MINING
    initializeGPUMining();
    #endif

    return true;
}

// GPU Mining Helper Functions (Week 9)
#ifdef ENABLE_GPU_MINING
void MiningManager::initializeGPUMining() {
    // Get governance parameters
    extern dinero::Consensus::Params g_consensus_params;

    if (!g_consensus_params.allowGPUMining) {
        dinero::g_logger.info("[GPU] GPU mining disabled by governance");
        return;
    }

    dinero::g_logger.info("[GPU] GPU mining allowed by governance, detecting devices...");

    // Detect GPU devices
    dinero::gpu::GPUDeviceManager gpu_mgr;
    auto devices = gpu_mgr.enumerateAllDevices();

    if (devices.empty()) {
        dinero::g_logger.info("[GPU] No GPU devices detected");
        return;
    }

    dinero::g_logger.info("[GPU] Found " + std::to_string(devices.size()) + " GPU device(s)");

    // Initialize first available GPU (for now, single GPU support)
    const auto& selected_device = devices[0];
    dinero::g_logger.info("[GPU] Initializing device: " + selected_device.name +
                         " (" + std::to_string(selected_device.compute_units) + " CUs)");

    gpu_backend_ = dinero::gpu::createComputeBackend(selected_device.backend);
    if (!gpu_backend_) {
        dinero::g_logger.error("[GPU] Failed to create GPU backend");
        return;
    }

    if (!gpu_backend_->initDevice(selected_device.device_id)) {
        dinero::g_logger.error("[GPU] Failed to initialize GPU device");
        gpu_backend_.reset();
        return;
    }

    // Load and compile GPU kernel
    std::string kernel_source;
    if (selected_device.backend == dinero::gpu::BackendType::OPENCL) {
        // Load OpenCL kernel from file
        std::ifstream kernel_file("/Users/haydarevich/Documents/Dinero/src/mining/gpu/kernels/sha256d_opencl.cl");
        if (kernel_file.is_open()) {
            std::stringstream buffer;
            buffer << kernel_file.rdbuf();
            kernel_source = buffer.str();
            dinero::g_logger.info("[GPU] Loaded OpenCL kernel (" + std::to_string(kernel_source.length()) + " bytes)");
        } else {
            dinero::g_logger.error("[GPU] Failed to load OpenCL kernel file");
            gpu_backend_.reset();
            return;
        }
    } else {
        // CUDA kernel is pre-compiled (stub for now)
        dinero::g_logger.info("[GPU] Using pre-compiled CUDA kernel");
    }

    if (!gpu_backend_->compileKernel(kernel_source)) {
        dinero::g_logger.error("[GPU] Failed to compile GPU kernel");
        gpu_backend_.reset();
        return;
    }

    // Start GPU mining thread
    gpu_enabled_.store(true);
    gpu_mining_thread_ = std::thread(&MiningManager::gpuMiningLoop, this);

    dinero::g_logger.info("[GPU] GPU mining started: " + gpu_backend_->getDeviceName());
}
#endif

bool MiningManager::stopMining() {
    if (!is_mining_.load()) {
        return false;
    }

    should_stop_.store(true);
    is_mining_.store(false);

    // Stop GPU mining (Week 9 - GPU Mining Integration)
    #ifdef ENABLE_GPU_MINING
    gpu_enabled_.store(false);
    #endif

    // Wait for all CPU threads to finish
    for (auto& thread : mining_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    mining_threads_.clear();
    current_hashrate_.store(0.0);

    // Wait for GPU thread and clean up GPU resources (Week 9)
    #ifdef ENABLE_GPU_MINING
    if (gpu_mining_thread_.joinable()) {
        gpu_mining_thread_.join();
    }

    if (gpu_backend_) {
        gpu_backend_->stop();
        gpu_backend_.reset();
    }

    gpu_hashrate_.store(0.0);
    dinero::g_logger.info("[GPU] GPU mining stopped and resources cleaned up");
    #endif

    // Clear current job
    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        if (current_job_) {
            current_job_->stop_mining.store(true);
            current_job_.reset();
        }
    }

    dinero::g_logger.info("Stopped mining (CPU + GPU)");
    return true;
}

void MiningManager::setMiningAddress(const std::string& address) {
    std::lock_guard<std::mutex> lock(address_mutex_);

    if (address.empty()) {
        dinero::g_logger.error("Cannot set empty mining address");
        return;
    }

    // Basic address validation would go here
    // For now, accept any non-empty address

    mining_address_ = address;

    // CRITICAL: Pass address to BlockAssembler so it can generate mining scripts
    if (block_assembler_) {
        block_assembler_->SetMiningAddress(address);
        dinero::g_logger.info("Set mining address: " + address + " (propagated to BlockAssembler)");
    } else {
        dinero::g_logger.info("Set mining address: " + address + " (BlockAssembler will be updated when initialized)");
    }
}

std::string MiningManager::getMiningAddress() const {
    std::lock_guard<std::mutex> lock(address_mutex_);
    return mining_address_;
}

MiningInfo MiningManager::getMiningInfo() const {
    MiningInfo info;
    info.is_mining = is_mining_.load();
    info.thread_count = thread_count_.load();
    info.hashrate = current_hashrate_.load();
    info.mining_address = getMiningAddress();
    info.blocks_mined = blocks_mined_.load();
    info.last_block_time = last_block_time_.load();
    info.difficulty = 1.0; // Placeholder
    info.network_hashrate = 0.0; // Placeholder

    // GPU mining fields (Week 9 - GPU Mining Integration)
    #ifdef ENABLE_GPU_MINING
    info.gpu_available = (gpu_backend_ != nullptr);
    info.gpu_mining_enabled = gpu_enabled_.load();
    info.gpu_hashrate = gpu_hashrate_.load();

    if (gpu_backend_) {
        info.gpu_device_name = gpu_backend_->getDeviceName();
        // Determine backend type from device name or store it during initialization
        if (info.gpu_device_name.find("CUDA") != std::string::npos) {
            info.gpu_backend = "CUDA";
        } else {
            info.gpu_backend = "OpenCL";
        }
        info.gpu_device_count = 1;  // For now, single GPU
    } else {
        info.gpu_device_name = "None";
        info.gpu_backend = "None";
        info.gpu_device_count = 0;
    }

    // Combined hashrate (CPU + GPU)
    info.hashrate = current_hashrate_.load() + gpu_hashrate_.load();
    #else
    info.gpu_available = false;
    info.gpu_mining_enabled = false;
    info.gpu_hashrate = 0.0;
    info.gpu_device_name = "None";
    info.gpu_backend = "None";
    info.gpu_device_count = 0;
    #endif

    return info;
}

int MiningManager::getOptimalThreadCount() const {
    int hardware_threads = std::thread::hardware_concurrency();
    if (hardware_threads == 0) {
        return 1; // Fallback
    }

    // Use 50% of available CPU threads by default (leave room for system/other apps)
    // This provides good mining performance while keeping system responsive
    int optimal_threads = std::max(1, hardware_threads / 2);

    dinero::g_logger.info("CPU auto-detection: " + std::to_string(hardware_threads) +
                         " cores detected, using " + std::to_string(optimal_threads) +
                         " threads (50%) for mining");

    return optimal_threads;
}

void MiningManager::setThreadCount(int threads) {
    if (threads <= 0) {
        threads = getOptimalThreadCount();
    }
    
    bool was_mining = is_mining_.load();
    
    if (was_mining) {
        stopMining();
    }
    
    thread_count_.store(threads);
    
    if (was_mining) {
        startMining(threads);
    }
}

void MiningManager::miningLoop(int thread_id) {
    dinero::g_logger.debug("Mining thread " + std::to_string(thread_id) + " started");

    // Week 9.5 - Cooperative CPU+GPU Mining: CPU gets limited nonce range [0, 10M]
    // GPU gets [10M+1, UINT32_MAX] to avoid duplicate work
    const uint32_t CPU_NONCE_RANGE = 10'000'000;  // CPU limited to 10M nonces

    uint64_t thread_hashes = 0;
    // Divide CPU nonce range among threads (not full UINT32_MAX)
    uint32_t nonce_start = thread_id * (CPU_NONCE_RANGE / thread_count_.load());
    uint32_t nonce_end = (thread_id + 1) * (CPU_NONCE_RANGE / thread_count_.load());
    uint32_t nonce = nonce_start;

    if (thread_id == 0) {
        dinero::g_logger.info("[CPU] CPU nonce range: [0, " + std::to_string(CPU_NONCE_RANGE) +
                             "] (disjoint from GPU, divided among " + std::to_string(thread_count_.load()) + " threads)");
    }

    while (!should_stop_.load()) {
        // Get current mining job
        std::shared_ptr<MiningJob> job;
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            job = current_job_;
        }

        if (!job || job->stop_mining.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Try mining with current nonce
        if (tryNonce(job, nonce, thread_id)) {
            // Block found!
            onBlockFound(job, nonce);
            break;
        }

        thread_hashes++;
        nonce++;

        // Wrap around nonce space for this thread (within CPU range only)
        if (nonce >= nonce_end) {
            nonce = nonce_start;

            // Check if job needs refresh
            if (block_assembler_ && block_assembler_->ShouldRefreshJob(job)) {
                refreshJob();
            }
        }

        // Update global hash counter periodically
        if (thread_hashes % 10000 == 0) {
            total_hashes_.fetch_add(10000);
            updateHashrate();
        }
    }

    // Add remaining hashes to global counter
    total_hashes_.fetch_add(thread_hashes % 10000);

    dinero::g_logger.debug("Mining thread " + std::to_string(thread_id) + " stopped");
}

// GPU Mining Loop (Week 9 - GPU Mining Integration)
// Week 9.5 - Cooperative CPU+GPU Mining: GPU gets disjoint nonce range
#ifdef ENABLE_GPU_MINING
void MiningManager::gpuMiningLoop() {
    dinero::g_logger.info("[GPU] GPU mining thread started (cooperative mode)");

    // Define disjoint nonce ranges (per COOPERATIVE_MINING_DESIGN.md)
    const uint32_t CPU_NONCE_RANGE = 10'000'000;  // CPU gets [0, 10M]
    const uint32_t GPU_NONCE_START = CPU_NONCE_RANGE + 1;  // GPU gets [10M+1, UINT32_MAX]
    const uint32_t GPU_BATCH_SIZE = 1048576;  // 1M hashes per batch

    dinero::g_logger.info("[GPU] GPU nonce range: [" + std::to_string(GPU_NONCE_START) +
                         ", " + std::to_string(UINT32_MAX) + "] (disjoint from CPU)");

    uint32_t current_nonce = GPU_NONCE_START;

    while (!should_stop_.load() && gpu_enabled_.load()) {
        // Get current mining job
        std::shared_ptr<MiningJob> job;
        {
            std::lock_guard<std::mutex> lock(job_mutex_);
            job = current_job_;
        }

        if (!job || job->stop_mining.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            current_nonce = GPU_NONCE_START;  // Reset nonce on new job
            continue;
        }

        // Prepare work package for GPU with disjoint nonce range
        dinero::gpu::WorkPackage work;
        std::memcpy(work.header, &job->header, 128);  // BlockHeader v1 is 128 bytes

        // Convert target to GPU format (simplified)
        std::memcpy(work.target, job->target_hex.c_str(), std::min(size_t(32), job->target_hex.length()));

        // Set disjoint nonce range for GPU (no overlap with CPU)
        work.nonce_start = current_nonce;
        work.nonce_end = std::min(current_nonce + GPU_BATCH_SIZE - 1, static_cast<uint32_t>(UINT32_MAX));
        work.backend = dinero::gpu::BackendType::CUDA;  // Will be set by backend
        work.device_id = 0;  // Single GPU for now

        // Execute GPU mining
        dinero::gpu::MiningResult result;
        if (gpu_backend_->mine(work, result)) {
            // Update GPU hashrate
            gpu_hashrate_.store(gpu_backend_->getHashrate());

            // Check if block found
            if (result.found) {
                dinero::g_logger.info("[GPU] Block found! Nonce: 0x" + std::to_string(result.nonce) +
                                     " (from GPU backend)");
                onBlockFound(job, result.nonce);
                current_nonce = GPU_NONCE_START;  // Reset for next job
                continue;  // Get new job
            }
        }

        // Advance nonce for next batch
        current_nonce += GPU_BATCH_SIZE;

        // Wrap around GPU nonce space
        if (current_nonce < GPU_NONCE_START || current_nonce >= UINT32_MAX - GPU_BATCH_SIZE) {
            current_nonce = GPU_NONCE_START;

            // Check if job needs refresh
            if (block_assembler_ && block_assembler_->ShouldRefreshJob(job)) {
                refreshJob();
            }
        }

        // Small delay to prevent GPU overheating
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    dinero::g_logger.info("[GPU] GPU mining thread stopped");
}
#endif

void MiningManager::updateHashrate() {
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    auto last_time = last_hashrate_time_.load();
    if (now - last_time >= 1000) { // Update every second
        uint64_t total_hashes = total_hashes_.load();
        double elapsed_seconds = (now - last_time) / 1000.0;
        double hashrate = total_hashes / elapsed_seconds;
        
        current_hashrate_.store(hashrate);
        last_hashrate_time_.store(now);
        total_hashes_.store(0); // Reset counter
    }
}

// Block template management
std::shared_ptr<MiningJob> MiningManager::getCurrentJob() const {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return current_job_;
}

void MiningManager::refreshJob() {
    if (!block_assembler_) {
        return;
    }
    
    auto new_job = block_assembler_->CreateJob();
    if (!new_job) {
        dinero::g_logger.error("Failed to create new mining job");
        return;
    }
    
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (current_job_) {
        current_job_->stop_mining.store(true);
    }
    current_job_ = new_job;
    
    dinero::g_logger.debug("Refreshed mining job: " + new_job->job_id);
}

bool MiningManager::submitBlock(const Block& block) {
    // Block submission is handled by BlockAcceptor via ChainDB
    // No need for blockchain_ check here
    
    // Submit block to blockchain
    try {
        // This would call blockchain->ProcessNewBlock(block)
        // Phase M.0: Convert uint256 → hex for logging
        dinero::g_logger.info("Submitted block with hash: " + block.GetHash().GetHex());
        
        blocks_mined_.fetch_add(1);
        last_block_time_.store(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        // Refresh job after successful block submission
        refreshJob();
        
        return true;
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to submit block: " + std::string(e.what()));
        return false;
    }
}


void MiningManager::setChainDB(ChainDB* chain_db) {
    chain_db_ = chain_db;
    // Recreate BlockAssembler if we have both ChainDB and ChainManager
    if (chain_db_ && chain_manager_ && !block_assembler_) {
        // Phase 39: BlockAssembler constructor updated (ChainManager parameter removed)
        block_assembler_ = std::make_unique<BlockAssembler>(chain_db_);
        // CRITICAL: Set mining address if it was already configured
        if (!mining_address_.empty()) {
            block_assembler_->SetMiningAddress(mining_address_);
            dinero::g_logger.info("MiningManager: Propagated mining address to new BlockAssembler: " + mining_address_);
        }
    } else if (chain_db_ && block_assembler_) {
        // BlockAssembler already exists - update ChainDB
        block_assembler_->setChainDB(chain_db_);
        dinero::g_logger.info("MiningManager: ChainDB updated for BlockAssembler");
    }
}

void MiningManager::setChainManager(ChainManager* chain_manager) {
    chain_manager_ = chain_manager;
    // Recreate BlockAssembler if we have both ChainDB and ChainManager
    if (chain_db_ && chain_manager_ && !block_assembler_) {
        // Phase 39: BlockAssembler constructor updated (ChainManager parameter removed)
        block_assembler_ = std::make_unique<BlockAssembler>(chain_db_);
        // CRITICAL: Set mining address if it was already configured
        if (!mining_address_.empty()) {
            block_assembler_->SetMiningAddress(mining_address_);
            dinero::g_logger.info("MiningManager: Propagated mining address to new BlockAssembler: " + mining_address_);
        }
    }
    dinero::g_logger.info("MiningManager: ChainManager set for activation layer");
}

void MiningManager::setMempool(Mempool* mempool) {
    mempool_ = mempool;
    // Update BlockAssembler if it exists
    if (block_assembler_) {
        block_assembler_->setMempool(mempool_);
        dinero::g_logger.info("MiningManager: Mempool set for BlockAssembler");
    }
}


// Block mining helpers
bool MiningManager::tryNonce(std::shared_ptr<MiningJob> job, uint32_t nonce, uint32_t thread_id) {
    if (!job || job->stop_mining.load()) {
        return false;
    }

    // Create block header with current nonce
    BlockHeader header = job->header;
    header.nonce = nonce;

    // Calculate block hash
    std::string hash = calculateBlockHash(header);

    // [HASHCHK] logging every 100K hashes on thread 0
    static std::atomic<uint64_t> hashchk_counter{0};
    if (thread_id == 0 && (hashchk_counter.fetch_add(1) % 100000 == 0)) {
        std::ostringstream log;
        log << "[HASHCHK] nonce=0x" << std::hex << std::setfill('0') << std::setw(8) << header.nonce
            << " hash=" << hash.substr(0, 16) << "..."
            << " target=" << job->target_hex.substr(0, 16) << "..."
            << " bits=0x" << std::hex << std::setw(8) << header.bits
            << " time=" << std::dec << header.timestamp;
        dinero::g_logger.info(log.str());
    }

    // Check if hash meets target
    return meetsTarget(hash, job->target_hex);
}

std::string MiningManager::calculateBlockHash(const BlockHeader& header) const {
    // Use proper double SHA-256 (Bitcoin-style proof-of-work)
    // Phase M.0: Convert uint256 → hex for return value
    return header.GetHash().GetHex();
}

bool MiningManager::meetsTarget(const std::string& hash, const std::string& target) const {
    // Numerical comparison of 256-bit hashes (not lexicographic)
    // Both hash and target are hex strings representing 256-bit numbers
    // We need to compare them as big-endian unsigned integers

    // Ensure both strings are 64 characters (32 bytes = 256 bits)
    if (hash.length() != 64 || target.length() != 64) {
        return false;
    }

    // Compare hex strings as 256-bit numbers by comparing byte-by-byte
    // from most significant to least significant
    for (size_t i = 0; i < 64; i += 2) {
        // Extract 2-character hex byte
        std::string hash_byte = hash.substr(i, 2);
        std::string target_byte = target.substr(i, 2);

        // Convert to integers for comparison
        unsigned int hash_val = std::stoul(hash_byte, nullptr, 16);
        unsigned int target_val = std::stoul(target_byte, nullptr, 16);

        if (hash_val < target_val) {
            return true;  // hash < target
        } else if (hash_val > target_val) {
            return false; // hash > target
        }
        // If equal, continue to next byte
    }

    // All bytes equal, hash == target, which counts as meeting target
    return true;
}

void MiningManager::onBlockFound(std::shared_ptr<MiningJob> job, uint32_t winning_nonce) {
    // Week 9.5 - Cooperative CPU+GPU Mining: Thread-safe solution callback
    // Protect against race conditions when both CPU and GPU find blocks simultaneously
    std::lock_guard<std::mutex> lock(job_mutex_);

    // Check if block was already found by another backend
    if (job->stop_mining.load()) {
        dinero::g_logger.info("Block already found by another backend (nonce: " +
                             std::to_string(winning_nonce) + " discarded)");
        return;
    }

    dinero::g_logger.info("Block found! Nonce: " + std::to_string(winning_nonce) +
                         ", Job: " + job->job_id);

    // Stop all mining on this job (signals both CPU and GPU to stop)
    job->stop_mining.store(true);

    // Create complete block
    Block block;
    block.header = job->header;
    block.header.nonce = winning_nonce;
    block.vtx = job->transactions;

    // Submit block
    if (submitBlock(block)) {
        dinero::g_logger.info("Successfully submitted block at height " +
                             std::to_string(job->height));
    } else {
        dinero::g_logger.error("Failed to submit found block");
    }
}

} // namespace dinero
