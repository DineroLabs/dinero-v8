// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/rolling_24h_counter.h"
#include "network/clock_source.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

using dinero::network::FakeClockSource;
using dinero::network::Rolling24hCounter;

TEST(Rolling24hCounter, EmptyReturnsZero) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    EXPECT_EQ(c.Total24h(), 0u);
}

TEST(Rolling24hCounter, SingleHourAccumulates) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    c.Add(5);
    c.Add(7);
    EXPECT_EQ(c.Total24h(), 12u);
}

TEST(Rolling24hCounter, OldHourBucketsRotateOut) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    c.Add(10);  // hour 0
    clk.AdvanceSystem(std::chrono::hours(23));
    c.Add(5);   // hour 23
    EXPECT_EQ(c.Total24h(), 15u);
    clk.AdvanceSystem(std::chrono::hours(2));  // hour 25 — clears hour 0 bucket
    c.Add(1);
    EXPECT_EQ(c.Total24h(), 6u);  // 5 (hour 23) + 1 (hour 25); hour 0 (10) rotated out
}

TEST(Rolling24hCounter, FullWraparoundClearsAllBuckets) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    c.Add(100);
    clk.AdvanceSystem(std::chrono::hours(24));
    c.Add(1);
    EXPECT_EQ(c.Total24h(), 1u);
}

TEST(Rolling24hCounter, ThreadSafeUnderConcurrentIncrement) {
    FakeClockSource clk;
    Rolling24hCounter c(&clk);
    constexpr int kThreads = 8;
    constexpr int kIncrementsPerThread = 10000;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            for (int j = 0; j < kIncrementsPerThread; ++j) c.Add(1);
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_EQ(c.Total24h(),
              static_cast<uint64_t>(kThreads) * kIncrementsPerThread);
}
