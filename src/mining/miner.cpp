#include "mining/miner.h"
#include "consensus/subsidy.h"   // For ConsensusSubsidy::UNA_PER_DIN
#include "mining/block_assembler.h"
#include "mining/gpu/compute_backend.h"
#include "mining/gpu/gpu_device_manager.h"
#include "common/logger.h"
#include "common/sha256d.h"
#include "storage/chain_db.h"  // Week 5: ChainDB for context injection
#include "daemon/ws_bus.hpp"
#include "dinero/daemon/block_acceptor.h"  // BlockAcceptor - single authority for block acceptance
#include "mining/header_layout.h"  // DINERO_HEADER_SIZE_BYTES
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <random>
#include "compat/jsoncpp_compat.h"

namespace dinero {

// Phase 39: Constructor simplified - ChainManager removed (ChainManager deleted)
Miner::Miner(ChainDB* chain_db)
    : chain_db_(chain_db) {
    if (!chain_db_) {
        throw std::runtime_error("Miner: ChainDB cannot be null");
    }

    // Initialize BlockAssembler immediately (no longer needs ChainManager)
    block_assembler_ = std::make_unique<BlockAssembler>(chain_db_);

    // Initialize timing
    last_hashrate_report_ = std::chrono::steady_clock::now();
    last_job_refresh_ = std::chrono::steady_clock::now();

    dinero::g_logger.info("Miner initialized with BlockAssembler");
}

// Phase 39: SetChainManager method removed (ChainManager deleted)

Miner::~Miner() {
    StopMining();
}

bool Miner::StartMining(uint32_t num_threads, const std::string& mining_address) {
    if (mining_enabled_.load()) {
        dinero::g_logger.warning("Mining already active");
        return false;
    }
    
    if (num_threads == 0) {
        dinero::g_logger.error("Invalid thread count: " + std::to_string(num_threads));
        return false;
    }
    
    if (mining_address.empty()) {
        dinero::g_logger.error("Mining address not set");
        return false;
    }
    
    mining_address_ = mining_address;
    num_threads_ = num_threads;
    
    // Reset state
    shutdown_requested_.store(false);
    next_nonce_start_.store(0);
    job_available_.store(false);
    job_refresh_needed_.store(false);
    
    // Reset statistics
    stats_.total_hashes.store(0);
    stats_.blocks_found.store(0);
    stats_.jobs_processed.store(0);
    stats_.current_hashrate.store(0.0);
    stats_.active_threads.store(0);
    stats_.mining_start_time.store(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    
    // Create initial mining job
    current_job_ = CreateNewJob();
    if (!current_job_) {
        dinero::g_logger.error("Failed to create initial mining job");
        return false;
    }
    
    job_available_.store(true);
    mining_enabled_.store(true);
    stats_.is_mining.store(true);
    
    // Start job manager thread
    job_manager_thread_ = std::thread(&Miner::JobManagerThread, this);
    
    // Detect and initialize GPU mining
    if (InitGPU()) {
        gpu_mining_thread_ = std::thread(&Miner::GPUMinerThread, this);
        dinero::g_logger.info("GPU mining thread started");
    }

    // Start CPU mining threads
    mining_threads_.reserve(num_threads);
    for (uint32_t i = 0; i < num_threads; ++i) {
        mining_threads_.emplace_back(&Miner::MinerThread, this, i);
    }

    dinero::g_logger.info("Mining started with " + std::to_string(num_threads) + " CPU threads" +
                         (gpu_available_.load() ? " + GPU" : ""));
    dinero::g_logger.info("   Mining address: " + mining_address);
    dinero::g_logger.info("   Current height: " + std::to_string(current_job_->height));
    dinero::g_logger.info("   Difficulty: 0x" + std::to_string(current_job_->target_bits));
    dinero::g_logger.info("   Block reward: " + std::to_string(current_job_->block_reward / ConsensusSubsidy::UNA_PER_DIN) + " DIN");
    
    // Broadcast mining start event
    BroadcastMiningStats();
    
    return true;
}

void Miner::StopMining() {
    if (!mining_enabled_.load()) {
        return;
    }
    
    dinero::g_logger.info("🛑 Stopping mining...");
    
    // Signal shutdown
    shutdown_requested_.store(true);
    mining_enabled_.store(false);
    stats_.is_mining.store(false);
    
    // Stop current job
    if (current_job_) {
        current_job_->stop_mining.store(true);
    }
    
    // Wake up job condition
    job_condition_.notify_all();
    
    // Join job manager thread
    if (job_manager_thread_.joinable()) {
        job_manager_thread_.join();
    }
    
    // Join GPU mining thread
    if (gpu_mining_thread_.joinable()) {
        gpu_mining_thread_.join();
    }
    if (gpu_backend_) {
        gpu_backend_->stop();
        gpu_backend_.reset();
        gpu_available_.store(false);
    }

    // Join CPU mining threads
    for (auto& thread : mining_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    mining_threads_.clear();
    stats_.active_threads.store(0);
    
    dinero::g_logger.info("Mining stopped");
    
    // Broadcast mining stop event
    BroadcastMiningStats();
}

MiningStats& Miner::GetStats() const {
    // Return a reference to the stats (they're thread-safe with atomics)
    return const_cast<MiningStats&>(stats_);
}

std::string Miner::GetMiningInfo() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    std::ostringstream oss;
    
    oss << "Mining Information:\n";
    oss << "  Status: " << (stats_.is_mining.load() ? "Active" : "Stopped") << "\n";
    oss << "  Threads: " << stats_.active_threads.load() << "\n";
    oss << "  Address: " << mining_address_ << "\n";
    oss << "  Hashrate: " << std::fixed << std::setprecision(2) << stats_.current_hashrate.load() << " H/s\n";
    oss << "  Total Hashes: " << stats_.total_hashes.load() << "\n";
    oss << "  Blocks Found: " << stats_.blocks_found.load() << "\n";
    oss << "  Jobs Processed: " << stats_.jobs_processed.load() << "\n";
    
    if (current_job_) {
        oss << "  Current Height: " << current_job_->height << "\n";
        oss << "  Current Difficulty: 0x" << std::hex << current_job_->target_bits << std::dec << "\n";
        oss << "  Block Reward: " << std::fixed << std::setprecision(6) 
            << (static_cast<double>(current_job_->block_reward) / ConsensusSubsidy::UNA_PER_DIN) << " DIN\n";
        oss << "  Job ID: " << current_job_->job_id << "\n";
    }
    
    // Mining uptime
    if (stats_.mining_start_time.load() > 0) {
        uint64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t uptime = current_time - stats_.mining_start_time.load();
        
        uint64_t hours = uptime / 3600;
        uint64_t minutes = (uptime % 3600) / 60;
        uint64_t seconds = uptime % 60;
        
        oss << "  Uptime: " << hours << "h " << minutes << "m " << seconds << "s\n";
    }
    
    return oss.str();
}

void Miner::SetMiningAddress(const std::string& address) {
    mining_address_ = address;
    
    // Set mining address on block assembler
    if (block_assembler_) {
        block_assembler_->SetMiningAddress(address);
    }
    
    // Refresh job if mining is active
    if (mining_enabled_.load()) {
        RefreshMiningJob();
    }
    
    dinero::g_logger.info("Mining address updated: " + address);
}

void Miner::RefreshMiningJob() {
    job_refresh_needed_.store(true);
    job_condition_.notify_all();
}

// Private methods

void Miner::MinerThread(uint32_t thread_id) {
    dinero::g_logger.debug("Mining thread " + std::to_string(thread_id) + " started");
    
    stats_.active_threads.fetch_add(1);
    
    auto last_hashrate_update = std::chrono::steady_clock::now();
    uint64_t thread_hashes = 0;
    
    while (!shutdown_requested_.load()) {
        // Wait for job
        std::unique_lock<std::mutex> lock(job_mutex_);
        job_condition_.wait(lock, [this] { 
            return job_available_.load() || shutdown_requested_.load(); 
        });
        
        if (shutdown_requested_.load()) {
            break;
        }
        
        auto job = current_job_;
        lock.unlock();
        
        if (!job || job->is_stale || job->stop_mining.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        // Get nonce range
        uint32_t start_nonce, end_nonce;
        if (!GetNextNonceRange(start_nonce, end_nonce)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        
        // Mine the range
        bool found_solution = MineNonceRange(job, start_nonce, end_nonce, thread_id);
        
        if (found_solution) {
            dinero::g_logger.info("🎯 Thread " + std::to_string(thread_id) + " found solution!");
            stats_.blocks_found.fetch_add(1);
            
            // Broadcast block found event
            BroadcastMiningStats();
        }
        
        // Update thread hash count
        thread_hashes += (end_nonce - start_nonce);
        
        // Update hashrate periodically
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hashrate_update);
        
        if (elapsed.count() >= 1000) {  // Update every second
            UpdateHashrate(thread_hashes, elapsed.count());
            thread_hashes = 0;
            last_hashrate_update = now;
        }
    }
    
    stats_.active_threads.fetch_sub(1);
    dinero::g_logger.debug("Mining thread " + std::to_string(thread_id) + " stopped");
}

bool Miner::MineNonceRange(std::shared_ptr<MiningJob> job, uint32_t start_nonce, 
                          uint32_t end_nonce, uint32_t thread_id) {
    if (!job || job->is_stale || job->stop_mining.load()) {
        return false;
    }
    
    // Create a copy of the header for this thread
    BlockHeader header = job->header;
    
    for (uint32_t nonce = start_nonce; nonce < end_nonce; ++nonce) {
        if (shutdown_requested_.load() || job->stop_mining.load()) {
            break;
        }
        
        header.nonce = nonce;
        
        // Calculate hash
        std::string hash = DineroPoW::CalculateBlockHash(header);
        
        // Check if hash meets target
        if (DineroPoW::MeetsTarget(hash, job->target_bits)) {
            dinero::g_logger.info("🎯 SOLUTION FOUND!");
            dinero::g_logger.info("   Thread: " + std::to_string(thread_id));
            dinero::g_logger.info("   Nonce: 0x" + std::to_string(nonce));
            dinero::g_logger.info("   Hash: " + hash);
            dinero::g_logger.info("   Target: " + job->target_hex);
            
            // Submit the block
            return SubmitBlock(job, nonce, thread_id);
        }
        
        stats_.total_hashes.fetch_add(1);
    }
    
    return false;
}

bool Miner::CheckHashTarget(const std::string& hash, const std::string& target) const {
    return DineroPoW::MeetsTarget(hash, 0);  // Use DineroPoW implementation
}

// ============================================================================
// CONSENSUS INVARIANT:
// Miner must NEVER modify chain state directly.
// BlockAcceptor is the sole authority for block acceptance.
//
// This function:
//   1. Finds valid nonce (CPU mining)
//   2. Serializes block to hex
//   3. Submits to BlockAcceptor (single authority)
//   4. Returns true ONLY if block is validated AND persisted
//
// Violation of this invariant creates consensus bugs:
//   ❌ Returning true without BlockAcceptor acceptance = consensus illusion
//   ❌ Direct ChainDB writes = bypassing validation
//   ❌ Phantom chain progress = corrupted ASERT difficulty
//
// If a block is not validated and persisted, it is NOT mined.
// ============================================================================
bool Miner::SubmitBlock(std::shared_ptr<MiningJob> job, uint32_t nonce, uint32_t thread_id) {
    if (!job || !chain_db_) {
        return false;
    }

    // Update job with winning nonce
    job->header.nonce = nonce;

    // Create block structure
    Block block;
    block.header = job->header;
    block.vtx = job->transactions;
    
    dinero::g_logger.info("Submitting block to BlockAcceptor...");
    dinero::g_logger.info("   Height: " + std::to_string(job->height));
    dinero::g_logger.info("   Transactions: " + std::to_string(block.vtx.size()));
    dinero::g_logger.info("   Reward: " + std::to_string(job->block_reward / ConsensusSubsidy::UNA_PER_DIN) + " DIN");

    // Serialize block to binary
    std::string block_binary = block.Serialize();

    // Convert binary to hex
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned char b : block_binary) {
        hex << std::setw(2) << static_cast<int>(b);
    }

    // Submit to BlockAcceptor (single authority for block validation and acceptance)
    // This performs:
    //   - PoW validation
    //   - Merkle root validation
    //   - Transaction validation
    //   - UTXO connection
    //   - ChainDB persistence
    //   - Chain tip update
    auto accept_result = dinero::BlockAcceptor::AcceptBlockFromRPC(hex.str(), "miner");

    if (accept_result.rejected()) {
        dinero::g_logger.error("❌ Block rejected: " + accept_result.reason +
                              " (code: " + std::string(dinero::BlockRejectCodeToString(accept_result.code)) + ")");

        // Stop mining this job (it's invalid)
        job->stop_mining.store(true);

        // Refresh job to get valid template
        RefreshMiningJob();

        return false;
    }

    // Block accepted successfully!
    std::string hash_hex = accept_result.block_hash.GetHex();
    dinero::g_logger.info("🎉 Block accepted at height " + std::to_string(accept_result.height) +
                         " (hash: " + hash_hex.substr(0, 16) + "...)");

    // Update mining stats
    stats_.last_block_time.store(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());

    // Stop current job and request new one (chain tip has advanced)
    job->stop_mining.store(true);
    RefreshMiningJob();

    return true;
}

bool Miner::GetNextNonceRange(uint32_t& start_nonce, uint32_t& end_nonce) {
    // CONSENSUS FIX: Use proper thread striding - each thread gets every Nth nonce
    static thread_local uint32_t thread_id = 0;
    static thread_local uint32_t base_nonce = 0;
    static thread_local bool thread_initialized = false;
    
    if (!thread_initialized) {
        // Initialize each thread with a unique ID
        std::lock_guard<std::mutex> lock(nonce_mutex_);
        static uint32_t global_thread_counter = 0;
        thread_id = global_thread_counter++;
        base_nonce = 0; // Start from 0 for all threads
        thread_initialized = true;
        
        dinero::g_logger.debug("Thread " + std::to_string(thread_id) + " initialized (will test nonces " + std::to_string(thread_id) + ", " + std::to_string(thread_id + num_threads_) + ", " + std::to_string(thread_id + 2*num_threads_) + ", ...)");
    }
    
    // Each thread gets a range starting from its current position
    start_nonce = base_nonce + thread_id;
    end_nonce = start_nonce + (nonce_range_size_ * num_threads_); // Ensure we don't overlap
    
    // Advance base for next batch
    base_nonce += (nonce_range_size_ * num_threads_);
    
    // Handle overflow
    if (base_nonce > (UINT32_MAX - (nonce_range_size_ * num_threads_))) {
        base_nonce = 0; // Wrap around
    }
    
    return true;
}

void Miner::UpdateHashrate(uint64_t hashes, uint64_t elapsed_ms) {
    if (elapsed_ms == 0) return;
    
    double hashrate = static_cast<double>(hashes) / (static_cast<double>(elapsed_ms) / 1000.0);
    
    // Exponential moving average for smoother hashrate
    double current_rate = stats_.current_hashrate.load();
    double alpha = 0.1;  // Smoothing factor
    double new_rate = (current_rate == 0.0) ? hashrate : (alpha * hashrate + (1.0 - alpha) * current_rate);
    
    stats_.current_hashrate.store(new_rate);
    
    // Report hashrate periodically
    auto now = std::chrono::steady_clock::now();
    auto since_last_report = std::chrono::duration_cast<std::chrono::seconds>(now - last_hashrate_report_);
    
    if (since_last_report.count() >= hashrate_report_interval_) {
        std::string gpu_info = "";
        if (gpu_available_.load()) {
            double gpu_hr = gpu_hashrate_.load();
            gpu_info = ", GPU: " + std::to_string(static_cast<uint64_t>(gpu_hr)) + " H/s";
        }
        dinero::g_logger.info("Mining: CPU " + std::to_string(static_cast<uint64_t>(new_rate)) + " H/s" +
                             gpu_info + ", " +
                             std::to_string(stats_.active_threads.load()) + " threads, " +
                             std::to_string(stats_.total_hashes.load()) + " total hashes");
        last_hashrate_report_ = now;
    }
}

std::shared_ptr<MiningJob> Miner::CreateNewJob() {
    if (!block_assembler_) {
        return nullptr;
    }
    
    // Create mining job using configured mining payout address
    auto job = block_assembler_->CreateJob();
    
    if (job) {
        stats_.jobs_processed.fetch_add(1);
        stats_.current_job_id = job->job_id;
        stats_.current_height = job->height;
        stats_.current_difficulty = job->target_bits;
        
        dinero::g_logger.info("Created new mining job: " + job->job_id + 
                             " (height=" + std::to_string(job->height) + 
                             ", difficulty=0x" + std::to_string(job->target_bits) + ")");
    }
    
    return job;
}

void Miner::BroadcastMiningStats() {
    try {
        Json::Value event;
        event["v"] = 1;
        event["topic"] = "mining";
        event["event"] = stats_.is_mining.load() ? "mining.active" : "mining.stopped";
        event["threads"] = static_cast<int>(stats_.active_threads.load());
        event["hashrate"] = stats_.current_hashrate.load();
        event["blocks_found"] = static_cast<int64_t>(stats_.blocks_found.load());
        event["total_hashes"] = static_cast<int64_t>(stats_.total_hashes.load());
        
        if (current_job_) {
            event["height"] = static_cast<int>(current_job_->height);
            event["difficulty"] = std::to_string(current_job_->target_bits);
            event["reward"] = static_cast<double>(current_job_->block_reward) / ConsensusSubsidy::UNA_PER_DIN;
        }
        
        Json::FastWriter writer;
        std::string payload = writer.write(event);
        
        // Note: WebSocket broadcasting removed - clients can poll mining stats via RPC
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Error broadcasting mining stats: " + std::string(e.what()));
    }
}

void Miner::JobManagerThread() {
    dinero::g_logger.debug("Job manager thread started");
    
    while (!shutdown_requested_.load()) {
        auto now = std::chrono::steady_clock::now();
        
        // Check if job refresh is needed
        bool needs_refresh = job_refresh_needed_.load();
        auto since_last_refresh = std::chrono::duration_cast<std::chrono::seconds>(now - last_job_refresh_);
        
        if (needs_refresh || since_last_refresh.count() >= job_refresh_interval_) {
            // Create new job
            auto new_job = CreateNewJob();
            
            if (new_job) {
                std::lock_guard<std::mutex> lock(job_mutex_);
                
                // Stop old job
                if (current_job_) {
                    current_job_->stop_mining.store(true);
                }
                
                current_job_ = new_job;
                next_nonce_start_.store(0);  // Reset nonce counter
                job_available_.store(true);
                job_refresh_needed_.store(false);
                last_job_refresh_ = now;
                
                job_condition_.notify_all();
                
                dinero::g_logger.debug("Job refreshed: " + new_job->job_id);
            }
        }
        
        // Sleep for a short interval
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    dinero::g_logger.debug("Job manager thread stopped");
}

// GPU Mining Support

bool Miner::InitGPU() {
#if defined(ENABLE_CUDA) || defined(ENABLE_METAL) || defined(ENABLE_OPENCL)
    auto devices = gpu_device_manager_.detectAllDevices();
    if (devices.empty()) {
        dinero::g_logger.info("[GPU] No GPU devices found — CPU mining only");
        return false;
    }

    // Select best available backend (CUDA > Metal > OpenCL)
    auto best_backend = gpu_device_manager_.getBestAvailableBackend();
    if (best_backend == gpu::BackendType::NONE) {
        dinero::g_logger.info("[GPU] No usable GPU backend — CPU mining only");
        return false;
    }

    // Find first device matching the best backend
    gpu::GPUDevice* target_dev = nullptr;
    for (auto& dev : devices) {
        if (dev.backend == best_backend) {
            target_dev = &dev;
            break;
        }
    }

    if (!target_dev) {
        dinero::g_logger.info("[GPU] No device for " + gpu::backendToString(best_backend) + " backend");
        return false;
    }

    dinero::g_logger.info("[GPU] Found: " + target_dev->name + " (" +
                         gpu::backendToString(best_backend) + ", " +
                         std::to_string(target_dev->global_memory_mb) + " MB)");

    gpu_backend_ = gpu::createBackend(best_backend);
    if (!gpu_backend_) {
        dinero::g_logger.error("[GPU] Failed to create " + gpu::backendToString(best_backend) + " backend");
        return false;
    }

    if (!gpu_backend_->initDevice(target_dev->device_id)) {
        dinero::g_logger.error("[GPU] Failed to initialize device");
        gpu_backend_.reset();
        return false;
    }

    if (!gpu_backend_->compileKernel("")) {
        dinero::g_logger.error("[GPU] Failed to compile kernel / allocate buffers");
        gpu_backend_->stop();
        gpu_backend_.reset();
        return false;
    }

    gpu_available_.store(true);
    dinero::g_logger.info("[GPU] " + gpu::backendToString(best_backend) +
                         " mining initialized on " + target_dev->name);
    return true;
#else
    return false;
#endif
}

void Miner::GPUMinerThread() {
    dinero::g_logger.info("[GPU] Mining thread started");

    while (!shutdown_requested_.load()) {
        // Wait for job
        std::unique_lock<std::mutex> lock(job_mutex_);
        job_condition_.wait(lock, [this] {
            return job_available_.load() || shutdown_requested_.load();
        });

        if (shutdown_requested_.load()) break;

        auto job = current_job_;
        lock.unlock();

        if (!job || job->is_stale || job->stop_mining.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Get nonce range for GPU (larger batch than CPU)
        uint32_t start_nonce, end_nonce;
        {
            std::lock_guard<std::mutex> nlock(nonce_mutex_);
            start_nonce = next_nonce_start_.load();
            uint64_t next = static_cast<uint64_t>(start_nonce) + GPU_NONCE_BATCH_SIZE;
            if (next > UINT32_MAX) next = 0;
            next_nonce_start_.store(static_cast<uint32_t>(next));
        }
        end_nonce = start_nonce + GPU_NONCE_BATCH_SIZE - 1;

        // Prepare work package
        gpu::WorkPackage work;
        // Serialize the block header into the 128-byte work package
        auto header_bytes = job->header.SerializeForHash(); // std::array<uint8_t, 128>
        static_assert(sizeof(work.header) >= DINERO_HEADER_SIZE_BYTES, "WorkPackage header too small");
        memcpy(work.header, header_bytes.data(), DINERO_HEADER_SIZE_BYTES);

        // Convert compact target bits to 256-bit target for GPU comparison
        std::string target_hex = DineroPoW::BitsToTargetHex(job->target_bits);
        // Parse hex string to uint32_t array (big-endian)
        for (int i = 0; i < 8; i++) {
            std::string word_hex = target_hex.substr(i * 8, 8);
            work.target[i] = static_cast<uint32_t>(strtoul(word_hex.c_str(), nullptr, 16));
        }

        work.nonce_start = start_nonce;
        work.nonce_end = end_nonce;
        work.backend = gpu_backend_->getBackendType();

        // Launch GPU mining
        gpu::MiningResult result;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = gpu_backend_->mine(work, result);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (!ok) {
            dinero::g_logger.error("[GPU] Kernel execution error");
            continue;
        }

        // Update stats
        stats_.total_hashes.fetch_add(result.hashes_tried);
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (elapsed_us > 0) {
            gpu_hashrate_.store(static_cast<double>(result.hashes_tried) / elapsed_us * 1e6);
        }

        if (result.found) {
            dinero::g_logger.info("[GPU] SOLUTION FOUND! Nonce: 0x" +
                                ([&]{ std::ostringstream o; o << std::hex << result.nonce; return o.str(); })());
            if (SubmitBlock(job, result.nonce, 9999)) {
                stats_.blocks_found.fetch_add(1);
                BroadcastMiningStats();
            }
        }
    }

    dinero::g_logger.info("[GPU] Mining thread stopped");
}

// DineroPoW implementation

std::string DineroPoW::CalculateBlockHash(const BlockHeader& header) {
    // Use canonical BlockHeader::GetHash() which properly:
    // 1. Serializes header to 128 bytes via SerializeForHash()
    // 2. Computes double SHA-256 using crypto::CSHA256
    // 3. Returns uint256 in correct byte order
    return header.GetHash().GetHex();
}

bool DineroPoW::MeetsTarget(const std::string& hash, uint32_t target_bits) {
    std::string target_hex = BitsToTargetHex(target_bits);

    // CRITICAL FIX: hash from double_sha256() is in reversed/display format (little-endian)
    // We must reverse it to big-endian before comparing with big-endian target
    // This matches the fix in block_acceptor.cpp and MiningEngine::checkProofOfWork
    std::string hash_reversed;
    for (int i = 63; i >= 0; i -= 2) {
        hash_reversed += hash.substr(i - 1, 2);
    }

    // Compare hash with target (both as hex strings, both now in big-endian)
    return hash_reversed <= target_hex;
}

std::string DineroPoW::BitsToTargetHex(uint32_t bits) {
    // Canonical Bitcoin compact format decoding
    const uint32_t exp = bits >> 24;
    const uint32_t mant = bits & 0x00ffffff;
    
    // Create 32-byte big-endian target array
    uint8_t target[32];
    memset(target, 0, 32);
    
    if (exp <= 3) {
        uint32_t v = mant >> (8 * (3 - exp));
        for (int i = 0; i < 4; ++i) {
            target[31 - i] = (v >> (8 * i)) & 0xff;
        }
    } else {
        const int off = exp - 3;
        target[32 - off - 3] = (mant >> 16) & 0xff;
        target[32 - off - 2] = (mant >> 8) & 0xff;
        target[32 - off - 1] = mant & 0xff;
    }
    
    // Convert to hex string
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(target[i]);
    }
    
    return oss.str();
}

} // namespace dinero
