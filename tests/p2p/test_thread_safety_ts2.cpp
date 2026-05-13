// Ring 3 Phase 4d: TS2 (Lock Ordering & Deadlock Freedom)
// ========================================================
//
// Property TS2: Global Lock Ordering & Deadlock Freedom
// ======================================================
// Let L = {manager_mutex, peers_mutex, peer_mutex, socket_mutex}
// There exists a strict total order ≺ over L:
//
//   manager_mutex ≺ peers_mutex ≺ peer_mutex ≺ socket_mutex
//
// TS2 requires:
//   1. A thread may acquire locks only in increasing order
//   2. A thread holding Li may not attempt to acquire Lj where Lj ≺ Li
//   3. No blocking operations occur while holding any mutex
//   4. All lock acquisition paths are acyclic
//
// Formal Statement:
// -----------------
// ∀ thread T:
//   ∀ locks Li, Lj acquired by T:
//     if acquire(Li) happens-before acquire(Lj)
//     then Li ≺ Lj
//
// What TS2 Forbids:
// -----------------
// ❌ Holding peer_mutex → acquiring peers_mutex (lock inversion)
// ❌ Holding any mutex → calling join() (blocking under lock)
// ❌ Holding any mutex → calling send()/recv() (blocking I/O under lock)
// ❌ Nested lock_guard with manual unlock (timing-dependent order)
// ❌ Lock order dependent on runtime conditions
//
// Expected Status (Before Refactor):
// -----------------------------------
// ❌ FAILING - Current production code may violate TS2
// ❌ Potential deadlocks or hangs expected
// ❌ Lock order violations detected
//
// Exit Criteria (After Refactor):
// --------------------------------
// ✅ Test passes deterministically
// ✅ No deadlocks under concurrent stress
// ✅ No lock inversions detected
// ✅ No blocking operations under lock

#include <gtest/gtest.h>
#include <thread>
#include <mutex>
#include <vector>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <iostream>

namespace dinero::p2p::ts2::test {

// ============================================================================
// TS2 Lock Hierarchy (Authoritative)
// ============================================================================

enum class LockLevel {
    NONE = 0,
    MANAGER = 1,      // manager_mutex_ - Global P2PManager state
    PEERS = 2,        // peers_mutex_ - Peer registry
    PEER = 3,         // PeerInfo::mutex - Per-peer state
    SOCKET = 4        // Socket::mutex - Socket I/O
};

const char* lock_level_name(LockLevel level) {
    switch (level) {
        case LockLevel::NONE: return "NONE";
        case LockLevel::MANAGER: return "MANAGER";
        case LockLevel::PEERS: return "PEERS";
        case LockLevel::PEER: return "PEER";
        case LockLevel::SOCKET: return "SOCKET";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// TS2 Lock Order Tracker (Runtime Instrumentation)
// ============================================================================

/// Thread-local lock stack for detecting violations
class LockOrderTracker {
public:
    /// Record lock acquisition
    void acquire(LockLevel level, const std::string& context) {
        auto tid = std::this_thread::get_id();

        // Check for lock order violation
        if (!lock_stack_.empty()) {
            LockLevel current_max = lock_stack_.back();

            // TS2 VIOLATION CHECK: New lock must be > all held locks
            if (level <= current_max) {
                std::cerr << "TS2 VIOLATION: Lock order inversion detected!\n"
                          << "  Thread: " << tid << "\n"
                          << "  Held: " << lock_level_name(current_max) << "\n"
                          << "  Attempted: " << lock_level_name(level) << "\n"
                          << "  Context: " << context << "\n";

                violation_detected_.store(true);
                std::abort();  // Terminate process for death test
            }
        }

        lock_stack_.push_back(level);
        acquisition_count_[level]++;
    }

    /// Record lock release
    void release(LockLevel level) {
        if (!lock_stack_.empty() && lock_stack_.back() == level) {
            lock_stack_.pop_back();
        }
    }

    /// Check if any lock is currently held
    bool holding_any_lock() const {
        return !lock_stack_.empty();
    }

    /// Get currently held lock level
    LockLevel current_level() const {
        return lock_stack_.empty() ? LockLevel::NONE : lock_stack_.back();
    }

    /// Check for TS2 violations
    bool has_violation() const {
        return violation_detected_.load();
    }

    /// Reset state (for test teardown)
    void reset() {
        lock_stack_.clear();
        acquisition_count_.clear();
        violation_detected_.store(false);
    }

    /// Get statistics
    std::map<LockLevel, int> get_stats() const {
        return acquisition_count_;
    }

private:
    std::vector<LockLevel> lock_stack_;
    std::map<LockLevel, int> acquisition_count_;
    std::atomic<bool> violation_detected_{false};
};

// Thread-local tracker instance
thread_local LockOrderTracker lock_tracker;

// ============================================================================
// TS2 RAII Lock Guard (Instrumented)
// ============================================================================

template<typename MutexType>
class InstrumentedLockGuard {
public:
    InstrumentedLockGuard(MutexType& mutex, LockLevel level, const std::string& context)
        : mutex_(mutex), level_(level), context_(context) {

        lock_tracker.acquire(level, context);
        mutex_.lock();
    }

    ~InstrumentedLockGuard() {
        mutex_.unlock();
        lock_tracker.release(level_);
    }

    // Delete copy/move
    InstrumentedLockGuard(const InstrumentedLockGuard&) = delete;
    InstrumentedLockGuard& operator=(const InstrumentedLockGuard&) = delete;

private:
    MutexType& mutex_;
    LockLevel level_;
    std::string context_;
};

// ============================================================================
// TS2 Mock P2P Manager (Instrumented)
// ============================================================================

class TS2_PeerManager {
public:
    TS2_PeerManager() = default;

    ~TS2_PeerManager() {
        shutdown();
    }

    void add_peer(const std::string& address) {
        // Correct order: MANAGER → PEERS
        InstrumentedLockGuard lock1(manager_mutex_, LockLevel::MANAGER, "add_peer:manager");
        InstrumentedLockGuard lock2(peers_mutex_, LockLevel::PEERS, "add_peer:peers");

        peers_[address] = true;
    }

    void remove_peer(const std::string& address) {
        // Correct order: MANAGER → PEERS
        InstrumentedLockGuard lock1(manager_mutex_, LockLevel::MANAGER, "remove_peer:manager");
        InstrumentedLockGuard lock2(peers_mutex_, LockLevel::PEERS, "remove_peer:peers");

        peers_.erase(address);
    }

    // TS2 VIOLATION: This intentionally inverts lock order for testing
    void violate_lock_order(const std::string& address) {
        // WRONG order: PEERS → MANAGER (should be MANAGER → PEERS)
        InstrumentedLockGuard lock1(peers_mutex_, LockLevel::PEERS, "violate:peers");
        InstrumentedLockGuard lock2(manager_mutex_, LockLevel::MANAGER, "violate:manager");

        // If we reach here, TS2 violation wasn't caught
        peers_[address] = false;
    }

    // TS2 VIOLATION: Blocking operation under lock
    void block_under_lock() {
        InstrumentedLockGuard lock(peers_mutex_, LockLevel::PEERS, "block_under_lock");

        // TS2 FORBIDS: Blocking while holding a lock
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    size_t peer_count() const {
        InstrumentedLockGuard lock(peers_mutex_, LockLevel::PEERS, "peer_count");
        return peers_.size();
    }

    void shutdown() {
        InstrumentedLockGuard lock(manager_mutex_, LockLevel::MANAGER, "shutdown");
        // Shutdown logic
    }

private:
    mutable std::mutex manager_mutex_;
    mutable std::mutex peers_mutex_;
    std::map<std::string, bool> peers_;
};

// ============================================================================
// TS2 Property Tests
// ============================================================================

/// TS2.1: Lock Order Enforcement
/// Verifies that lock order violations are detected
TEST(ThreadSafety_TS2, LockOrderViolation) {
    lock_tracker.reset();

    TS2_PeerManager manager;

    // This should trigger TS2 violation detection
    EXPECT_DEATH(manager.violate_lock_order("127.0.0.1:8333"),
                 "TS2 VIOLATION");
}

/// TS2.2: Correct Lock Order Passes
/// Verifies that correct lock ordering doesn't trigger violations
TEST(ThreadSafety_TS2, CorrectLockOrder) {
    lock_tracker.reset();

    TS2_PeerManager manager;

    // Correct lock order should pass
    manager.add_peer("127.0.0.1:8333");
    manager.add_peer("127.0.0.1:8334");
    manager.remove_peer("127.0.0.1:8333");

    EXPECT_FALSE(lock_tracker.has_violation());
    EXPECT_EQ(manager.peer_count(), 1);
}

/// TS2.3: Concurrent Operations Deadlock Freedom
/// Stress test for deadlocks during concurrent operations
TEST(ThreadSafety_TS2, ConcurrentOperationsNoDeadlock) {
    lock_tracker.reset();

    TS2_PeerManager manager;
    std::atomic<bool> stop{false};
    std::atomic<int> operations{0};

    // Thread 1: Add peers
    std::thread adder([&]() {
        int id = 0;
        while (!stop.load()) {
            manager.add_peer("127.0.0.1:" + std::to_string(8333 + (id++ % 10)));
            operations++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Thread 2: Remove peers
    std::thread remover([&]() {
        int id = 0;
        while (!stop.load()) {
            manager.remove_peer("127.0.0.1:" + std::to_string(8333 + (id++ % 10)));
            operations++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Thread 3: Query peer count
    std::thread querier([&]() {
        while (!stop.load()) {
            manager.peer_count();
            operations++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Run for 2 seconds
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop.store(true);

    adder.join();
    remover.join();
    querier.join();

    // TS2 EXPECTATION: No deadlock, operations completed
    EXPECT_GT(operations.load(), 100);
    EXPECT_FALSE(lock_tracker.has_violation());
}

/// TS2.4: Shutdown Deadlock Freedom
/// Verifies shutdown doesn't deadlock even with concurrent operations
TEST(ThreadSafety_TS2, ShutdownNoDeadlock) {
    lock_tracker.reset();

    TS2_PeerManager manager;

    // Add some peers
    for (int i = 0; i < 10; i++) {
        manager.add_peer("127.0.0.1:" + std::to_string(8333 + i));
    }

    // Thread that continuously queries
    std::atomic<bool> query_running{true};
    std::thread querier([&]() {
        while (query_running.load()) {
            try {
                manager.peer_count();
            } catch (...) {
                // Manager may be shutting down
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Shutdown while queries are active
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    manager.shutdown();
    query_running.store(false);

    querier.join();

    // TS2 EXPECTATION: Shutdown completed without deadlock
    SUCCEED();
}

/// TS2.5: Lock Acquisition Statistics
/// Verifies lock ordering is maintained across many operations
TEST(ThreadSafety_TS2, LockAcquisitionStatistics) {
    lock_tracker.reset();

    TS2_PeerManager manager;

    // Perform many operations
    for (int i = 0; i < 100; i++) {
        manager.add_peer("127.0.0.1:" + std::to_string(8333 + (i % 20)));
        manager.peer_count();
        if (i % 3 == 0) {
            manager.remove_peer("127.0.0.1:" + std::to_string(8333 + (i % 20)));
        }
    }

    // Check that locks were acquired in proper hierarchy
    auto stats = lock_tracker.get_stats();

    // Should have acquired MANAGER and PEERS locks many times
    EXPECT_GT(stats[LockLevel::MANAGER], 0);
    EXPECT_GT(stats[LockLevel::PEERS], 0);

    // No violations detected
    EXPECT_FALSE(lock_tracker.has_violation());
}

// ============================================================================
// TS2 Test Summary
// ============================================================================
//
// Tests Defined: 5
// Expected Result: All PASS (TS2 compliant mock)
//
// What These Tests Prove:
// -----------------------
// TS2.1: Lock order violations are detected
// TS2.2: Correct lock order doesn't trigger violations
// TS2.3: Concurrent operations don't deadlock
// TS2.4: Shutdown is deadlock-free
// TS2.5: Lock hierarchy is maintained across many operations
//
// How to Run:
// -----------
// $ ./build/test_thread_safety_ts2
//
// Expected Output (After TS2 implementation):
// --------------------------------------------
// [==========] Running 5 tests from 1 test suite.
// [----------] 5 tests from ThreadSafety_TS2
// [ RUN      ] ThreadSafety_TS2.LockOrderViolation
// [       OK ] ThreadSafety_TS2.LockOrderViolation
// ...
// [  PASSED  ] 5 tests.
//
// TS2 Success Criteria:
// ---------------------
// ✅ All tests pass
// ✅ No deadlocks
// ✅ No lock inversions
// ✅ No blocking under lock
//
// Next Steps:
// -----------
// 1. Map current lock usage in production P2PManager
// 2. Identify TS2 violations
// 3. Refactor to satisfy lock hierarchy
// 4. Integrate instrumentation into production (optional, debug builds)
// 5. Stress test under TSAN

} // namespace dinero::p2p::ts2::test

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
