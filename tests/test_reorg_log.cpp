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
    const auto snapshot = log.Take();
    EXPECT_EQ(snapshot.total, 0u);
    EXPECT_TRUE(snapshot.events.empty());
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
    // Hoist the snapshot into a local. `Take()` returns BY VALUE, so
    // `log.Take().events.front()` would bind a reference into a temporary
    // that dies at the end of the full expression — lifetime extension does
    // not apply through a member call. Clang catches it with -Wdangling-gsl;
    // at runtime it reads garbage.
    const auto snapshot = log.Take();
    ASSERT_EQ(snapshot.events.size(), 1u);
    const auto& e = snapshot.events.front();
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
    const auto snapshot = log.Take();
    ASSERT_EQ(snapshot.events.size(), 3u);
    EXPECT_EQ(snapshot.events[0].seq, 1u);
    EXPECT_EQ(snapshot.events[1].seq, 2u);
    EXPECT_EQ(snapshot.events[2].seq, 3u);
}

TEST(ReorgLogTest, ring_keeps_the_newest_and_total_keeps_climbing) {
    // The whole point of exposing total separately: a consumer that sees total
    // outrun the events it can account for knows the ring overflowed, rather
    // than silently under-reporting.
    ReorgLog log;
    for (int i = 0; i < 100; ++i) log.Record(1, 1);
    const auto snapshot = log.Take();
    EXPECT_EQ(snapshot.events.size(), ReorgLog::kCapacity);
    EXPECT_EQ(snapshot.total, 100u);
    EXPECT_EQ(snapshot.events.front().seq, 100u - ReorgLog::kCapacity + 1u);
    EXPECT_EQ(snapshot.events.back().seq, 100u);
}

TEST(ReorgLogTest, take_returns_a_snapshot_not_a_reference) {
    ReorgLog log;
    log.Record(1, 1);
    auto snapshot = log.Take();
    log.Record(2, 2);
    EXPECT_EQ(snapshot.events.size(), 1u) << "a caller's snapshot must not grow underneath it";
    EXPECT_EQ(snapshot.total, 1u) << "a caller's snapshot must not change underneath it";
}

TEST(ReorgLogTest, take_reads_the_counter_and_ring_under_one_lock) {
    // Reading the counter and the ring separately would let a Record() land
    // between them, so the total would outrun the accountable events with no
    // overflow having occurred — a false positive on the design's only
    // overflow signal. Take() is the only accessor now; this is what makes
    // that impossible.
    ReorgLog log;
    for (int i = 0; i < 5; ++i) log.Record(1, 1);
    const auto snapshot = log.Take();
    EXPECT_EQ(snapshot.total, 5u);
    EXPECT_EQ(snapshot.events.size(), 5u);
    EXPECT_EQ(snapshot.boot_id, log.BootId());
    EXPECT_EQ(snapshot.events.back().seq, snapshot.total);
}

TEST(ReorgLogTest, take_stays_self_consistent_while_recording) {
    // The property that matters: within one snapshot, the newest seq never
    // exceeds the total. A torn read breaks exactly this.
    ReorgLog log;
    std::thread writer([&log] {
        for (int i = 0; i < 2000; ++i) log.Record(1, 1);
    });
    for (int i = 0; i < 2000; ++i) {
        const auto snapshot = log.Take();
        if (!snapshot.events.empty()) {
            ASSERT_LE(snapshot.events.back().seq, snapshot.total)
                << "torn read: a recorded event is newer than the total";
        }
    }
    writer.join();
}

TEST(ReorgLogTest, timestamp_is_rfc3339_utc) {
    // Nothing tested the format, so a regression would ship straight into the
    // JSON a consumer parses.
    ReorgLog log;
    log.Record(1, 1);
    const auto snapshot = log.Take();
    ASSERT_EQ(snapshot.events.size(), 1u);
    const std::string& ts = snapshot.events.front().timestamp;
    ASSERT_EQ(ts.size(), 20u) << "expected YYYY-MM-DDTHH:MM:SSZ, got: " << ts;
    EXPECT_EQ(ts[4], '-');
    EXPECT_EQ(ts[7], '-');
    EXPECT_EQ(ts[10], 'T');
    EXPECT_EQ(ts[13], ':');
    EXPECT_EQ(ts[16], ':');
    EXPECT_EQ(ts[19], 'Z');
    EXPECT_NE(ts.substr(0, 4), "1900") << "gmtime failed and left a zeroed tm";
}

TEST(ReorgLogTest, concurrent_record_loses_nothing) {
    // Record() runs on the chain-activation path; Take() runs on an RPC
    // thread. Losing an event to a race would be a silent under-report.
    ReorgLog log;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&log] {
            for (int i = 0; i < 100; ++i) log.Record(1, 1);
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(log.Take().total, 800u);
}
