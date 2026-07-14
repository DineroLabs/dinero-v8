#pragma once

#include <mutex>
#include <unordered_set>

#include "primitives/uint256.h"

namespace dinero {

// Thread-safe set of block hashes whose data exists in ChainDB (hasBlock OK)
// but cannot be parsed (getBlock returns a Serialization error). Prevents the
// header-branch import from re-setting BLOCK_HAVE_DATA in a loop.
//
// WHY THIS IS ITS OWN TYPE (2026-07-14): the raw set was accessed from two
// lock domains that never meet — erased from ProcessIncomingStoredBlock on
// the scheduler-drain/peer thread (scheduler lock) while inserted from
// ConnectTip and read from ActivateBestChain under activation_mutex_ on other
// threads. Concurrent insert/erase on a std::unordered_set writes through a
// freed/reallocated bucket array and free-lists hash nodes into other
// allocations — process-wide heap corruption. Encapsulating the set behind its
// own leaf mutex makes it impossible to touch it unlocked. The mutex is a LEAF
// lock: held only for the individual operation, never while acquiring another
// lock, so it cannot invert with activation_mutex_ or the scheduler lock.
class UnreadableBlockSet {
public:
    void mark(const uint256& hash) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_.insert(hash);
    }

    void clear(const uint256& hash) {
        std::lock_guard<std::mutex> lock(mutex_);
        set_.erase(hash);
    }

    bool contains(const uint256& hash) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_.count(hash) != 0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return set_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_set<uint256> set_;
};

}  // namespace dinero
