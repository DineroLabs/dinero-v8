#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace dinero {

/**
 * Exclusive, idempotent entry gate for one-shot teardown.
 *
 * DaemonApp::Stop() is reachable from more than one thread — the FFI node
 * thread's shutdown-request path, fatal-error escalation, the destructor —
 * and its pre-gate guard was `if (!started_) return;` with started_ cleared
 * only at the END of teardown. Two entrants inside that window interleave a
 * double teardown: services destroyed under a still-running sibling Stop
 * (observed on-device 2026-07-15 as two "DaemonApp::Stop entered" sequences
 * followed by EXC_BAD_ACCESS in a libc++ hash table).
 *
 * Semantics:
 *  - The first TryEnter() returns true (the winner runs teardown).
 *  - Every concurrent or later TryEnter() blocks until MarkDone(), then
 *    returns false — no caller ever proceeds past a live teardown.
 *  - After MarkDone(), TryEnter() returns false immediately.
 */
class StopGate {
public:
    bool TryEnter() {
        if (!entered_.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return done_; });
        return false;
    }

    void MarkDone() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_ = true;
        }
        cv_.notify_all();
    }

    bool done() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return done_;
    }

private:
    std::atomic<bool> entered_{false};
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool done_ = false;
};

}  // namespace dinero
