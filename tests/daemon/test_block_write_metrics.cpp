// Copyright (c) 2026 Dinero Labs.
//
// The block-write rate limiter and its counters. These shipped with no test at
// all: ShouldEmitRateLimited had two production callers and zero test callers,
// so every property below was asserted only by reading.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "daemon/block_write_metrics.h"

using dinero::daemon::ShouldEmitRateLimited;
using dinero::daemon::ShouldEmitRateLimitedAt;

TEST(BlockWriteMetrics, FirstCallEmitsThenSuppressesWithinTheInterval) {
    std::atomic<uint64_t> state{0};
    EXPECT_TRUE(ShouldEmitRateLimited(state, 60)) << "the first call must emit";
    EXPECT_FALSE(ShouldEmitRateLimited(state, 60)) << "the second must not";
    EXPECT_FALSE(ShouldEmitRateLimited(state, 60));
}

// THE case, and the reason the stored value is now_s + 1 rather than now_s.
//
// steady_clock's epoch is unspecified and is boot-relative on Linux, so
// now_s == 0 during the first second of uptime -- which a daemon auto-started
// at boot hits. If the raw timestamp were stored, that 0 would read back as
// the "never emitted" sentinel and the limiter would emit on EVERY call for
// that second. The very thing it exists to prevent: this limiter was added
// because one line per delivery produced 9.3 MB/min.
//
// A state of 0 is indistinguishable from "never emitted" by construction, so
// the property is asserted the way it actually matters: after any successful
// emit the stored state must be non-zero, whatever the clock reads.
TEST(BlockWriteMetrics, SurvivesTheFirstSecondOfUptime) {
    using dinero::daemon::ShouldEmitRateLimitedAt;
    // now_s == 0 is the first second of uptime on Linux, where steady_clock's
    // epoch is boot. Reachable only through the seam: any machine running this
    // suite is long past it, so driving the real clock would assert nothing.
    std::atomic<uint64_t> state{0};
    EXPECT_TRUE(ShouldEmitRateLimitedAt(state, 60, /*now_s=*/0))
        << "the first call must emit";
    EXPECT_NE(state.load(), 0u)
        << "storing a RAW 0 here reads back as the 'never emitted' sentinel";
    EXPECT_FALSE(ShouldEmitRateLimitedAt(state, 60, /*now_s=*/0))
        << "the limiter must still suppress during the boot second -- storing "
           "the raw timestamp made every call in that second emit, which is "
           "precisely the log flood it exists to prevent";
    EXPECT_FALSE(ShouldEmitRateLimitedAt(state, 60, /*now_s=*/30));
    EXPECT_TRUE(ShouldEmitRateLimitedAt(state, 60, /*now_s=*/61))
        << "and must emit again once the interval has genuinely elapsed";
}

TEST(BlockWriteMetrics, AZeroIntervalAlwaysEmits) {
    std::atomic<uint64_t> state{0};
    EXPECT_TRUE(ShouldEmitRateLimited(state, 0));
    EXPECT_TRUE(ShouldEmitRateLimited(state, 0))
        << "with no interval there is nothing to suppress";
}

// Independent counters must not share a slot: two rate-limited sites with one
// shared state would suppress each other.
TEST(BlockWriteMetrics, SeparateStatesAreIndependent) {
    std::atomic<uint64_t> a{0}, b{0};
    EXPECT_TRUE(ShouldEmitRateLimited(a, 60));
    EXPECT_TRUE(ShouldEmitRateLimited(b, 60))
        << "a second site's first line must not be suppressed by the first's";
}

// Only ONE thread may win a slot; the rest are suppressed. Otherwise the
// concurrent case reintroduces exactly the log flood being limited.
TEST(BlockWriteMetrics, OnlyOneConcurrentCallerWinsTheSlot) {
    std::atomic<uint64_t> state{0};
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            if (ShouldEmitRateLimited(state, 60)) ++winners;
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(winners.load(), 1) << "exactly one caller may emit per interval";
}
