#include "common/thread_pool.h"
#include <stdexcept>

namespace dinero {

ThreadPool::ThreadPool(size_t num_threads) {
    if (num_threads == 0) {
        throw std::invalid_argument("ThreadPool requires at least 1 thread");
    }

    m_workers.reserve(num_threads);

    for (size_t i = 0; i < num_threads; ++i) {
        m_workers.emplace_back([this] {
            workerLoop();
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_stop.load()) {
            return;  // Already shutting down
        }
        m_stop.store(true);
    }

    // Wake up all waiting threads
    m_condition.notify_all();

    // Join all worker threads
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);

            // Wait for task or shutdown signal
            m_condition.wait(lock, [this] {
                return m_stop.load() || !m_tasks.empty();
            });

            // Exit if shutting down and no tasks remain
            if (m_stop.load() && m_tasks.empty()) {
                return;
            }

            // Get next task
            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
        }

        // Execute task outside the lock
        if (task) {
            task();  // Exception guards are in enqueue() template
        }
    }
}

} // namespace dinero
