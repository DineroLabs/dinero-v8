/**
 * Phase 26.9: Worker Interface Implementation
 *
 * Wires external miners to MiningCoordinator:
 * - CPU workers
 * - GPU workers
 * - Stratum bridges
 */

#include "mining/worker_interface.h"
#include "common/logger.h"
#include "common/sha256d.h"
#include <chrono>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace mining {

// ============================================================================
// CPU Worker Implementation
// ============================================================================

CpuWorker::CpuWorker(
    MiningCoordinator* coordinator,
    const std::string& worker_id,
    int thread_count
)
    : coordinator_(coordinator)
    , worker_id_(worker_id)
    , thread_count_(thread_count)
    , is_running_(false)
{
    if (!coordinator_) {
        throw std::runtime_error("CpuWorker: MiningCoordinator is null");
    }

    if (thread_count_ <= 0) {
        thread_count_ = 1;
    }
}

CpuWorker::~CpuWorker() {
    stop();
}

bool CpuWorker::start() {
    if (is_running_.load()) {
        g_logger.warning("[CpuWorker] Already running");
        return false;
    }

    // Register with coordinator
    coordinator_->registerWorker(worker_id_, MiningCoordinator::WorkerType::CPU);

    // Start worker threads
    is_running_.store(true);

    for (int i = 0; i < thread_count_; i++) {
        std::string thread_id_str = worker_id_ + "_thread_" + std::to_string(i);
        worker_threads_.emplace_back(
            std::make_unique<std::thread>(&CpuWorker::miningLoop, this, i)
        );
    }

    g_logger.info("[CpuWorker] Started " + std::to_string(thread_count_) + " threads for worker " + worker_id_);

    return true;
}

void CpuWorker::stop() {
    if (!is_running_.load()) {
        return;
    }

    is_running_.store(false);

    // Wait for threads to finish
    for (auto& thread : worker_threads_) {
        if (thread && thread->joinable()) {
            thread->join();
        }
    }

    worker_threads_.clear();

    // Unregister from coordinator
    coordinator_->unregisterWorker(worker_id_);

    g_logger.info("[CpuWorker] Stopped worker " + worker_id_);
}

void CpuWorker::miningLoop(int thread_id) {
    std::string thread_worker_id = worker_id_ + "_t" + std::to_string(thread_id);

    g_logger.info("[CpuWorker] Thread " + std::to_string(thread_id) + " started");

    // Nonce range for this thread
    const uint32_t NONCE_RANGE = 0xFFFFFFFF / thread_count_;
    uint32_t nonce_start = thread_id * NONCE_RANGE;
    uint32_t nonce_end = nonce_start + NONCE_RANGE;

    while (is_running_.load()) {
        // Get current job from coordinator
        auto job = coordinator_->getCurrentJob();
        if (!job) {
            // No job available, wait
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Mine this job (simplified CPU mining loop)
        for (uint32_t nonce = nonce_start; nonce < nonce_end && is_running_.load(); nonce++) {
            // Build block header
            // TODO: Use BlockTemplateManager to build proper header
            // For now, just simulate work

            // Hash counter (for hashrate tracking)
            // TODO: Track actual hashes and submit to coordinator

            // Check if we need to refresh job
            auto current_job = coordinator_->getCurrentJob();
            if (!current_job || current_job->job_id != job->job_id) {
                // Job changed, get new job
                break;
            }

            // Simulate hash check (placeholder)
            // TODO: Actual SHA256d hashing and difficulty check

            // Sleep briefly to avoid burning CPU (remove for production)
            if (nonce % 10000 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        }
    }

    g_logger.info("[CpuWorker] Thread " + std::to_string(thread_id) + " stopped");
}

MiningCoordinator::WorkerStats CpuWorker::getStats() const {
    // Get stats from coordinator
    auto all_stats = coordinator_->getWorkerStats();

    for (const auto& stats : all_stats) {
        if (stats.worker_id == worker_id_) {
            return stats;
        }
    }

    // Return empty stats if not found
    MiningCoordinator::WorkerStats stats;
    stats.worker_id = worker_id_;
    stats.type = MiningCoordinator::WorkerType::CPU;
    stats.shares_accepted = 0;
    stats.shares_rejected = 0;
    stats.blocks_found = 0;
    stats.hashrate = 0.0;
    stats.difficulty = 1.0;
    stats.last_share_time = 0;

    return stats;
}

// ============================================================================
// GPU Worker Implementation
// ============================================================================

GpuWorker::GpuWorker(
    MiningCoordinator* coordinator,
    const std::string& worker_id,
    int device_id
)
    : coordinator_(coordinator)
    , worker_id_(worker_id)
    , device_id_(device_id)
    , is_running_(false)
{
    if (!coordinator_) {
        throw std::runtime_error("GpuWorker: MiningCoordinator is null");
    }
}

GpuWorker::~GpuWorker() {
    stop();
}

bool GpuWorker::start() {
    if (is_running_.load()) {
        g_logger.warning("[GpuWorker] Already running");
        return false;
    }

    // Register with coordinator
    coordinator_->registerWorker(worker_id_, MiningCoordinator::WorkerType::GPU);

    // Start worker thread
    is_running_.store(true);
    worker_thread_ = std::make_unique<std::thread>(&GpuWorker::miningLoop, this);

    g_logger.info("[GpuWorker] Started GPU worker " + worker_id_ + " on device " + std::to_string(device_id_));

    return true;
}

void GpuWorker::stop() {
    if (!is_running_.load()) {
        return;
    }

    is_running_.store(false);

    if (worker_thread_ && worker_thread_->joinable()) {
        worker_thread_->join();
    }

    worker_thread_.reset();

    // Unregister from coordinator
    coordinator_->unregisterWorker(worker_id_);

    g_logger.info("[GpuWorker] Stopped worker " + worker_id_);
}

void GpuWorker::miningLoop() {
    g_logger.info("[GpuWorker] GPU mining thread started for device " + std::to_string(device_id_));

    while (is_running_.load()) {
        // Get current job from coordinator
        auto job = coordinator_->getCurrentJob();
        if (!job) {
            // No job available, wait
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // TODO: Integrate with existing GPU mining infrastructure
        // - Initialize GPU kernel
        // - Submit batch jobs
        // - Collect results
        // - Submit shares to coordinator

        // For now, just simulate GPU work
        g_logger.info("[GpuWorker] Processing job " + job->job_id + " on GPU device " + std::to_string(device_id_));

        // Simulate GPU batch processing
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    g_logger.info("[GpuWorker] GPU mining thread stopped");
}

MiningCoordinator::WorkerStats GpuWorker::getStats() const {
    // Get stats from coordinator
    auto all_stats = coordinator_->getWorkerStats();

    for (const auto& stats : all_stats) {
        if (stats.worker_id == worker_id_) {
            return stats;
        }
    }

    // Return empty stats if not found
    MiningCoordinator::WorkerStats stats;
    stats.worker_id = worker_id_;
    stats.type = MiningCoordinator::WorkerType::GPU;
    stats.shares_accepted = 0;
    stats.shares_rejected = 0;
    stats.blocks_found = 0;
    stats.hashrate = 0.0;
    stats.difficulty = 1.0;
    stats.last_share_time = 0;

    return stats;
}

// ============================================================================
// Stratum Worker Bridge Implementation
// ============================================================================

StratumWorkerBridge::StratumWorkerBridge(MiningCoordinator* coordinator)
    : coordinator_(coordinator)
{
    if (!coordinator_) {
        throw std::runtime_error("StratumWorkerBridge: MiningCoordinator is null");
    }
}

void StratumWorkerBridge::onWorkerConnected(
    const std::string& session_id,
    const std::string& worker_name,
    bool is_v2
) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    SessionInfo info;
    info.worker_name = worker_name;
    info.is_v2 = is_v2;

    // Generate extranonce1 for this session
    std::hash<std::string> hasher;
    size_t hash = hasher(session_id);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(8) << (hash & 0xFFFFFFFF);
    info.extranonce1 = oss.str();

    sessions_[session_id] = info;

    // Register with coordinator
    auto worker_type = is_v2
        ? MiningCoordinator::WorkerType::STRATUM_V2
        : MiningCoordinator::WorkerType::STRATUM_V1;

    coordinator_->registerWorker(session_id, worker_type);

    g_logger.info("[StratumBridge] Worker connected: " + worker_name +
                  " (session=" + session_id + ", v2=" + (is_v2 ? "yes" : "no") + ")");
}

void StratumWorkerBridge::onWorkerDisconnected(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // Unregister from coordinator
    coordinator_->unregisterWorker(session_id);

    // Remove session
    sessions_.erase(session_id);

    g_logger.info("[StratumBridge] Worker disconnected: " + session_id);
}

std::shared_ptr<MiningJob> StratumWorkerBridge::getJob(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // Check if session exists
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        g_logger.warning("[StratumBridge] Job request from unknown session: " + session_id);
        return nullptr;
    }

    // Get current job from coordinator
    auto job = coordinator_->getCurrentJob();
    if (!job) {
        g_logger.warning("[StratumBridge] No current job available");
        return nullptr;
    }

    // Set extranonce1 for this session
    job->extranonce1 = it->second.extranonce1;

    return job;
}

bool StratumWorkerBridge::submitShare(
    const std::string& session_id,
    const ShareSubmission& share
) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    // Check if session exists
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        g_logger.warning("[StratumBridge] Share from unknown session: " + session_id);
        return false;
    }

    // Determine worker type
    auto worker_type = it->second.is_v2
        ? MiningCoordinator::WorkerType::STRATUM_V2
        : MiningCoordinator::WorkerType::STRATUM_V1;

    // Submit to coordinator
    return coordinator_->submitShare(share, session_id, worker_type);
}

void StratumWorkerBridge::setDifficulty(const std::string& session_id, double difficulty) {
    coordinator_->setWorkerDifficulty(session_id, difficulty);
}

double StratumWorkerBridge::getRecommendedDifficulty(
    const std::string& session_id,
    double hashrate
) {
    return coordinator_->calculateRecommendedDifficulty(session_id, hashrate);
}

} // namespace mining
} // namespace dinero
