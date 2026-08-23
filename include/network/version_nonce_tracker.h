#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace dinero::network {

// Bitcoin-style self-connection detector. Every nonce placed in one of our
// version messages is remembered briefly. Receiving any remembered nonce from
// a peer proves the remote socket is another end of this process's own dial.
class VersionNonceTracker {
public:
    static constexpr size_t kCapacity = 1024;

    void Remember(uint64_t nonce) {
        if (nonce == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!nonces_.insert(nonce).second) return;
        order_.push_back(nonce);
        while (order_.size() > kCapacity) {
            nonces_.erase(order_.front());
            order_.pop_front();
        }
    }

    bool Contains(uint64_t nonce) const {
        if (nonce == 0) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return nonces_.count(nonce) != 0;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return nonces_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<uint64_t> order_;
    std::unordered_set<uint64_t> nonces_;
};

} // namespace dinero::network
