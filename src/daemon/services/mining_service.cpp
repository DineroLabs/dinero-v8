#include "daemon/services/mining_service.h"
#include "daemon/services/logger_service.h"
#include "daemon/services/config_service.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/daemon_context.h"
#include "common/ilogger.h"           // For ILogger interface dependency injection
#include "daemon/mining_safety_gates.h"  // Week 3: Wire context to MiningSafetyGates
// #include "mining/template_validator.h"   // Week 6: Wire context to BlockBroadcastVerifier (REMOVED - zombie code)
#include "consensus/pow_consensus_engine.h"  // Phase 2: PoW consensus engine
#include "metrics/metrics_registry.h"    // Week 5: Telemetry integration
#include "mining/gpu/gpu_device_manager.h"  // GPU mining support
#include "mining/gpu/compute_backend.h"     // GPU backend interface
#include <sstream>
#include <iostream>
#include <thread>
#include <chrono>

namespace dinero {

MiningService::MiningService() : start_time_(std::chrono::steady_clock::now()) {
}

MiningService::~MiningService() {
    Stop();  // Ensure clean shutdown
}

bool MiningService::Init(DaemonContext& ctx) {
    // Store dependencies
    if (ctx.logger) {
        logger_ = std::dynamic_pointer_cast<LoggerService>(ctx.logger);
    }
    // Use dedicated mining logger if available, fallback to shared logger
    logger_interface_ = ctx.mining_logger ? ctx.mining_logger : ctx.logger_interface;

    if (ctx.config) {
        config_ = std::dynamic_pointer_cast<ConfigService>(ctx.config);
    }
    if (ctx.chainstate) {
        chainstate_ = std::dynamic_pointer_cast<ChainstateService>(ctx.chainstate);
    }
    if (ctx.mempool) {
        mempool_ = std::dynamic_pointer_cast<MempoolService>(ctx.mempool);
    }

    if (!logger_interface_ || !config_ || !chainstate_) {
        if (!logger_interface_) {
            std::cerr << "[MiningService] Logger interface dependency missing" << std::endl;
        } else {
            std::cerr << "[MiningService] Missing required dependencies" << std::endl;
        }
        return false;
    }

    // Week 5: Generate miner instance ID (for metrics tracking)
    // Format: "miner_" + timestamp + "_" + random suffix
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    miner_id_ = "miner_" + std::to_string(timestamp);
    logger_interface_->info("[MiningService] Miner ID: " + miner_id_);

    // Week 3: Wire DaemonContext to MiningSafetyGates for chainstate/wallet access
    MiningSafetyGates::SetContext(&ctx);
    logger_interface_->info("[MiningService] MiningSafetyGates context wired");

    // Week 6: Wire DaemonContext to BlockBroadcastVerifier for P2P access (REMOVED - zombie code)
    // dinero::BlockBroadcastVerifier::SetContext(&ctx);
    // logger_interface_->info("[MiningService] BlockBroadcastVerifier context wired");

    // Phase C: Legacy Mining class removed
    // All initialization now happens in MiningManager v2

    // Phase C: Create and initialize MiningManager v2
    try {
        logger_interface_->info("[MiningService] Creating MiningManager v2...");
        mining_manager_v2_ = std::make_unique<MiningManager>();
        logger_interface_->info("[MiningService] MiningManager v2 created, calling Init()...");

        bool init_result = mining_manager_v2_->Init(ctx);

        if (!init_result) {
            logger_interface_->error("[MiningService] Failed to initialize MiningManager v2");
            return false;
        }

        logger_interface_->info("[MiningService] MiningManager v2 initialized (Phase C)");
    } catch (const std::exception& e) {
        logger_interface_->error("[MiningService] Failed to create MiningManager v2: " +
                      std::string(e.what()));
        return false;
    }

    // Initialize GPU device manager (optional - no failure if GPU unavailable)
    try {
        gpu_device_manager_ = std::make_unique<dinero::gpu::GPUDeviceManager>();
        gpu_device_manager_->detectAllDevices();
        if (gpu_device_manager_->hasGPU()) {
            logger_interface_->info("[MiningService] GPU detected: " +
                         std::to_string(gpu_device_manager_->getDeviceCount()) + " device(s)");
            auto best_backend = gpu_device_manager_->getBestAvailableBackend();
            logger_interface_->info("[MiningService] Best GPU backend: " +
                         dinero::gpu::backendToString(best_backend));
        } else {
            logger_interface_->info("[MiningService] No GPU devices detected (CPU-only mining)");
        }
    } catch (const std::exception& e) {
        logger_interface_->warning("[MiningService] GPU initialization failed (non-fatal): " +
                        std::string(e.what()));
    }

    logger_interface_->info("[MiningService] Initialized successfully");
    return true;
}

bool MiningService::Start() {
    if (started_) {
        logger_interface_->warning("[MiningService] Already started");
        return false;
    }

    logger_interface_->info("[MiningService] Starting mining service...");

    // Phase C: Start MiningManager v2 (does NOT auto-start mining)
    if (mining_manager_v2_) {
        if (!mining_manager_v2_->Start()) {
            logger_interface_->error("[MiningService] Failed to start MiningManager v2");
            return false;
        }
        logger_interface_->info("[MiningService] MiningManager v2 service started");
    }

    // Check if mining should be enabled from config
    mining_enabled_ = config_->GetBool("gen", false);  // Bitcoin-style "gen" flag
    int thread_count = config_->GetInt("genproclimit", 1);

    logger_interface_->info("[MiningService] Mining configured:");
    logger_interface_->info("[MiningService]   Enabled: " + std::string(mining_enabled_ ? "yes" : "no"));
    logger_interface_->info("[MiningService]   Threads: " + std::to_string(thread_count));

    // Phase C: Set thread count on MiningManager v2
    if (mining_manager_v2_) {
        mining_manager_v2_->setThreadCount(thread_count);
    }

    // Phase C: Get mining address from config and set on MiningManager v2
    std::string mining_address = config_->GetString("miningaddress", "");

    if (!mining_address.empty() && mining_manager_v2_) {
        mining_manager_v2_->setMiningAddress(mining_address);
        logger_interface_->info("[MiningService] Using configured mining address: " + mining_address);
    } else if (mining_address.empty()) {
        logger_interface_->warning("[MiningService] No mining address configured - mining rewards will not be claimed");
        logger_interface_->warning("[MiningService] Set 'miningaddress=<your_address>' in config to receive rewards");
    }

    // ═══════════════════════════════════════════════════════════════════
    // Phase F.2: Removed auto-start mining code (E.3.1 contract enforcement)
    // ═══════════════════════════════════════════════════════════════════
    //
    // Contract: E.3 - Restart Semantics
    // - Mining does NOT auto-resume after daemon restart
    // - User must explicitly call mining.start RPC
    // - Config persists (address), STATE does not (mining enabled=false)
    //
    // BEFORE F.2 (violated E.3.1):
    //   if (mining_enabled_) {
    //       mining_manager_v2_->startMining(thread_count);  // ❌ Auto-start!
    //   }
    //
    // AFTER F.2 (compliant):
    //   Mining never auto-starts, even if gen=1 config flag is set.
    //   User must explicitly call mining.start RPC to begin mining.
    //
    // ═══════════════════════════════════════════════════════════════════

    logger_interface_->info("[MiningService] Mining auto-start disabled (E.3 contract enforcement)");
    logger_interface_->info("[MiningService] Call mining.start RPC to begin mining");

    // Week 5: Start telemetry update thread
    start_time_ = std::chrono::steady_clock::now();
    telemetry_running_ = true;
    telemetry_thread_ = std::make_unique<std::thread>(&MiningService::TelemetryUpdateLoop, this);
    logger_interface_->info("[MiningService] Telemetry update thread started");

    // Week 5: Initial telemetry update
    UpdateTelemetry();

    started_ = true;
    logger_interface_->info("[MiningService] Mining service started successfully");
    return true;
}

void MiningService::Stop() {
    if (!started_) {
        return;
    }

    logger_interface_->info("[MiningService] Stopping mining service...");

    // Stop GPU mining first (if running)
    stopGPUMining();

    // Week 5: Stop telemetry update thread
    telemetry_running_ = false;
    if (telemetry_thread_ && telemetry_thread_->joinable()) {
        telemetry_thread_->join();
        logger_interface_->info("[MiningService] Telemetry update thread stopped");
    }

    // Phase C: Get final stats from MiningManager v2 before stopping
    if (mining_enabled_ && mining_manager_v2_) {
        const auto& stats = mining_manager_v2_->getStats();
        uint64_t blocks_found = stats.blocks_found.load();
        double hashrate = stats.current_hashrate.load();

        logger_interface_->info("[MiningService] Mining statistics:");
        logger_interface_->info("[MiningService]   Blocks found: " + std::to_string(blocks_found));
        logger_interface_->info("[MiningService]   Final hashrate: " + std::to_string(hashrate) + " H/s");

        // Week 5: Final telemetry update
        UpdateTelemetry();
    }

    // Phase C: Stop MiningManager v2
    if (mining_manager_v2_) {
        mining_manager_v2_->Stop();
        logger_interface_->info("[MiningService] MiningManager v2 stopped");
    }

    logger_interface_->info("[MiningService] Mining service stopped");
    started_ = false;
}

bool MiningService::IsHealthy() const {
    if (!started_ || !mining_manager_v2_) {
        return false;
    }

    // Phase C: Use MiningManager v2 for health check
    if (mining_enabled_) {
        try {
            // If we can query mining status without error, it's healthy
            mining_manager_v2_->isMining();
            return mining_manager_v2_->IsHealthy();
        } catch (...) {
            return false;
        }
    }

    // If mining is disabled, service is still healthy (just idle)
    return true;
}

std::string MiningService::GetMetrics() const {
    if (!mining_manager_v2_) {
        return R"({"status":"not_initialized"})";
    }

    // Phase C: Use MiningManager v2 for metrics
    const auto& stats = mining_manager_v2_->getStats();

    std::ostringstream oss;
    oss << "{"
        << R"("service":"mining",)"
        << R"("started":)" << (started_ ? "true" : "false") << ","
        << R"("enabled":)" << (mining_enabled_ ? "true" : "false") << ","
        << R"("is_mining":)" << (stats.is_mining.load() ? "true" : "false") << ","
        << R"("hashrate":)" << stats.current_hashrate.load() << ","
        << R"("blocks_found":)" << stats.blocks_found.load() << ","
        << R"("active_threads":)" << stats.active_threads.load() << ","
        << R"("thread_count":)" << mining_manager_v2_->getThreadCount()
        << "}";

    return oss.str();
}

// Phase 2: Create block template using consensus engine
Block MiningService::createBlockTemplate(const DaemonContext& ctx) {
    // Phase C: Use consensus engine from context
    if (ctx.consensus) {
        if (logger_interface_) {
            logger_interface_->info("[MiningService] Using consensus engine for block template creation");
        }
        return ctx.consensus->CreateBlockTemplate();
    }

    // Phase C: Consensus engine required (no fallback)
    if (logger_interface_) {
        logger_interface_->error("[MiningService] Consensus engine not available - cannot create block template");
    }
    return Block();  // Return empty block on error
}

// Week 5: Update MetricsRegistry with current mining statistics
void MiningService::UpdateTelemetry() {
    if (!mining_manager_v2_) {
        return;
    }

    try {
        // Phase C: Get stats from MiningManager v2
        const auto& stats = mining_manager_v2_->getStats();

        // Update mining metrics with per-miner labels
        metrics::LabelMap labels = {{"miner_id", miner_id_}};

        metrics::MetricsRegistry::SetMiningHashrate(stats.current_hashrate.load(), labels);
        metrics::MetricsRegistry::SetMiningThreads(mining_manager_v2_->getThreadCount(), labels);

        // Calculate uptime
        auto now = std::chrono::steady_clock::now();
        auto uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - start_time_).count();
        metrics::MetricsRegistry::SetMiningUptime(static_cast<double>(uptime_seconds), labels);

        // Log miner_id for debugging
        if (logger_interface_) {
            logger_interface_->debug("[MiningService] Telemetry updated: miner_id=" + miner_id_ +
                          " hashrate=" + std::to_string(stats.current_hashrate.load()) +
                          " threads=" + std::to_string(mining_manager_v2_->getThreadCount()));
        }

        // Note: Blocks found are updated via MiningManager v2
        // when blocks are actually found, so we don't duplicate those updates here
    } catch (const std::exception& e) {
        if (logger_interface_) {
            logger_interface_->warning("[MiningService] Failed to update telemetry: " +
                           std::string(e.what()));
        }
    }
}

// Week 5: Background thread that periodically updates metrics
void MiningService::TelemetryUpdateLoop() {
    while (telemetry_running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));  // Update every 5 seconds

        if (telemetry_running_ && mining_manager_v2_) {
            UpdateTelemetry();
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// GPU MINING IMPLEMENTATION (Unified Template Architecture)
// ═══════════════════════════════════════════════════════════════════════════

bool MiningService::hasGPU() const {
    return gpu_device_manager_ && gpu_device_manager_->hasGPU();
}

bool MiningService::startGPUMining(uint32_t device_id) {
    if (!gpu_device_manager_ || !gpu_device_manager_->hasGPU()) {
        if (logger_) {
            logger_interface_->error("[MiningService] No GPU devices available");
        }
        return false;
    }

    if (gpu_enabled_) {
        if (logger_) {
            logger_interface_->warning("[MiningService] GPU mining already running");
        }
        return false;
    }

    // Get best available backend
    auto backend_type = gpu_device_manager_->getBestAvailableBackend();
    if (backend_type == dinero::gpu::BackendType::NONE) {
        if (logger_) {
            logger_interface_->error("[MiningService] No GPU backend available");
        }
        return false;
    }

    // Create GPU backend
    gpu_backend_ = dinero::gpu::createBackend(backend_type);
    if (!gpu_backend_) {
        if (logger_) {
            logger_interface_->error("[MiningService] Failed to create GPU backend");
        }
        return false;
    }

    // Initialize device
    if (!gpu_backend_->initDevice(device_id)) {
        if (logger_) {
            logger_interface_->error("[MiningService] Failed to initialize GPU device " +
                         std::to_string(device_id));
        }
        gpu_backend_.reset();
        return false;
    }

    if (logger_) {
        logger_interface_->info("[MiningService] GPU mining starting on device: " +
                     gpu_backend_->getDeviceName());
    }

    // MiningManager v2 is CPU-only in this service path; refuse GPU start to
    // avoid a false "running" state with no real work submission pipeline.
    if (logger_) {
        logger_interface_->warning("[MiningService] GPU mining is unavailable in this build path");
    }

    gpu_should_stop_ = true;
    gpu_enabled_ = false;
    gpu_hashrate_.store(0.0);
    if (gpu_backend_) {
        gpu_backend_->stop();
        gpu_backend_.reset();
    }
    return false;
}

void MiningService::stopGPUMining() {
    if (!gpu_enabled_) {
        return;
    }

    if (logger_) {
        logger_interface_->info("[MiningService] Stopping GPU mining...");
    }

    // Signal GPU thread to stop
    gpu_should_stop_ = true;
    gpu_enabled_ = false;

    // Wait for GPU thread to finish
    if (gpu_mining_thread_.joinable()) {
        gpu_mining_thread_.join();
    }

    // Cleanup GPU backend
    if (gpu_backend_) {
        gpu_backend_->stop();
        gpu_backend_.reset();
    }

    if (logger_) {
        logger_interface_->info("[MiningService] GPU mining stopped");
    }
}

double MiningService::getGPUHashrate() const {
    if (!gpu_backend_) {
        return 0.0;
    }
    return gpu_backend_->getHashrate();
}

// GPU Mining worker thread - pulls from MiningService::createBlockTemplate()
void MiningService::GPUMiningLoop() {
    if (!logger_ || !logger_interface_) {
        return;
    }

    logger_interface_->info("[MiningService] GPU mining thread started");
    logger_interface_->error("[MiningService] GPUMiningLoop invoked without a wired GPU work pipeline");
    gpu_enabled_ = false;
    gpu_should_stop_ = true;
    gpu_hashrate_.store(0.0);

    logger_interface_->info("[MiningService] GPU mining thread stopped");
}

} // namespace dinero
