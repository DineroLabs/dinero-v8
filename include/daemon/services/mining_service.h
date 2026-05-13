#pragma once
#include "daemon/iservice.h"
// Phase C: Legacy Mining class removed
#include "mining/mining_manager_v2.h"  // Phase C: New MiningManager
#include "primitives/block.h"  // Phase 2: For Block return type
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

// Forward declarations for GPU mining
namespace dinero {
namespace gpu {
    class IComputeBackend;
    class GPUDeviceManager;
}
}

namespace dinero {

// Forward declarations
class ILogger;
class IConsensusEngine;

/**
 * MiningService - IService wrapper for MiningManager
 *
 * Phase C: Wraps MiningManager v2 (job-based, IService pattern)
 * Legacy Mining class removed - all functionality now in MiningManager v2
 *
 * Dependencies: Logger, Config, Chainstate, Mempool
 *
 * Initialization order:
 * - Init() creates MiningManager v2 instance and wires dependencies
 * - Start() delegates to MiningManager lifecycle
 * - Stop() delegates to MiningManager graceful shutdown
 *
 * Week 5: Integrated telemetry updates to MetricsRegistry
 * Phase C: API remains compatible, implementation delegates to MiningManager v2
 */
class MiningService : public IService {
public:
    MiningService();
    ~MiningService() override;

    std::string Name() const override { return "Mining"; }

    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Service health
    bool IsHealthy() const override;
    std::string GetMetrics() const override;

    // Phase C: Access to MiningManager v2
    MiningManager& getMiningManager() { return *mining_manager_v2_; }
    const MiningManager& getMiningManager() const { return *mining_manager_v2_; }

    // Forward commonly used methods (Phase C: Delegates to MiningManager v2)
    bool isMiningEnabled() const {
        return mining_manager_v2_ ? mining_manager_v2_->isMining() : false;
    }

    bool startMining() {
        return mining_manager_v2_ ? mining_manager_v2_->startMining() : false;
    }

    void stopMining() {
        if (mining_manager_v2_) mining_manager_v2_->stopMining();
    }

    // Phase 2: Create block template using consensus engine (if available)
    // Falls back to mining_->createBlockTemplate() if consensus engine not available
    Block createBlockTemplate(const DaemonContext& ctx);

    void setMiningAddress(const std::string& address) {
        if (mining_manager_v2_) mining_manager_v2_->setMiningAddress(address);
    }

    std::string getMiningAddress() const {
        return mining_manager_v2_ ? mining_manager_v2_->getMiningAddress() : "";
    }

    double getHashrate() const {
        if (!mining_manager_v2_) return 0.0;
        return mining_manager_v2_->getStats().current_hashrate.load();
    }

    uint64_t getBlocksFound() const {
        if (!mining_manager_v2_) return 0;
        return mining_manager_v2_->getStats().blocks_found.load();
    }
    
    // Access to consensus engine (Phase 2: Modular consensus)
    IConsensusEngine* getConsensusEngine() const { return consensus_engine_.get(); }

    // GPU Mining control (Unified Template Architecture)
    bool startGPUMining(uint32_t device_id = 0);
    void stopGPUMining();
    bool isGPUMiningEnabled() const { return gpu_enabled_; }
    double getGPUHashrate() const;
    bool hasGPU() const;

private:
    // Phase C: MiningManager v2 only (legacy Mining class removed)
    std::unique_ptr<MiningManager> mining_manager_v2_;
    std::unique_ptr<IConsensusEngine> consensus_engine_;  // Phase 2: Modular consensus

    // GPU Mining infrastructure (Unified Template Architecture)
    std::unique_ptr<dinero::gpu::GPUDeviceManager> gpu_device_manager_;
    std::unique_ptr<dinero::gpu::IComputeBackend> gpu_backend_;
    std::thread gpu_mining_thread_;
    std::atomic<bool> gpu_enabled_{false};
    std::atomic<double> gpu_hashrate_{0.0};
    std::atomic<bool> gpu_should_stop_{false};

    // GPU mining worker thread (pulls from MiningService::createBlockTemplate)
    void GPUMiningLoop();

    // Dependencies from context
    // Logger dependencies (dual pattern during migration):
    // - logger_: Legacy LoggerService (keep for compatibility during migration)
    // - logger_interface_: New ILogger dependency injection (actively used)
    std::shared_ptr<class LoggerService> logger_;
    class ILogger* logger_interface_ = nullptr;

    std::shared_ptr<class ConfigService> config_;
    std::shared_ptr<class ChainstateService> chainstate_;
    std::shared_ptr<class MempoolService> mempool_;

    bool started_ = false;
    bool mining_enabled_ = false;

    // Week 5: Miner instance ID for metrics tracking
    std::string miner_id_;

    // Week 5: Telemetry update thread
    std::unique_ptr<std::thread> telemetry_thread_;
    std::atomic<bool> telemetry_running_{false};
    std::chrono::steady_clock::time_point start_time_;

    // Week 5: Update MetricsRegistry with current mining stats
    void UpdateTelemetry();
    
    // Week 5: Background thread that periodically updates metrics
    void TelemetryUpdateLoop();
};

} // namespace dinero
