#ifndef DINERO_CONSENSUS_GUARDED_FOREST_H
#define DINERO_CONSENSUS_GUARDED_FOREST_H

// GuardedForest<T> — a read/write lock around an owned value whose reads and
// replacements/mutations run on DIFFERENT threads.
//
// Motivation (audit HIGH finding): the Utreexo forest is replaced via
// move-assignment and mutated in place on the block-connect thread (under
// activation_mutex_), while RPC / mining / FFI threads read it (getRoots,
// getCommitment, getNumLeaves) with NO lock. A replacement frees the forest's
// heap buffers; a concurrent reader that has taken an interior pointer then
// dereferences freed memory — a use-after-free.
//
// Contract:
//   * Readers on any non-activation thread call WithShared(fn); fn receives a
//     const& and MUST return a COPY (never a reference/pointer that escapes the
//     lock). The shared lock is held for the whole of fn.
//   * A whole-value replacement uses Replace(next) (exclusive lock).
//   * An in-place structural mutation uses WithExclusive(fn) (exclusive lock).
//   * unguarded() is ONLY for code paths already serialized by an outer lock
//     (the activation thread holding activation_mutex_); it takes no lock.
//
// Multiple readers proceed concurrently; a replacement/mutation waits for all
// readers to finish and blocks new readers until it completes. The mutex is a
// LEAF lock: never acquire another lock while holding it, so it cannot invert
// with activation_mutex_ / g_block_index_mutex.

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace dinero {
namespace consensus {

template <typename T>
class GuardedForest {
public:
    GuardedForest() = default;
    explicit GuardedForest(T initial) : value_(std::move(initial)) {}

    GuardedForest(const GuardedForest&) = delete;
    GuardedForest& operator=(const GuardedForest&) = delete;

    // Writer: replace the whole value. Frees the previous T's buffers while
    // holding the exclusive lock, so no reader can be inside a read.
    void Replace(T next) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        value_ = std::move(next);
    }

    // Writer: run an in-place mutation (e.g. removeLastNLeaves) under the
    // exclusive lock.
    template <typename F>
    auto WithExclusive(F&& fn) -> decltype(fn(std::declval<T&>())) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        return fn(value_);
    }

    // Reader: run fn under a shared lock. fn must copy out whatever it needs.
    template <typename F>
    auto WithShared(F&& fn) const -> decltype(fn(std::declval<const T&>())) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return fn(value_);
    }

    // Escape hatch for the activation thread, which is already serialized by
    // activation_mutex_. Never call from another thread.
    T& unguarded() { return value_; }
    const T& unguarded() const { return value_; }

private:
    mutable std::shared_mutex mutex_;
    T value_;
};

}  // namespace consensus
}  // namespace dinero

#endif  // DINERO_CONSENSUS_GUARDED_FOREST_H
