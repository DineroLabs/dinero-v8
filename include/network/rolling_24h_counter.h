// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "network/clock_source.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace dinero::network {

// 24-hour rolling event counter, hourly bucket granularity. Total24h()
// returns the sum across the trailing 24 hourly buckets. Buckets older
// than 24h are zeroed lazily on first touch from a new hour.
//
// Thread-safe: Add() is lock-free in the hot path (atomic increment on
// the current bucket); only the hour-boundary rotation acquires the
// mutex. Time source is injectable for test determinism.
class Rolling24hCounter {
public:
    explicit Rolling24hCounter(const ClockSource* clock)
        : clock_(clock), last_touched_hour_index_((assert(clock), SystemHourIndex())) {}

    void Add(uint64_t delta) {
        const uint64_t hour_index = SystemHourIndex();
        RotateIfHourCrossed(hour_index);
        const size_t bucket = hour_index % kBuckets;
        buckets_[bucket].fetch_add(delta, std::memory_order_relaxed);
    }

    uint64_t Total24h() const {
        const uint64_t hour_index = SystemHourIndex();
        RotateIfHourCrossed(hour_index);
        uint64_t total = 0;
        for (auto& b : buckets_) {
            total += b.load(std::memory_order_relaxed);
        }
        return total;
    }

private:
    static constexpr size_t kBuckets = 24;

    uint64_t SystemHourIndex() const {
        const auto now = clock_->SystemNow();
        const auto hrs = std::chrono::duration_cast<std::chrono::hours>(
            now.time_since_epoch()).count();
        return static_cast<uint64_t>(hrs);
    }

    void RotateIfHourCrossed(uint64_t hour_index) const {
        std::lock_guard<std::mutex> lock(rotation_mutex_);
        if (hour_index == last_touched_hour_index_) return;
        const uint64_t span = hour_index - last_touched_hour_index_;
        if (span >= kBuckets) {
            for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
        } else {
            for (uint64_t i = 1; i <= span; ++i) {
                const size_t idx = (last_touched_hour_index_ + i) % kBuckets;
                buckets_[idx].store(0, std::memory_order_relaxed);
            }
        }
        last_touched_hour_index_ = hour_index;
    }

    const ClockSource* clock_;
    mutable std::array<std::atomic<uint64_t>, kBuckets> buckets_{};
    mutable std::mutex rotation_mutex_;
    mutable uint64_t last_touched_hour_index_;
};

}  // namespace dinero::network
