#pragma once
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>

// Forward declarations
class MiningEngine;
struct WorkTemplate;

/**
 * @brief Mining thread supervisor with proper lifecycle management
 * 
 * Features:
 * - Supervisor + workers model with std::thread
 * - Idempotent start()/stop() and dynamic setThreads(n)
 * - Clean shutdown with bounded time
 * - Thread-safe work distribution
 * - No resource leaks or dangling threads
 */
class MiningSupervisor {
public:
    enum class State {
        Idle,
        Starting,
        Running,
        Stopping
    };

    explicit MiningSupervisor(MiningEngine& engine);
    ~MiningSupervisor();

    // Lifecycle management
    bool start(int threads);
    void setThreads(int threads);
    void stop();

    // Work management
    void setJob(std::shared_ptr<WorkTemplate> job);

    // Status queries
    State getState() const { return m_state.load(); }
    int getThreadCount() const { return static_cast<int>(m_workers.size()); }
    bool isRunning() const { return m_state.load() == State::Running; }

private:
    // Thread management
    void resizeWorkersUnlocked(int target);
    void workerLoop();

    // Work processing
    void mineBatch(const WorkTemplate& job);
    uint64_t trySomeNonces(const WorkTemplate& job);

    // References
    MiningEngine& m_engine;

    // State management
    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_shouldStop{false};
    std::mutex m_mutex;
    std::condition_variable m_jobCondition;

    // Worker threads (std::thread for C++17 compatibility)
    std::vector<std::thread> m_workers;

    // Work distribution
    std::shared_ptr<WorkTemplate> m_currentJob{nullptr};
    std::mutex m_jobMutex;

    // Constants
    static constexpr std::chrono::milliseconds BATCH_DURATION{250};
    static constexpr uint32_t NONCES_PER_BATCH{1000};
};