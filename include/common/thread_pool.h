#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>

namespace dinero {

/**
 * @class ThreadPool
 * @brief Production-grade thread pool for asynchronous task execution
 *
 * Features:
 * - Fixed-size worker thread pool
 * - Task queue with FIFO execution
 * - Graceful shutdown with task draining
 * - Exception isolation (tasks can't crash workers)
 * - Future-based result retrieval
 *
 * Thread Safety: All methods are thread-safe
 *
 * Usage:
 *   ThreadPool pool(4);  // 4 worker threads
 *   pool.enqueue([]() { doWork(); });
 *   pool.shutdown();  // Wait for all tasks to complete
 */
class ThreadPool {
public:
    /**
     * @brief Construct thread pool with specified number of workers
     * @param num_threads Number of worker threads (default: hardware concurrency)
     */
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());

    /**
     * @brief Destructor - automatically shuts down pool and joins threads
     */
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * @brief Enqueue a task for asynchronous execution
     * @param task Callable object (function, lambda, etc.)
     * @return std::future<ReturnType> Future for retrieving result
     *
     * Example:
     *   auto result = pool.enqueue([]() { return 42; });
     *   int value = result.get();  // Blocks until task completes
     */
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type>;

    /**
     * @brief Get number of worker threads
     */
    size_t size() const { return m_workers.size(); }

    /**
     * @brief Get approximate number of queued tasks
     * Note: This is a snapshot and may change immediately after return
     */
    size_t queueSize() const {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        return m_tasks.size();
    }

    /**
     * @brief Initiate graceful shutdown
     * - Stops accepting new tasks
     * - Allows queued tasks to complete
     * - Joins all worker threads
     */
    void shutdown();

    /**
     * @brief Check if pool is shutting down
     */
    bool isShuttingDown() const { return m_stop.load(); }

private:
    // Worker threads
    std::vector<std::thread> m_workers;

    // Task queue
    std::queue<std::function<void()>> m_tasks;
    mutable std::mutex m_queue_mutex;
    std::condition_variable m_condition;

    // Shutdown flag
    std::atomic<bool> m_stop{false};

    /**
     * @brief Worker thread main loop
     */
    void workerLoop();
};

// ═══════════════════════════════════════════════════════════════════════════
// Template Implementation
// ═══════════════════════════════════════════════════════════════════════════

template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;

    // Create packaged_task for future result retrieval
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> res = task->get_future();

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);

        // Don't enqueue if shutting down
        if (m_stop.load()) {
            throw std::runtime_error("ThreadPool::enqueue called on stopped pool");
        }

        m_tasks.emplace([task]() {
            try {
                (*task)();
            } catch (const std::exception& e) {
                // Swallow exception to prevent worker thread crash
                // Logging should happen in the task itself
            } catch (...) {
                // Swallow unknown exceptions
            }
        });
    }

    m_condition.notify_one();
    return res;
}

} // namespace dinero
