#pragma once
/**
 * Recursive mutex that knows whether the calling thread holds it, so a
 * consensus invariant like "this runs under the activation lock" can be
 * ASSERTED in code rather than maintained by convention.
 *
 * Why this exists: ReconsiderBlock took no lock while ConnectBlock ran under
 * activation_mutex_, producing a compare-then-write TOCTOU on block failure
 * flags. Adding the lock fixed it, but removing the lock again left every
 * suite green — the integration fixture could not supply the damaging payload,
 * so the lock was justified by reading the code and by nothing executable.
 *
 * With AssertHeld at the sites that require it, deleting the lock is caught by
 * EVERY test that exercises those paths. The invariant becomes enforced rather
 * than documented, and the same annotation tells a reviewer exactly what the
 * mutex covers.
 *
 * THROWS rather than assert(): assert() compiles away under NDEBUG, so a
 * release build — which is what CI and production run — would silently lose
 * the check.
 *
 * Satisfies Lockable, so existing std::lock_guard / std::unique_lock call
 * sites work unchanged.
 */

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace dinero {

class AnnotatedRecursiveMutex {
public:
    void lock() {
        mutex_.lock();
        owner_.store(std::this_thread::get_id(), std::memory_order_release);
        ++depth_;  // guarded by the mutex itself
    }

    bool try_lock() {
        if (!mutex_.try_lock()) return false;
        owner_.store(std::this_thread::get_id(), std::memory_order_release);
        ++depth_;
        return true;
    }

    void unlock() {
        // Clear the owner BEFORE releasing: another thread may acquire the
        // instant unlock() returns, and a stale owner id would make its
        // AssertHeld pass spuriously.
        if (--depth_ == 0) {
            owner_.store(std::thread::id{}, std::memory_order_release);
        }
        mutex_.unlock();
    }

    bool HeldByCurrentThread() const {
        return owner_.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    /// Throws if the calling thread does not hold this mutex. `where` names the
    /// call site so a failure identifies the offending path immediately.
    void AssertHeld(const char* where) const {
        if (!HeldByCurrentThread()) {
            throw std::logic_error(
                std::string("lock invariant violated: ") + where +
                " requires the activation lock, but the calling thread does not "
                "hold it. This path compares and then writes block failure "
                "flags; without serialization a stale result can re-assert "
                "flags a newer operator decision has cleared.");
        }
    }

private:
    std::recursive_mutex mutex_;
    std::atomic<std::thread::id> owner_{};
    int depth_ = 0;
};

}  // namespace dinero
