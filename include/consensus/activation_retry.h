#pragma once

#include "primitives/uint256.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace dinero::consensus {

/**
 * Retry bookkeeping for operational best-chain activation failures.
 *
 * A temporary storage/resource failure must not make a higher-work candidate
 * disappear.  This tracker leaves candidacy untouched and only applies a
 * bounded exponential cooldown so repeated activation ticks do not spin at
 * full CPU while the underlying condition recovers.
 */
class ActivationRetryTracker {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit ActivationRetryTracker(std::chrono::milliseconds base_delay =
                                        std::chrono::milliseconds(250),
                                    std::chrono::milliseconds max_delay =
                                        std::chrono::seconds(30))
        : base_delay_(base_delay), max_delay_(max_delay) {}

    std::chrono::milliseconds RecordFailure(const uint256& hash, TimePoint now) {
        Entry& entry = entries_[hash];
        entry.failures = std::min<uint32_t>(entry.failures + 1, 31);
        const uint32_t shift = std::min<uint32_t>(entry.failures - 1, 16);
        const auto multiplier = uint64_t{1} << shift;
        const auto delay = std::min(
            max_delay_,
            std::chrono::milliseconds(base_delay_.count() * multiplier));
        entry.retry_at = now + delay;
        return delay;
    }

    bool IsReady(const uint256& hash, TimePoint now) const {
        const auto it = entries_.find(hash);
        return it == entries_.end() || now >= it->second.retry_at;
    }

    void Clear(const uint256& hash) { entries_.erase(hash); }

    uint32_t FailureCount(const uint256& hash) const {
        const auto it = entries_.find(hash);
        return it == entries_.end() ? 0 : it->second.failures;
    }

private:
    struct Entry {
        uint32_t failures{0};
        TimePoint retry_at{};
    };

    std::chrono::milliseconds base_delay_;
    std::chrono::milliseconds max_delay_;
    std::unordered_map<uint256, Entry> entries_;
};

}  // namespace dinero::consensus
