#pragma once

#include "mining/mining_coordinator.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace dinero {
namespace mining {

// ============================================================================
// Worker Interface (Base Class)
// ============================================================================

/**
 * Base interface for all mining workers
 *
 * Provides common interface for:
 * - CPU miners
 * - GPU miners
 * - Stratum clients
 * - External miners
 */
class IWorker {
public:
    virtual ~IWorker() = default;

    /**
     * Start worker
     */
    virtual bool start() = 0;

    /**
     * Stop worker
     */
    virtual void stop() = 0;

    /**
     * Check if worker is running
     */
    virtual bool isRunning() const = 0;

    /**
     * Get worker identifier
     */
    virtual std::string getWorkerId() const = 0;

    /**
     * Get worker type
     */
    virtual MiningCoordinator::WorkerType getWorkerType() const = 0;

    /**
     * Get worker statistics
     */
    virtual MiningCoordinator::WorkerStats getStats() const = 0;
};

// ============================================================================
// CPU Worker
// ============================================================================

/**
 * CPU mining worker
 *
 * Uses MiningEngine internally but coordinates through MiningCoordinator.
 */
class CpuWorker : public IWorker {
public:
    CpuWorker(
        MiningCoordinator* coordinator,
        const std::string& worker_id,
        int thread_count = 1
    );

    ~CpuWorker() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override { return is_running_.load(); }

    std::string getWorkerId() const override { return worker_id_; }
    MiningCoordinator::WorkerType getWorkerType() const override {
        return MiningCoordinator::WorkerType::CPU;
    }

    MiningCoordinator::WorkerStats getStats() const override;

private:
    MiningCoordinator* coordinator_;
    std::string worker_id_;
    int thread_count_;
    std::atomic<bool> is_running_;
    std::vector<std::unique_ptr<std::thread>> worker_threads_;

    // Mining loop (per thread)
    void miningLoop(int thread_id);
};

// ============================================================================
// GPU Worker
// ============================================================================

/**
 * GPU mining worker
 *
 * Integrates with existing GPU mining infrastructure.
 */
class GpuWorker : public IWorker {
public:
    GpuWorker(
        MiningCoordinator* coordinator,
        const std::string& worker_id,
        int device_id = 0
    );

    ~GpuWorker() override;

    bool start() override;
    void stop() override;
    bool isRunning() const override { return is_running_.load(); }

    std::string getWorkerId() const override { return worker_id_; }
    MiningCoordinator::WorkerType getWorkerType() const override {
        return MiningCoordinator::WorkerType::GPU;
    }

    MiningCoordinator::WorkerStats getStats() const override;

private:
    MiningCoordinator* coordinator_;
    std::string worker_id_;
    int device_id_;
    std::atomic<bool> is_running_;
    std::unique_ptr<std::thread> worker_thread_;

    // Mining loop (GPU)
    void miningLoop();
};

// ============================================================================
// Stratum Worker Bridge
// ============================================================================

/**
 * Stratum worker bridge
 *
 * Bridges Stratum server sessions to MiningCoordinator.
 * Used by StratumServer to submit shares from remote miners.
 */
class StratumWorkerBridge {
public:
    StratumWorkerBridge(MiningCoordinator* coordinator);

    /**
     * Handle new Stratum connection
     *
     * @param session_id        Stratum session ID
     * @param worker_name       Worker name (from mining.authorize)
     * @param is_v2             True if Stratum V2
     */
    void onWorkerConnected(
        const std::string& session_id,
        const std::string& worker_name,
        bool is_v2 = false
    );

    /**
     * Handle Stratum disconnection
     */
    void onWorkerDisconnected(const std::string& session_id);

    /**
     * Get job for Stratum client
     *
     * @param session_id        Stratum session ID
     * @return                  Mining job (Stratum-formatted)
     */
    std::shared_ptr<MiningJob> getJob(const std::string& session_id);

    /**
     * Submit share from Stratum client
     *
     * @param session_id        Stratum session ID
     * @param share             Share submission
     * @return                  True if valid
     */
    bool submitShare(
        const std::string& session_id,
        const ShareSubmission& share
    );

    /**
     * Set difficulty for Stratum session
     *
     * @param session_id        Stratum session ID
     * @param difficulty        Difficulty
     */
    void setDifficulty(const std::string& session_id, double difficulty);

    /**
     * Get recommended difficulty for session
     *
     * @param session_id        Stratum session ID
     * @param hashrate          Estimated hashrate (H/s)
     * @return                  Recommended difficulty
     */
    double getRecommendedDifficulty(
        const std::string& session_id,
        double hashrate
    );

private:
    MiningCoordinator* coordinator_;

    struct SessionInfo {
        std::string worker_name;
        bool is_v2;
        std::string extranonce1;
    };

    std::map<std::string, SessionInfo> sessions_;
    std::mutex sessions_mutex_;
};

} // namespace mining
} // namespace dinero
