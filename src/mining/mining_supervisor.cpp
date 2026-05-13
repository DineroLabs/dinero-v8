#include "mining/mining_supervisor.h"
#include "daemon/mining_engine.h"
#include "common/logger.h"

#include <algorithm>
#include <chrono>

MiningSupervisor::MiningSupervisor(MiningEngine& engine)
    : m_engine(engine)
{
    dinero::g_logger.debug("MiningSupervisor created");
}

MiningSupervisor::~MiningSupervisor() {
    stop();
    dinero::g_logger.debug("MiningSupervisor destroyed");
}

bool MiningSupervisor::start(int threads) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state.load() == State::Running) {
        dinero::g_logger.warning("MiningSupervisor already running");
        return true;
    }
    
    dinero::g_logger.info("Starting MiningSupervisor with " + std::to_string(threads) + " threads");
    
    m_state.store(State::Starting);
    m_shouldStop.store(false);
    
    // Start worker threads
    resizeWorkersUnlocked(threads);
    
    m_state.store(State::Running);
    dinero::g_logger.info("MiningSupervisor started successfully");
    return true;
}

void MiningSupervisor::setThreads(int threads) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state.load() != State::Running) {
        dinero::g_logger.warning("Cannot set threads when not running");
        return;
    }
    
    dinero::g_logger.info("Resizing MiningSupervisor to " + std::to_string(threads) + " threads");
    resizeWorkersUnlocked(threads);
}

void MiningSupervisor::stop() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state.load() == State::Idle) {
        return; // Already stopped
    }
    
    dinero::g_logger.info("Stopping MiningSupervisor...");
    m_state.store(State::Stopping);
    m_shouldStop.store(true);
    
    // Notify all workers to wake up
    m_jobCondition.notify_all();
    
    // Wait for all workers to finish
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    m_workers.clear();
    m_state.store(State::Idle);
    dinero::g_logger.info("MiningSupervisor stopped");
}

void MiningSupervisor::setJob(std::shared_ptr<WorkTemplate> job) {
    std::lock_guard<std::mutex> jobLock(m_jobMutex);
    m_currentJob = job;
    
    // Notify workers of new job
    m_jobCondition.notify_all();
    
    dinero::g_logger.debug("New mining job set");
}

void MiningSupervisor::resizeWorkersUnlocked(int target) {
    const int current = static_cast<int>(m_workers.size());
    
    if (target == current) {
        return; // No change needed
    }
    
    if (target > current) {
        // Add workers
        for (int i = current; i < target; ++i) {
            m_workers.emplace_back([this]() {
                workerLoop();
            });
            dinero::g_logger.debug("Started mining worker " + std::to_string(i + 1));
        }
    } else {
        // Remove workers (they'll stop on next iteration)
        dinero::g_logger.debug("Reducing worker count from " + std::to_string(current) + " to " + std::to_string(target));
        // Workers will naturally stop when they check m_shouldStop
    }
}

void MiningSupervisor::workerLoop() {
    dinero::g_logger.debug("Mining worker started");
    
    while (!m_shouldStop.load()) {
        std::shared_ptr<WorkTemplate> job;
        
        // Get current job
        {
            std::unique_lock<std::mutex> jobLock(m_jobMutex);
            m_jobCondition.wait_for(jobLock, std::chrono::milliseconds(200), [&] {
                return m_shouldStop.load() || m_currentJob != nullptr;
            });
            
            if (m_shouldStop.load()) {
                break;
            }
            
            job = m_currentJob;
        }
        
        if (job) {
            mineBatch(*job);
        }
    }
    
    dinero::g_logger.debug("Mining worker stopped");
}

void MiningSupervisor::mineBatch(const WorkTemplate& job) {
    const auto startTime = std::chrono::steady_clock::now();
    uint64_t hashesComputed = 0;
    
    // Mine for the batch duration
    while (!m_shouldStop.load()) {
        const auto batchStart = std::chrono::steady_clock::now();
        
        // Try some nonces
        uint64_t batchHashes = trySomeNonces(job);
        hashesComputed += batchHashes;
        
        // Check if we should stop (time limit or stop signal)
        const auto batchEnd = std::chrono::steady_clock::now();
        const auto batchDuration = std::chrono::duration_cast<std::chrono::milliseconds>(batchEnd - batchStart);
        
        if (batchDuration >= BATCH_DURATION || m_shouldStop.load()) {
            break;
        }
    }
    
    // Update statistics
    if (hashesComputed > 0) {
        dinero::g_logger.debug("Mined " + std::to_string(hashesComputed) + " hashes in batch");
    }
}

uint64_t MiningSupervisor::trySomeNonces(const WorkTemplate& job) {
    uint64_t hashesComputed = 0;
    
    // Simple nonce incrementing for now
    // In a real implementation, this would do actual hash computation
    for (uint32_t i = 0; i < NONCES_PER_BATCH && !m_shouldStop.load(); ++i) {
        // Placeholder: increment nonce and check if it solves the block
        // This would involve:
        // 1. Update the block header with new nonce
        // 2. Hash the block header
        // 3. Check if hash meets difficulty target
        // 4. If solved, notify MiningEngine
        
        hashesComputed++;
        
        // Simulate some work
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    
    return hashesComputed;
}