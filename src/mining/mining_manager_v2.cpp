// SPDX-License-Identifier: MIT
// Dinero - MiningManager Implementation (Phase C)

#include "mining/mining_manager_v2.h"
#include "daemon/daemon_context.h"
#include "mining/block_assembler.h"
#include "mining/mining_readiness.h"
#include "mining/header_layout.h"  // For DINERO_HEADER_SIZE_BYTES
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include "common/ilogger.h"
#include "primitives/block.h"
#include "crypto/sha256.h"
#include "consensus/chainparams.h"
#include "dinero/daemon/block_acceptor.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/services/p2p_service.h"
#include "daemon/services/wallet_service.h"
#include "daemon/services/metrics_service.h"
#include "metrics/metrics_registry.h"
#include <thread>
#include <chrono>
#include <sstream>
#include <iostream>

namespace {

// Convert compact target (nBits) to 64-char hex string (big-endian)
std::string BitsToTargetHex(uint32_t bits) {
    const uint32_t exp = bits >> 24;
    const uint32_t mant = bits & 0x00ffffff;
    uint8_t target[32];
    memset(target, 0, 32);
    if (exp <= 3) {
        uint32_t v = mant >> (8 * (3 - exp));
        for (int i = 0; i < 4; ++i)
            target[31 - i] = (v >> (8 * i)) & 0xff;
    } else {
        const int off = exp - 3;
        target[32 - off - 3] = (mant >> 16) & 0xff;
        target[32 - off - 2] = (mant >> 8) & 0xff;
        target[32 - off - 1] = mant & 0xff;
    }
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 32; i++) {
        result.push_back(hex[target[i] >> 4]);
        result.push_back(hex[target[i] & 0x0f]);
    }
    return result;
}

} // anonymous namespace

namespace dinero {

// ═══════════════════════════════════════════════════════════════════
// Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════

MiningManager::MiningManager()
    : last_hashrate_update_(std::chrono::steady_clock::now())
{
}

MiningManager::~MiningManager() {
    Stop();
}

// ═══════════════════════════════════════════════════════════════════
// IService Interface Implementation
// ═══════════════════════════════════════════════════════════════════

bool MiningManager::Init(DaemonContext& ctx) {
    daemon_ctx_ = &ctx;

    // Phase B Decision #4: Inject BlockAssembler (don't own it)
    // TODO: BlockAssembler should be created by consensus layer and added to DaemonContext
    // For Phase C, we'll create it here temporarily

    // Get ChainDB from ChainstateService (defensive: check if initialized)
    if (ctx.chainstate && ctx.chainstate.get()) {
        try {
            auto chainstate_svc = std::dynamic_pointer_cast<ChainstateService>(ctx.chainstate);
            if (chainstate_svc) {
                chain_db_ = chainstate_svc->GetChainDB();
            }
        } catch (const std::exception& e) {
            // Chainstate service exists but not yet initialized
            if (logger_) {
                logger_->error("[MiningManager] ChainDB access failed: " + std::string(e.what()));
            }
            return false;
        }
    }

    // Get Mempool from MempoolService (defensive: check if initialized)
    if (ctx.mempool) {
        try {
            mempool_ = &ctx.mempool->mempool();
        } catch (const std::exception& e) {
            // Mempool service exists but not yet initialized - this is OK during startup
            if (logger_) {
                logger_->warning("[MiningManager] Mempool not yet initialized: " + std::string(e.what()));
            }
            // Will be null, validation below will catch if required
        }
    }

    // Get WalletManager from WalletService (defensive: check if initialized)
    if (ctx.wallet) {
        try {
            wallet_manager_ = &ctx.wallet->get();
        } catch (const std::exception& e) {
            // Wallet service exists but not yet initialized - this is OK during startup
            if (logger_) {
                logger_->warning("[MiningManager] Wallet not yet initialized: " + std::string(e.what()));
            }
            // Will be null, not required for mining initialization
        }
    }

    // Get Logger
    if (ctx.mining_logger) {
        logger_ = ctx.mining_logger;
    } else if (ctx.logger_interface) {
        logger_ = ctx.logger_interface;
    }

    // Get MetricsRegistry (Phase B Decision #3: Push stats to registry)
    if (ctx.metrics) {
        // TODO: Wire up MetricsRegistry when MetricsService API is ready
        // metrics_registry_ = ctx.metrics->getRegistry();
    }

    // Validate required dependencies
    if (!chain_db_) {
        if (logger_) logger_->error("[MiningManager] Init failed: ChainDB not available");
        return false;
    }

    if (!mempool_) {
        if (logger_) logger_->error("[MiningManager] Init failed: Mempool not available");
        return false;
    }

    // Phase C: Get BlockAssembler from DaemonContext (Phase B Decision #4: Inject, don't own)
    if (ctx.block_assembler) {
        block_assembler_ = ctx.block_assembler.get();
        if (logger_) {
            logger_->info("[MiningManager] BlockAssembler injected from DaemonContext");
        }
    } else {
        if (logger_) {
            logger_->error("[MiningManager] BlockAssembler not available in DaemonContext");
        }
        return false;
    }

    if (logger_) {
        logger_->info("[MiningManager] Initialized");
    }

    return true;
}

bool MiningManager::Start() {
    if (logger_) {
        logger_->info("[MiningManager] Service started (mining inactive, use RPC to start)");
    }

    // NOTE: Don't start mining automatically
    // DaemonApp should check config for gen=1 and call startMining() if needed

    return true;
}

void MiningManager::Stop() {
    if (isMining()) {
        stopMining();
    }

    if (logger_) {
        logger_->info("[MiningManager] Service stopped");
    }
}

bool MiningManager::IsHealthy() const {
    if (!isMining()) {
        return true;  // Not mining is a valid state
    }

    // Check if job manager thread is alive
    // (Can't easily check thread health without more infrastructure)
    // For now, assume healthy if mining flag is set
    return stats_.is_mining.load();
}

std::string MiningManager::GetMetrics() const {
    // Return JSON with mining stats
    // Format for Prometheus/monitoring
    std::ostringstream oss;
    oss << "{"
        << "\"mining\":" << (stats_.is_mining.load() ? "true" : "false") << ","
        << "\"threads\":" << stats_.active_threads.load() << ","
        << "\"hashrate\":" << stats_.current_hashrate.load() << ","
        << "\"total_hashes\":" << stats_.total_hashes.load() << ","
        << "\"blocks_found\":" << stats_.blocks_found.load() << ","
        << "\"jobs_processed\":" << stats_.jobs_processed.load();

    {
        std::lock_guard lock(stats_mutex_);
        oss << ",\"current_job_id\":\"" << stats_.current_job_id << "\""
            << ",\"current_height\":" << stats_.current_height
            << ",\"current_difficulty\":" << stats_.current_difficulty;
    }

    {
        std::lock_guard lock(readiness_mutex_);
        oss << ",\"paused_for_readiness\":" << (paused_for_readiness_.load() ? "true" : "false")
            << ",\"readiness_reason\":\"" << readiness_pause_code_ << "\"";
    }

    oss << "}";
    return oss.str();
}

// ═══════════════════════════════════════════════════════════════════
// Mining Control API
// ═══════════════════════════════════════════════════════════════════

bool MiningManager::startMining(int threads) {
    if (isMining()) {
        if (logger_) logger_->warning("[MiningManager] Mining already active");
        return false;
    }

    // Validate dependencies
    if (!block_assembler_) {
        if (logger_) logger_->error("[MiningManager] Cannot start: BlockAssembler not injected");
        return false;
    }

    if (daemon_ctx_) {
        auto chainstate_svc = std::dynamic_pointer_cast<ChainstateService>(daemon_ctx_->chainstate);
        auto p2p_svc = std::dynamic_pointer_cast<P2PService>(daemon_ctx_->p2p);
        auto config_svc = std::dynamic_pointer_cast<ConfigService>(daemon_ctx_->config);
        const auto readiness = mining::EvaluateMiningReadiness(
            chainstate_svc.get(),
            p2p_svc.get(),
            config_svc.get());
        if (!readiness.ready) {
            {
                std::lock_guard readiness_lock(readiness_mutex_);
                readiness_pause_code_ = readiness.reason_code;
                readiness_pause_reason_ = readiness.message;
            }
            if (logger_) {
                logger_->warning("[MiningManager] Refusing start: " + readiness.message);
            }
            return false;
        }
    }

    // Determine thread count
    if (threads <= 0) {
        threads = getOptimalThreadCount();
    }
    thread_count_.store(threads);

    // Get mining address
    std::string address;
    {
        std::lock_guard lock(address_mutex_);
        address = mining_address_;
    }

    if (address.empty()) {
        if (logger_) logger_->warning("[MiningManager] No mining address set, using default");
        // Could auto-derive from wallet here if available
    }

    // Set mining address in BlockAssembler
    if (!address.empty()) {
        block_assembler_->SetMiningAddress(address);
    }

    // Reset shutdown flag
    shutdown_requested_.store(false);

    // Reset job manager state (clean slate for new mining session)
    {
        std::lock_guard lock(job_mutex_);
        force_job_refresh_.store(false);
        job_available_.store(false);
        last_accepted_tip_.reset();
        current_job_.reset();
    }
    paused_for_readiness_.store(false);
    {
        std::lock_guard readiness_lock(readiness_mutex_);
        readiness_pause_code_.clear();
        readiness_pause_reason_.clear();
    }

    // Reset statistics
    stats_.is_mining.store(true);
    stats_.active_threads.store(threads);
    stats_.total_hashes.store(0);
    stats_.current_hashrate.store(0.0);
    stats_.jobs_processed.store(0);
    stats_.mining_start_time.store(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    last_hashrate_update_ = std::chrono::steady_clock::now();
    last_hashrate_total_hashes_ = 0;

    // Start job manager thread FIRST
    job_manager_thread_ = std::thread(&MiningManager::jobManagerLoop, this);

    // Wait a bit for job manager to create first job
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Start CPU worker threads
    worker_threads_.reserve(threads);
    for (int i = 0; i < threads; i++) {
        worker_threads_.emplace_back(&MiningManager::workerLoop, this, i);
    }

    // Try to initialize GPU mining (runs alongside CPU)
    gpu_nonce_cursor_.store(0x80000000);  // Reset GPU nonce range
    if (initGPU()) {
        gpu_worker_thread_ = std::thread(&MiningManager::gpuWorkerLoop, this);
        if (logger_) {
            logger_->info("[MiningManager] GPU mining thread started (" +
                         gpu_backend_->getDeviceName() + ")");
        }
    }

    if (logger_) {
        std::string mode = gpu_available_.load() ? "CPU+" + gpu::backendToString(gpu_backend_->getBackendType()) : "CPU-only";
        logger_->info("[MiningManager] Mining started with " + std::to_string(threads) + " CPU threads (" + mode + ")");
    }

    // Push initial metrics
    pushMetrics();

    return true;
}

void MiningManager::stopMining() {
    if (!isMining()) {
        return;
    }

    if (logger_) {
        logger_->info("[MiningManager] Stopping mining...");
    }

    // Phase B approved shutdown sequence:
    // (1) Set atomic shutdown flag
    shutdown_requested_.store(true);
    stats_.is_mining.store(false);

    if (current_job_) {
        current_job_->stop_mining.store(true);
    }

    // (2) Wake up all workers
    job_cv_.notify_all();

    // (3) Join CPU worker threads
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();

    // (3b) Join GPU worker thread
    if (gpu_worker_thread_.joinable()) {
        gpu_worker_thread_.join();
    }
    if (gpu_backend_) {
        gpu_backend_->stop();
        gpu_backend_.reset();
    }
    gpu_available_.store(false);
    gpu_hashrate_.store(0.0);

    // (4) Job manager already sees shutdown_requested_

    // (5) Join job manager thread
    if (job_manager_thread_.joinable()) {
        job_manager_thread_.join();
    }

    // (6) Clean up
    {
        std::lock_guard lock(job_mutex_);
        current_job_.reset();
    }
    paused_for_readiness_.store(false);
    {
        std::lock_guard readiness_lock(readiness_mutex_);
        readiness_pause_code_.clear();
        readiness_pause_reason_.clear();
    }

    stats_.active_threads.store(0);
    stats_.current_hashrate.store(0.0);
    last_hashrate_total_hashes_ = stats_.total_hashes.load();
    last_hashrate_update_ = std::chrono::steady_clock::now();

    if (logger_) {
        logger_->info("[MiningManager] Mining stopped");
    }

    // Push final metrics
    pushMetrics();
}

// ═══════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════

void MiningManager::setMiningAddress(const std::string& address) {
    std::lock_guard lock(address_mutex_);
    mining_address_ = address;

    if (block_assembler_) {
        block_assembler_->SetMiningAddress(address);
    }

    if (logger_) {
        logger_->info("[MiningManager] Mining address set: " + address);
    }
}

std::string MiningManager::getMiningAddress() const {
    std::lock_guard lock(address_mutex_);
    return mining_address_;
}

void MiningManager::setThreadCount(int threads) {
    if (threads <= 0) {
        threads = getOptimalThreadCount();
    }

    bool was_mining = isMining();

    if (was_mining) {
        // Signal shutdown and wait for clean stop
        shutdown_requested_.store(true);
        stats_.is_mining.store(false);
        if (current_job_) {
            current_job_->stop_mining.store(true);
        }
        job_cv_.notify_all();

        // Join CPU workers first
        for (auto& t : worker_threads_) {
            if (t.joinable()) t.join();
        }
        worker_threads_.clear();

        // Join GPU worker
        if (gpu_worker_thread_.joinable()) {
            gpu_worker_thread_.join();
        }
        if (gpu_backend_) {
            gpu_backend_->stop();
            gpu_backend_.reset();
        }
        gpu_available_.store(false);

        if (job_manager_thread_.joinable()) {
            job_manager_thread_.join();
        }

        {
            std::lock_guard lock(job_mutex_);
            current_job_.reset();
        }

        stats_.active_threads.store(0);

        if (logger_) {
            logger_->info("[MiningManager] Stopped for thread count change");
        }
    }

    thread_count_.store(threads);

    if (was_mining) {
        startMining(threads);
    }
}

int MiningManager::getOptimalThreadCount() const {
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) {
        return 1;  // Fallback
    }

    // Use hardware_concurrency - 1 to leave one core for system
    return std::max(1u, hw_threads - 1);
}

// ═══════════════════════════════════════════════════════════════════
// Thread Workers
// ═══════════════════════════════════════════════════════════════════

void MiningManager::jobManagerLoop() {
    if (logger_) {
        logger_->info("[DEBUG-V2] Job manager thread STARTED");
        logger_->debug("[MiningManager] Job manager thread started");
    }

    std::unique_lock<std::mutex> lock(job_mutex_);

    while (!shutdown_requested_.load()) {
        try {
            // Wait with timeout (500ms default) for periodic staleness checks
            // Wake early if:
            //  - shutdown requested
            //  - force refresh requested (block accepted)
            //  - no job available (initial state)
            job_cv_.wait_for(lock, std::chrono::milliseconds(refresh_interval_ms_), [this] {
                return shutdown_requested_.load()
                    || force_job_refresh_.load()
                    || !job_available_.load();
            });

            if (shutdown_requested_.load()) {
                break;
            }

            mining::MiningReadiness readiness;
            {
                auto* daemon_ctx = daemon_ctx_;
                lock.unlock();
                auto chainstate_svc = daemon_ctx
                    ? std::dynamic_pointer_cast<ChainstateService>(daemon_ctx->chainstate)
                    : std::shared_ptr<ChainstateService>{};
                auto p2p_svc = daemon_ctx
                    ? std::dynamic_pointer_cast<P2PService>(daemon_ctx->p2p)
                    : std::shared_ptr<P2PService>{};
                auto config_svc = daemon_ctx
                    ? std::dynamic_pointer_cast<ConfigService>(daemon_ctx->config)
                    : std::shared_ptr<ConfigService>{};
                readiness = mining::EvaluateMiningReadiness(
                    chainstate_svc.get(),
                    p2p_svc.get(),
                    config_svc.get());
                lock.lock();
            }

            if (!readiness.ready) {
                std::shared_ptr<MiningJob> old_job = current_job_;
                if (old_job) {
                    old_job->stop_mining.store(true);
                }
                current_job_.reset();
                job_available_.store(false);
                force_job_refresh_.store(false);
                last_accepted_tip_.reset();

                {
                    std::lock_guard stats_lock(stats_mutex_);
                    stats_.current_job_id.clear();
                    stats_.current_height = 0;
                    stats_.current_difficulty = 0;
                }

                bool state_changed = !paused_for_readiness_.exchange(true);
                bool reason_changed = false;
                {
                    std::lock_guard readiness_lock(readiness_mutex_);
                    reason_changed = readiness_pause_code_ != readiness.reason_code;
                    readiness_pause_code_ = readiness.reason_code;
                    readiness_pause_reason_ = readiness.message;
                }

                if ((state_changed || reason_changed) && logger_) {
                    logger_->warning("[MiningManager] Auto-paused: " + readiness.message);
                }
                continue;
            }

            if (paused_for_readiness_.exchange(false)) {
                std::string resume_reason;
                {
                    std::lock_guard readiness_lock(readiness_mutex_);
                    resume_reason = readiness_pause_reason_;
                    readiness_pause_code_.clear();
                    readiness_pause_reason_.clear();
                }
                if (logger_) {
                    logger_->info("[MiningManager] Auto-resumed after readiness recovery" +
                                  (resume_reason.empty() ? std::string() : " (" + resume_reason + ")"));
                }
            }

            // ---- Handle forced refresh (block accepted) ----
            if (force_job_refresh_.load()) {
                force_job_refresh_.store(false);  // 🔑 MUST clear immediately

                std::shared_ptr<MiningJob> new_job;

                if (last_accepted_tip_.has_value()) {
                    // Use explicit tip to avoid ChainDB race
                    uint256 explicit_tip = *last_accepted_tip_;
                    last_accepted_tip_.reset();   // 🔑 consume once

                    // Release lock while calling BlockAssembler
                    lock.unlock();
                    new_job = block_assembler_->CreateJob(&explicit_tip);
                    lock.lock();

                    if (logger_) {
                        logger_->info("[MiningManager] Force refresh with explicit tip: " +
                                    explicit_tip.GetHex().substr(0, 16) + "...");
                    }
                } else {
                    // No explicit tip - query ChainDB (default)
                    lock.unlock();
                    new_job = block_assembler_->CreateJob(nullptr);
                    lock.lock();

                    if (logger_) {
                        logger_->info("[MiningManager] Force refresh without explicit tip");
                    }
                }

                if (new_job) {
                    if (current_job_) {
                        current_job_->stop_mining.store(true);
                    }

                    current_job_ = new_job;
                    job_available_.store(true);

                    // Update job stats
                    {
                        std::lock_guard stats_lock(stats_mutex_);
                        stats_.current_job_id = new_job->job_id;
                        stats_.current_height = new_job->height;
                        stats_.current_difficulty = new_job->target_bits;
                    }

                    stats_.jobs_processed.fetch_add(1);

                    if (logger_) {
                        logger_->debug("[MiningManager] New job created: height=" +
                                      std::to_string(new_job->height) +
                                      " job_id=" + new_job->job_id);
                    }

                    // Wake up all workers
                    job_cv_.notify_all();

                    // Push metrics after job creation
                    lock.unlock();
                    pushMetrics();
                    lock.lock();
                }

                continue;  // go back to wait
            }

            // ---- Normal job creation path (initial or periodic) ----
            if (!job_available_.load()) {
                lock.unlock();
                auto new_job = block_assembler_->CreateJob(nullptr);
                lock.lock();

                if (new_job) {
                    if (current_job_) {
                        current_job_->stop_mining.store(true);
                    }

                    current_job_ = new_job;
                    job_available_.store(true);

                    // Update job stats
                    {
                        std::lock_guard stats_lock(stats_mutex_);
                        stats_.current_job_id = new_job->job_id;
                        stats_.current_height = new_job->height;
                        stats_.current_difficulty = new_job->target_bits;
                    }

                    stats_.jobs_processed.fetch_add(1);

                    if (logger_) {
                        logger_->debug("[MiningManager] Initial job created: height=" +
                                      std::to_string(new_job->height) +
                                      " job_id=" + new_job->job_id);
                    }

                    // Wake up all workers
                    job_cv_.notify_all();

                    // Push metrics
                    lock.unlock();
                    pushMetrics();
                    lock.lock();
                }
            }

        } catch (const std::exception& e) {
            if (logger_) {
                logger_->error("[MiningManager] Job manager error: " + std::string(e.what()));
            }
        }
    }

    if (logger_) {
        logger_->debug("[MiningManager] Job manager thread stopped");
    }
}

void MiningManager::workerLoop(int thread_id) {
    if (logger_) {
        logger_->info("[DEBUG-V2] Worker thread " + std::to_string(thread_id) + " STARTED");
        logger_->debug("[MiningManager] Worker thread " + std::to_string(thread_id) + " started");
    }

    uint64_t local_hashes = 0;
    auto last_update = std::chrono::steady_clock::now();

    while (!shutdown_requested_.load()) {
        // Wait for job (use job_available_ flag, not current_job_ pointer!)
        std::shared_ptr<MiningJob> job;
        {
            std::unique_lock lock(job_mutex_);
            job_cv_.wait(lock, [this] {
                return job_available_.load() || shutdown_requested_.load();
            });

            if (shutdown_requested_.load()) {
                break;
            }

            job = current_job_;
        }

        if (!job) {
            continue;
        }

        // Phase B approved nonce striding: Thread i gets nonces i, i+N, i+2N...
        uint32_t thread_count = thread_count_.load();
        uint32_t nonce = thread_id;

        // Check stop_mining BEFORE entering loop to prevent spin
        if (job->stop_mining.load()) {
            // Job is already stopped - clear availability flag to prevent spin
            job_available_.store(false);
            continue;
        }

        while (!job->stop_mining.load() && !shutdown_requested_.load()) {
            // Try nonce
            if (tryNonce(job, nonce)) {
                // Solution found!
                onSolutionFound(job, nonce);
                break;
            }

            // Update hash count
            local_hashes++;
            stats_.total_hashes.fetch_add(1);

            // Periodically update hashrate (every 10k hashes, not in hot path)
            if (local_hashes % 10000 == 0) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_update
                ).count();

                if (elapsed >= 1000) {  // Update every second
                    updateHashrate(local_hashes, elapsed);
                    local_hashes = 0;
                    last_update = now;
                }
            }

            // Stride to next nonce
            nonce += thread_count;

            // CPU uses lower half [0, 0x7FFFFFFF] when GPU is active
            uint32_t nonce_ceiling = gpu_available_.load() ? 0x80000000u : 0xFFFFFFFFu;
            if (nonce >= nonce_ceiling - thread_count) {
                nonce = thread_id;
            }
        }
    }

    if (logger_) {
        logger_->debug("[MiningManager] Worker thread " + std::to_string(thread_id) + " stopped");
    }
}

// ═══════════════════════════════════════════════════════════════════
// Mining Helpers
// ═══════════════════════════════════════════════════════════════════

bool MiningManager::tryNonce(std::shared_ptr<MiningJob> job, uint32_t nonce) {
    // DEBUG: Log first few calls
    static std::atomic<uint64_t> call_count{0};
    uint64_t count = call_count.fetch_add(1);
    if (count < 10 || (count % 1000000 == 0)) {
        if (logger_) {
            logger_->info("[DEBUG-V2-tryNonce] called: nonce=" + std::to_string(nonce) +
                         " total_calls=" + std::to_string(count));
        }
    }

    // Set nonce in header
    BlockHeader header = job->header;
    header.nonce = nonce;

    // FIX: Use proper Bitcoin-style double SHA-256 via GetHash()
    std::string block_hash = header.GetHash().GetHex();

    // DEBUG: Log first hash check to verify target
    if (count == 0) {
        if (logger_) {
            logger_->info("[DEBUG-FIRST-HASH] hash=" + block_hash.substr(0, 16) + "...");
            logger_->info("[DEBUG-FIRST-HASH] target=" + job->target_hex.substr(0, 16) + "...");
            logger_->info("[DEBUG-FIRST-HASH] bits=0x" + std::to_string(job->target_bits));
        }
    }

    // Compare with target (hash < target means solution found)
    bool found = block_hash < job->target_hex;
    if (found && logger_) {
        logger_->info("[BLOCK-FOUND] nonce=" + std::to_string(nonce) + " hash=" + block_hash);
    }
    return found;
}

void MiningManager::onSolutionFound(std::shared_ptr<MiningJob> job, uint32_t nonce) {
    // WINNER-TAKES-ALL GATE: Only first solution for this job is submitted
    // Critical for preventing fork storms in low-difficulty (regtest) scenarios
    bool expected = false;
    if (!job->solution_claimed.compare_exchange_strong(expected, true)) {
        // Another thread already claimed this solution - discard silently
        return;
    }

    // From here on: EXACTLY ONE THREAD enters per job

    // Solution found! Log full header
    if (logger_) {
        const auto& h = job->header;
        logger_->info("[MiningManager] *** BLOCK FOUND *** height=" +
                     std::to_string(job->height) +
                     " nonce=" + std::to_string(nonce));
        logger_->info("[MiningManager]   prev_hash:    " + h.prev_block_hash.GetHex());
        logger_->info("[MiningManager]   merkle_root:  " + h.merkle_root.GetHex());
        logger_->info("[MiningManager]   utreexo_root: " + h.utreexo_root.GetHex());
        char bits_buf[16]; snprintf(bits_buf, sizeof(bits_buf), "0x%08x", h.difficulty);
        logger_->info("[MiningManager]   nBits:        " + std::string(bits_buf));
    }

    // Build complete block
    Block block;
    block.header = job->header;
    block.header.nonce = nonce;
    block.vtx = job->transactions;

    // Submit to blockchain via BlockAcceptor
    try {
        BlockAcceptResult result = BlockAcceptor::AcceptBlockFromPeer(block, "miner");

        if (result.accepted()) {
            stats_.blocks_found.fetch_add(1);
            stats_.last_block_time.store(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );

            metrics::LabelMap labels;
            metrics::MetricsRegistry::IncrementMiningBlocksFound(labels);

            if (logger_) {
                logger_->info("[MiningManager] ✅ Block ACCEPTED! hash=" + result.block_hash.GetHex() +
                             " height=" + std::to_string(result.height));
            }

            // CRITICAL: Store accepted tip hash for explicit CreateJob() call
            // This avoids race condition with ChainDB getTip() query
            {
                std::lock_guard<std::mutex> lock(job_mutex_);
                last_accepted_tip_ = result.block_hash;  // store explicit tip (already uint256)
                force_job_refresh_.store(true);          // trigger refresh
                job_available_.store(false);             // invalidate current job
            }

            // Wake up job manager to create new job with explicit tip
            job_cv_.notify_one();
        } else {
            if (logger_) {
                logger_->error("[MiningManager] ❌ Block REJECTED: " + result.reason +
                              " (code: " + std::string(BlockRejectCodeToString(result.code)) + ")");
            }
        }
    } catch (const std::exception& e) {
        if (logger_) {
            logger_->error("[MiningManager] Block submission exception: " + std::string(e.what()));
        }
    }

    // Signal job staleness
    job->stop_mining.store(true);

    // Push metrics after block found
    pushMetrics();
}

void MiningManager::updateHashrate(uint64_t hashes, uint64_t elapsed_ms) {
    // Worker-local inputs are intentionally ignored for final rate computation.
    // We derive hashrate from GLOBAL total_hashes delta so displayed H/s is the
    // aggregate across all active worker threads.
    (void)hashes;

    if (elapsed_ms == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(stats_mutex_);

    const uint64_t total_hashes = stats_.total_hashes.load();
    const uint64_t delta_hashes = total_hashes - last_hashrate_total_hashes_;
    const auto elapsed_global_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_hashrate_update_
    ).count();

    if (elapsed_global_ms < 1000 || delta_hashes == 0) {
        return;
    }

    const double hashrate = (static_cast<double>(delta_hashes) * 1000.0) /
                            static_cast<double>(elapsed_global_ms);
    stats_.current_hashrate.store(hashrate);

    last_hashrate_total_hashes_ = total_hashes;
    last_hashrate_update_ = now;

    // Push metrics periodically based on global work.
    if ((total_hashes % 50000) == 0) {
        pushMetrics();
    }
}

// ═══════════════════════════════════════════════════════════════════
// GPU Mining
// ═══════════════════════════════════════════════════════════════════

static constexpr uint32_t GPU_NONCE_BATCH_SIZE = 0x1000000;  // 16M nonces per GPU dispatch

bool MiningManager::initGPU() {
#if defined(ENABLE_CUDA) || defined(ENABLE_METAL) || defined(ENABLE_OPENCL)
    fprintf(stderr, "[MiningManager::initGPU] Entered (ENABLE_METAL=%d)\n",
#ifdef ENABLE_METAL
        1
#else
        0
#endif
    );

    gpu::GPUDeviceManager device_manager;
    auto devices = device_manager.detectAllDevices();
    fprintf(stderr, "[MiningManager::initGPU] detectAllDevices returned %zu devices\n", devices.size());
    if (devices.empty()) {
        fprintf(stderr, "[MiningManager::initGPU] No GPU devices found\n");
        if (logger_) logger_->info("[MiningManager] No GPU devices found — CPU-only mining");
        return false;
    }

    auto best_backend = device_manager.getBestAvailableBackend();
    fprintf(stderr, "[MiningManager::initGPU] Best backend: %s\n", gpu::backendToString(best_backend).c_str());
    if (best_backend == gpu::BackendType::NONE) {
        if (logger_) logger_->info("[MiningManager] No usable GPU backend — CPU-only mining");
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
        fprintf(stderr, "[MiningManager::initGPU] No device for %s backend\n", gpu::backendToString(best_backend).c_str());
        if (logger_) logger_->info("[MiningManager] No device for " + gpu::backendToString(best_backend));
        return false;
    }

    fprintf(stderr, "[MiningManager::initGPU] Using device: %s (%zu MB)\n",
            target_dev->name.c_str(), target_dev->global_memory_mb);
    if (logger_) {
        logger_->info("[MiningManager] GPU: " + target_dev->name + " (" +
                     gpu::backendToString(best_backend) + ", " +
                     std::to_string(target_dev->global_memory_mb) + " MB)");
    }

    gpu_backend_ = gpu::createBackend(best_backend);
    if (!gpu_backend_) {
        fprintf(stderr, "[MiningManager::initGPU] createBackend returned nullptr\n");
        if (logger_) logger_->error("[MiningManager] Failed to create " + gpu::backendToString(best_backend) + " backend");
        return false;
    }

    fprintf(stderr, "[MiningManager::initGPU] Calling initDevice(%u)...\n", target_dev->device_id);
    if (!gpu_backend_->initDevice(target_dev->device_id)) {
        fprintf(stderr, "[MiningManager::initGPU] initDevice FAILED\n");
        if (logger_) logger_->error("[MiningManager] Failed to initialize GPU device");
        gpu_backend_.reset();
        return false;
    }

    fprintf(stderr, "[MiningManager::initGPU] Calling compileKernel...\n");
    if (!gpu_backend_->compileKernel("")) {
        fprintf(stderr, "[MiningManager::initGPU] compileKernel FAILED\n");
        if (logger_) logger_->error("[MiningManager] Failed to compile GPU kernel");
        gpu_backend_->stop();
        gpu_backend_.reset();
        return false;
    }

    fprintf(stderr, "[MiningManager::initGPU] SUCCESS — GPU mining ready\n");
    gpu_available_.store(true);
    return true;
#else
    fprintf(stderr, "[MiningManager::initGPU] No GPU backends compiled in\n");
    return false;
#endif
}

void MiningManager::gpuWorkerLoop() {
    if (logger_) {
        logger_->info("[GPU] Mining thread started on " + gpu_backend_->getDeviceName());
    }

    while (!shutdown_requested_.load()) {
        // Wait for job
        std::shared_ptr<MiningJob> job;
        {
            std::unique_lock lock(job_mutex_);
            job_cv_.wait(lock, [this] {
                return job_available_.load() || shutdown_requested_.load();
            });

            if (shutdown_requested_.load()) break;
            job = current_job_;
        }

        if (!job || job->stop_mining.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Allocate next batch of nonces from GPU range
        uint32_t start_nonce = gpu_nonce_cursor_.fetch_add(GPU_NONCE_BATCH_SIZE);
        // Wrap around if exhausted (very unlikely per job)
        if (start_nonce < 0x80000000) {
            start_nonce = 0x80000000;
            gpu_nonce_cursor_.store(0x80000000 + GPU_NONCE_BATCH_SIZE);
        }
        uint32_t end_nonce = start_nonce + GPU_NONCE_BATCH_SIZE - 1;

        // Prepare work package
        gpu::WorkPackage work;
        auto header_bytes = job->header.SerializeForHash();
        static_assert(sizeof(work.header) >= DINERO_HEADER_SIZE_BYTES, "WorkPackage header too small");
        memcpy(work.header, header_bytes.data(), DINERO_HEADER_SIZE_BYTES);

        // Convert compact target bits to 256-bit target for GPU comparison
        std::string target_hex = BitsToTargetHex(job->target_bits);
        for (int i = 0; i < 8; i++) {
            std::string word_hex = target_hex.substr(i * 8, 8);
            work.target[i] = static_cast<uint32_t>(strtoul(word_hex.c_str(), nullptr, 16));
        }

        work.nonce_start = start_nonce;
        work.nonce_end = end_nonce;
        work.backend = gpu_backend_->getBackendType();

        // Dispatch to GPU
        gpu::MiningResult result;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool ok = gpu_backend_->mine(work, result);
        auto t1 = std::chrono::high_resolution_clock::now();

        if (!ok) {
            if (logger_) logger_->error("[GPU] Kernel execution error");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Update stats
        stats_.total_hashes.fetch_add(result.hashes_tried);
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (elapsed_us > 0) {
            gpu_hashrate_.store(static_cast<double>(result.hashes_tried) / elapsed_us * 1e6);
        }

        if (result.found && !job->stop_mining.load()) {
            if (logger_) {
                std::ostringstream o;
                o << std::hex << result.nonce;
                logger_->info("[GPU] *** SOLUTION FOUND *** nonce=0x" + o.str());
            }
            onSolutionFound(job, result.nonce);
        }
    }

    if (logger_) {
        logger_->info("[GPU] Mining thread stopped");
    }
}

void MiningManager::pushMetrics() {
    // Phase C: Push to MetricsRegistry
    // RPC pulls from MetricsRegistry (not directly from MiningManager)
    // Uses static MetricsRegistry API (no instance pointer needed)

    // Empty labels for now (could add per-instance labels in the future)
    metrics::LabelMap labels;

    // GAUGE metrics (current state)
    metrics::MetricsRegistry::SetMiningHashrate(stats_.current_hashrate.load(), labels);
    metrics::MetricsRegistry::SetMiningThreads(stats_.active_threads.load(), labels);

    // Get current job info under mutex
    uint32_t current_height = 0;
    uint32_t current_bits = 0;
    {
        std::lock_guard lock(stats_mutex_);
        current_height = stats_.current_height;
        current_bits = stats_.current_difficulty;
    }

    if (current_height > 0) {
        metrics::MetricsRegistry::SetMiningJobHeight(current_height, labels);
    }

    if (current_bits > 0) {
        metrics::MetricsRegistry::SetMiningCurrentBits(current_bits, labels);
    }

    // Note: Blocks found counter is incremented in onSolutionFound()
    // Total hashes and jobs_processed are internal stats, not exposed to Prometheus
    // (they're available via GetMetrics() JSON for debugging)
}

} // namespace dinero
