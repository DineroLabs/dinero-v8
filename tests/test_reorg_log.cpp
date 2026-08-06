// tests/test_reorg_log.cpp
// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "daemon/reorg_log.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using dinero::ReorgLog;

TEST(ReorgLogTest, starts_empty_with_a_boot_id) {
    ReorgLog log;
    EXPECT_EQ(log.Total(), 0u);
    EXPECT_TRUE(log.Events().empty());
    EXPECT_FALSE(log.BootId().empty());
}

TEST(ReorgLogTest, two_instances_have_different_boot_ids) {
    // The boot id is how a consumer knows seq and total have reset.
    ReorgLog a;
    ReorgLog b;
    EXPECT_NE(a.BootId(), b.BootId());
}

TEST(ReorgLogTest, records_depth_and_assigns_sequence_from_one) {
    ReorgLog log;
    log.Record(3, 4);
    const auto events = log.Events();
    ASSERT_EQ(events.size(), 1u);
    const auto& e = events.front();
    EXPECT_EQ(e.seq, 1u);
    EXPECT_EQ(e.disconnected, 3u);
    EXPECT_EQ(e.connected, 4u);
    EXPECT_FALSE(e.timestamp.empty());
}

TEST(ReorgLogTest, sequence_is_monotonic) {
    ReorgLog log;
    log.Record(1, 1);
    log.Record(1, 1);
    log.Record(1, 1);
    const auto events = log.Events();
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].seq, 1u);
    EXPECT_EQ(events[1].seq, 2u);
    EXPECT_EQ(events[2].seq, 3u);
}

TEST(ReorgLogTest, ring_keeps_the_newest_and_total_keeps_climbing) {
    // The whole point of exposing total separately: a consumer that sees total
    // outrun the events it can account for knows the ring overflowed, rather
    // than silently under-reporting.
    ReorgLog log;
    for (int i = 0; i < 100; ++i) log.Record(1, 1);
    const auto events = log.Events();
    EXPECT_EQ(events.size(), ReorgLog::kCapacity);
    EXPECT_EQ(log.Total(), 100u);
    EXPECT_EQ(events.front().seq, 100u - ReorgLog::kCapacity + 1u);
    EXPECT_EQ(events.back().seq, 100u);
}

TEST(ReorgLogTest, events_returns_a_snapshot_not_a_reference) {
    ReorgLog log;
    log.Record(1, 1);
    auto snapshot = log.Events();
    log.Record(2, 2);
    EXPECT_EQ(snapshot.size(), 1u) << "a caller's snapshot must not grow underneath it";
}

TEST(ReorgLogTest, concurrent_record_loses_nothing) {
    // Record() runs on the chain-activation path; Events() runs on an RPC
    // thread. Losing an event to a race would be a silent under-report.
    ReorgLog log;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&log] {
            for (int i = 0; i < 100; ++i) log.Record(1, 1);
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(log.Total(), 800u);
}
