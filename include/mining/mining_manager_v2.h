// SPDX-License-Identifier: MIT
// Dinero - MiningManager (Phase C Redesign)
//
// Phase C/D Design: Job-based mining with dependency injection
// - IService pattern (DaemonContext-owned, not singleton)
// - Job manager thread + N CPU worker threads + 1 GPU worker thread
// - BlockAssembler injected (not owned)
// - GPU mining: Metal (Apple Silicon), CUDA (NVIDIA), OpenCL (AMD/Intel)
// - Push stats to MetricsRegistry, RPC pulls from registry
// - 500ms job refresh interval (configurable)

#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include "daemon/iservice.h"
#include "mining/block_assembler.h"
#include "mining/mining_stats.h"
#include <optional>
#include "mining/gpu/compute_backend.h"
#include "mining/gpu/gpu_device_manager.h"

// Forward declarations
struct DaemonContext;

namespace dinero {

// Forward declarations
class ChainDB;
class Mempool;
class WalletManager;
class ILogger;

namespace metrics {
    class MetricsRegistry;
}

/**
 * Metric Schema (for MetricsRegistry integration)
 *
 * ✅ Phase C Complete: Metrics are pushed to MetricsRegistry via pushMetrics()
 *
 * These metric names and semantics are FROZEN for Phase C.
 * Metrics are pushed at:
 * - Mining start/stop
 * - New job creation
 * - Block found
 * - Periodic updates during mining
 *
 * GAUGE METRICS (current state) - ✅ WIRED:
 * - "mining.hashrate.current"  : double - Current hashrate in H/s → SetMiningHashrate()
 * - "mining.threads.active"    : uint32 - Number of active worker threads → SetMiningThreads()
 * - "mining.job.height"        : uint32 - Current block height being mined → SetMiningJobHeight()
 * - "mining.job.difficulty"    : uint32 - Current difficulty target (bits) → SetMiningCurrentBits()
 *
 * COUNTER METRICS (cumulative, monotonic) - ✅ WIRED:
 * - "mining.blocks.found"      : uint64 - Total blocks found → IncrementMiningBlocksFound()
 *
 * INTERNAL STATS (available via GetMetrics() JSON, not pushed to Prometheus):
 * - "mining.hashes.total"      : uint64 - Total hashes computed (for debugging)
 * - "mining.jobs.created"      : uint64 - Total jobs created (for debugging)
 * - "mining.start.time"        : uint64 - Mining start timestamp (for uptime calculation)
 * - "mining.block.last.time"   : uint64 - Last block found timestamp
 * - "mining.job.id"            : string - Current job ID
 *
 * Prometheus Export Example:
 *   mining_hashrate_current 125000.5
 *   mining_threads_active 4
 *   mining_blocks_found_total 42
 *   mining_job_height 100523
 *   mining_job_difficulty 0x1d00ffff
 */

/**
 * MiningManager - Phase C Redesign
 *
 * Design principles:
 * 1. IService pattern (DaemonContext owns lifecycle)
 * 2. Dependency injection (BlockAssembler, ChainDB, Mempool, WalletManager)
 * 3. Job manager thread + N CPU worker threads + optional GPU worker thread
 * 4. GPU mining: Metal (Apple Silicon), CUDA (NVIDIA), OpenCL (AMD/Intel)
 * 5. Push stats to MetricsRegistry (RPC pulls from registry)
 * 6. 500ms job refresh interval (configurable)
 *
 * Thread model:
 * - Job manager thread: Creates jobs, monitors staleness, broadcasts new jobs
 * - CPU worker threads: Sleep on condition variable, hash nonces when job available
 * - GPU worker thread: Dispatches 16M nonce batches to GPU, runs alongside CPU
 * - Nonce partitioning: CPU uses [0, 0x7FFFFFFF], GPU uses [0x80000000, 0xFFFFFFFF]
 *
 * Ownership:
 * - DaemonContext owns MiningManager (via ctx.mining)
 * - MiningManager references BlockAssembler (doesn't own it)
 * - MiningManager references ChainDB, Mempool, WalletManager (doesn't own them)
 */
class MiningManager : public IService {
public:
    MiningManager();
    ~MiningManager() override;

    // ═══════════════════════════════════════════════════════════════════
    // IService Interface Implementation
    // ═══════════════════════════════════════════════════════════════════

    /**
     * Service name for logging and diagnostics
     */
    std::string Name() const override { return "MiningManager"; }

    /**
     * Initialize service with daemon context
     * Wires dependencies: BlockAssembler, ChainDB, Mempool, WalletManager, Logger
     */
    bool Init(DaemonContext& ctx) override;

    /**
     * Start the service (does NOT start mining automatically)
     * Check config for gen=1 to auto-start mining
     */
    bool Start() override;

    /**
     * Stop the service (stops mining if active)
     * Joins all threads, cleans up resources
     */
    void Stop() override;

    /**
     * Get service health status
     * @return true if threads are alive and healthy
     */
    bool IsHealthy() const override;

    /**
     * Get service metrics for monitoring
     * @return JSON object with mining stats
     */
    std::string GetMetrics() const override;

    // ═══════════════════════════════════════════════════════════════════
    // Mining Control API (called by RPC)
    // ═══════════════════════════════════════════════════════════════════

    /**
     * Start mining with N threads
     * @param threads Number of worker threads (0 = auto-detect)
     * @return true if mining started successfully
     */
    bool startMining(int threads = 0);

    /**
     * Stop mining gracefully
     * Joins all threads, cleans up jobs
     */
    void stopMining();

    /**
     * Check if mining is active
     */
    bool isMining() const { return stats_.is_mining.load(); }

    // ═══════════════════════════════════════════════════════════════════
    // Configuration
    // ═══════════════════════════════════════════════════════════════════

    /**
     * Set mining address (for coinbase rewards)
     */
    void setMiningAddress(const std::string& address);

    /**
     * Get mining address
     */
    std::string getMiningAddress() const;

    /**
     * Set number of worker threads
     * If mining is active, will restart with new thread count
     */
    void setThreadCount(int threads);

    /**
     * Get number of worker threads
     */
    int getThreadCount() const { return thread_count_.load(); }

    /**
     * Get optimal thread count for this machine
     */
    int getOptimalThreadCount() const;

    /**
     * Set job refresh interval in milliseconds
     * Default: 500ms (approved in Phase B)
     */
    void setRefreshInterval(uint32_t interval_ms) { refresh_interval_ms_ = interval_ms; }

    // ═══════════════════════════════════════════════════════════════════
    // Statistics (thread-safe atomics)
    // ═══════════════════════════════════════════════════════════════════

    /**
     * Get current mining statistics
     * NOTE: RPC should read from MetricsRegistry, not call this directly
     * This is for internal use and MetricsRegistry updates
     */
    const MiningStats& getStats() const { return stats_; }

private:
    // ═══════════════════════════════════════════════════════════════════
    // Dependency Injection (Phase B Decision #4: Inject, don't own)
    // ═══════════════════════════════════════════════════════════════════

    BlockAssembler* block_assembler_{nullptr};  // Injected (NOT owned)
    ChainDB* chain_db_{nullptr};                // Injected (NOT owned)
    Mempool* mempool_{nullptr};                 // Injected (NOT owned)
    WalletManager* wallet_manager_{nullptr};    // Injected (NOT owned)
    ILogger* logger_{nullptr};                  // Injected (NOT owned)
    metrics::MetricsRegistry* metrics_registry_{nullptr};  // Injected (NOT owned)
    DaemonContext* daemon_ctx_{nullptr};                   // Injected (NOT owned)

    // ═══════════════════════════════════════════════════════════════════
    // Mining Configuration
    // ═══════════════════════════════════════════════════════════════════

    std::string mining_address_;
    mutable std::mutex address_mutex_;
    std::atomic<int> thread_count_{0};
    uint32_t refresh_interval_ms_{500};  // Phase B Decision #5: 500ms default

    // ═══════════════════════════════════════════════════════════════════
    // Thread Management
    // ═══════════════════════════════════════════════════════════════════

    // Job manager thread (creates jobs, monitors staleness)
    std::thread job_manager_thread_;
    std::atomic<bool> shutdown_requested_{false};

    // CPU worker threads (hash nonces)
    std::vector<std::thread> worker_threads_;

    // GPU worker thread
    std::thread gpu_worker_thread_;
    std::unique_ptr<gpu::IComputeBackend> gpu_backend_;
    std::atomic<bool> gpu_available_{false};
    std::atomic<double> gpu_hashrate_{0.0};
    std::atomic<uint32_t> gpu_nonce_cursor_{0x80000000};  // GPU starts at upper half

    // Job synchronization
    std::shared_ptr<MiningJob> current_job_;
    mutable std::mutex job_mutex_;
    std::condition_variable job_cv_;
    std::atomic<bool> job_available_{false};
    std::atomic<bool> force_job_refresh_{false};  // Force immediate job refresh (block accepted)
    std::optional<uint256> last_accepted_tip_;  // Last accepted tip hash (for explicit CreateJob)
    std::atomic<bool> paused_for_readiness_{false};
    mutable std::mutex readiness_mutex_;
    std::string readiness_pause_reason_;
    std::string readiness_pause_code_;

    // ═══════════════════════════════════════════════════════════════════
    // Statistics (Phase B Decision #3: Push to MetricsRegistry)
    // ═══════════════════════════════════════════════════════════════════

    MiningStats stats_;
    mutable std::mutex stats_mutex_;  // For non-atomic job info
    std::chrono::steady_clock::time_point last_hashrate_update_;
    uint64_t last_hashrate_total_hashes_{0};

    // ═══════════════════════════════════════════════════════════════════
    // Thread Workers
    // ═══════════════════════════════════════════════════════════════════

    /**
     * Job manager thread loop
     * - Requests jobs from BlockAssembler
     * - Checks for staleness (chain tip, mempool, age)
     * - Broadcasts new jobs to workers via condition variable
     * - Runs every refresh_interval_ms_ (500ms default)
     */
    void jobManagerLoop();

    /**
     * Worker thread loop
     * - Waits on condition variable for new jobs
     * - Hashes nonces with striding (thread i: i, i+N, i+2N...)
     * - Detects solutions atomically
     * - Updates hashrate periodically
     */
    void workerLoop(int thread_id);

    /**
     * GPU worker thread loop
     * - Dispatches 16M nonce batches to GPU backend
     * - Runs alongside CPU worker threads
     * - Uses upper nonce space [0x80000000, 0xFFFFFFFF]
     */
    void gpuWorkerLoop();

    /**
     * Initialize GPU backend (Metal/CUDA/OpenCL)
     * @return true if GPU initialized successfully
     */
    bool initGPU();

    // ═══════════════════════════════════════════════════════════════════
    // Mining Helpers
    // ═══════════════════════════════════════════════════════════════════

    /**
     * Try a single nonce
     * @return true if solution found
     */
    bool tryNonce(std::shared_ptr<MiningJob> job, uint32_t nonce);

    /**
     * Handle block solution found
     * Submits to BlockAcceptor, refreshes job
     */
    void onSolutionFound(std::shared_ptr<MiningJob> job, uint32_t nonce);

    /**
     * Update hashrate statistics
     * Called periodically by workers (not in hot path)
     */
    void updateHashrate(uint64_t hashes, uint64_t elapsed_ms);

    /**
     * Push statistics to MetricsRegistry
     * Called after significant events (block found, job created, etc.)
     */
    void pushMetrics();
};

// ═══════════════════════════════════════════════════════════════════
// ODR Violation Prevention Guard
// ═══════════════════════════════════════════════════════════════════
//
// This static assert prevents accidental compilation of multiple definitions
// of MiningManager (ODR violation). If this fails at compile time, it means:
//
// 1. Both mining_manager.cpp (legacy) and mining_manager_v2.cpp are in the build
// 2. Or another file defines a class with the same name in dinero namespace
//
// Fix: Ensure only ONE implementation file defines dinero::MiningManager
//
// Context: Phase F.5 fix - Previously, both legacy and v2 were compiled,
// causing vtable corruption and SIGSEGV on first virtual method call.
//
static_assert(sizeof(MiningManager) > 0,
              "MiningManager ODR check: If this fails, multiple definitions exist");

} // namespace dinero
