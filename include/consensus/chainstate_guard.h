#pragma once

/**
 * Phase 6B: Chainstate Write Protection
 *
 * ChainstateGuard - Thread-safe access control for UTXO set and block index
 *
 * Problem:
 *   Parallel validation requires READ access to UTXO set (check if UTXOs exist)
 *   Block application requires WRITE access (spend/create UTXOs)
 *   Without synchronization, race conditions corrupt the chainstate
 *
 * Solution:
 *   Shared-exclusive lock (readers-writer lock)
 *   - Multiple readers allowed (parallel validation)
 *   - Single writer allowed (block application)
 *   - Readers and writer are mutually exclusive
 *
 * Usage:
 *   // Validation (read-only, concurrent)
 *   {
 *       auto lock = g_chainstate_guard.readLock();
 *       bool exists = utxo_set->Exists(txid, vout);
 *   }
 *
 *   // Apply block (write, exclusive)
 *   {
 *       auto lock = g_chainstate_guard.writeLock();
 *       utxo_set->SpendUTXO(txid, vout);
 *       utxo_set->AddUTXO(new_utxo);
 *   }
 *
 * Thread Safety:
 *   ✅ std::shared_mutex ensures readers/writer exclusivity
 *   ✅ RAII locks (std::shared_lock, std::unique_lock) prevent deadlocks
 *   ✅ No manual lock/unlock (exception-safe)
 */

#include <shared_mutex>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <optional>
#include <string>
#include <sstream>

namespace dinero {
namespace consensus {

/**
 * ChainstateGuard - Protects UTXO set and block index from concurrent mutations
 */
class ChainstateGuard {
public:
    ChainstateGuard() = default;

    // No copying or moving
    ChainstateGuard(const ChainstateGuard&) = delete;
    ChainstateGuard& operator=(const ChainstateGuard&) = delete;

    /**
     * Acquire read lock (shared, multiple readers allowed)
     * Use for: UTXO lookups, block index queries
     */
    std::shared_lock<std::shared_mutex> readLock() {
        active_readers_.fetch_add(1);
        auto lock = std::shared_lock<std::shared_mutex>(mutex_);
        return lock;
    }

    /**
     * Acquire write lock (exclusive, single writer)
     * Use for: UTXO mutations, block connection, chainstate updates
     */
    std::unique_lock<std::shared_mutex> writeLock() {
        active_writers_.fetch_add(1);
        auto lock = std::unique_lock<std::shared_mutex>(mutex_);
        return lock;
    }

    /**
     * Try to acquire read lock with timeout
     * Returns empty optional if timeout
     *
     * Platform note: macOS std::shared_mutex doesn't support timed operations,
     * so we implement a polling-based timeout for cross-platform compatibility
     */
    std::optional<std::shared_lock<std::shared_mutex>> tryReadLock(std::chrono::milliseconds timeout) {
        active_readers_.fetch_add(1);
        std::shared_lock<std::shared_mutex> lock(mutex_, std::defer_lock);

        // Try to acquire lock with polling (cross-platform compatible)
        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (lock.try_lock()) {
                return lock;
            }

            // Check timeout
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                active_readers_.fetch_sub(1);
                return std::nullopt;
            }

            // Brief sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    /**
     * Try to acquire write lock with timeout
     *
     * Platform note: Uses polling for cross-platform compatibility
     */
    std::optional<std::unique_lock<std::shared_mutex>> tryWriteLock(std::chrono::milliseconds timeout) {
        active_writers_.fetch_add(1);
        std::unique_lock<std::shared_mutex> lock(mutex_, std::defer_lock);

        // Try to acquire lock with polling (cross-platform compatible)
        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (lock.try_lock()) {
                return lock;
            }

            // Check timeout
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                active_writers_.fetch_sub(1);
                return std::nullopt;
            }

            // Brief sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Statistics
    struct Stats {
        uint64_t active_readers;
        uint64_t active_writers;
        uint64_t total_read_locks;
        uint64_t total_write_locks;

        std::string toString() const {
            std::ostringstream oss;
            oss << "ChainstateGuard::Stats {\n";
            oss << "  Active readers: " << active_readers << "\n";
            oss << "  Active writers: " << active_writers << "\n";
            oss << "  Total read locks: " << total_read_locks << "\n";
            oss << "  Total write locks: " << total_write_locks << "\n";
            oss << "}";
            return oss.str();
        }
    };

    Stats getStats() const {
        Stats stats;
        stats.active_readers = active_readers_.load();
        stats.active_writers = active_writers_.load();
        stats.total_read_locks = total_read_locks_.load();
        stats.total_write_locks = total_write_locks_.load();
        return stats;
    }

private:
    // Shared-exclusive mutex (readers-writer lock)
    mutable std::shared_mutex mutex_;

    // Metrics
    mutable std::atomic<uint64_t> active_readers_{0};
    mutable std::atomic<uint64_t> active_writers_{0};
    mutable std::atomic<uint64_t> total_read_locks_{0};
    mutable std::atomic<uint64_t> total_write_locks_{0};
};

/**
 * ScopedReadLock - RAII wrapper for read lock
 */
class ScopedReadLock {
public:
    explicit ScopedReadLock(ChainstateGuard& guard)
        : lock_(guard.readLock()) {}

    // No copying or moving
    ScopedReadLock(const ScopedReadLock&) = delete;
    ScopedReadLock& operator=(const ScopedReadLock&) = delete;

private:
    std::shared_lock<std::shared_mutex> lock_;
};

/**
 * ScopedWriteLock - RAII wrapper for write lock
 */
class ScopedWriteLock {
public:
    explicit ScopedWriteLock(ChainstateGuard& guard)
        : lock_(guard.writeLock()) {}

    // No copying or moving
    ScopedWriteLock(const ScopedWriteLock&) = delete;
    ScopedWriteLock& operator=(const ScopedWriteLock&) = delete;

private:
    std::unique_lock<std::shared_mutex> lock_;
};

// Global chainstate guard (initialized in main)
// extern ChainstateGuard g_chainstate_guard;

} // namespace consensus
} // namespace dinero
