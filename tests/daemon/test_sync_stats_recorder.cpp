// Forest checkpoint delta campaign — phase 0 instrumentation
// (docs/design/forest-checkpoint-deltas.md).
//
// SyncStatsRecorder accumulates per-block connect latency and forest
// checkpoint bytes, emits one aggregated summary line per 100 connected
// blocks, and exposes a snapshot for getsynchealth /
// getsnapshotbootstrapstatus. These are the baseline numbers the delta
// campaign's A/B benchmark is judged against, so the arithmetic here must
// be exact.

#include "daemon/sync_stats_recorder.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

using dinero::SyncStatsRecorder;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

namespace {

const steady_clock::time_point kStart{};  // epoch of steady_clock, t0

TEST(SyncStatsRecorder, AccumulatesPerBlockSamplesIntoSnapshot) {
    SyncStatsRecorder rec(kStart);

    rec.RecordBlockConnect(101, /*connect_ms=*/12, /*checkpoint_bytes=*/11000000,
                           kStart + seconds(1));
    rec.RecordBlockConnect(102, 8, 11000500, kStart + seconds(2));
    rec.RecordBlockConnect(103, 20, 10999500, kStart + seconds(3));

    const auto snap = rec.GetSnapshot();
    EXPECT_EQ(snap.blocks_connected, 3u);
    EXPECT_EQ(snap.total_connect_ms, 40u);
    EXPECT_EQ(snap.total_checkpoint_bytes, 33000000u);
    EXPECT_EQ(snap.last_height, 103u);
    EXPECT_EQ(snap.last_connect_ms, 20u);
    EXPECT_EQ(snap.last_checkpoint_bytes, 10999500u);
}

TEST(SyncStatsRecorder, EmitsSummaryExactlyEveryHundredBlocks) {
    SyncStatsRecorder rec(kStart);

    for (uint32_t i = 1; i <= 99; ++i) {
        const auto line =
            rec.RecordBlockConnect(i, 10, 1000, kStart + seconds(i));
        EXPECT_TRUE(line.empty()) << "unexpected summary at block " << i;
    }
    const auto line100 =
        rec.RecordBlockConnect(100, 10, 1000, kStart + seconds(100));
    EXPECT_FALSE(line100.empty());

    for (uint32_t i = 101; i <= 199; ++i) {
        const auto line =
            rec.RecordBlockConnect(i, 10, 1000, kStart + seconds(i));
        EXPECT_TRUE(line.empty()) << "unexpected summary at block " << i;
    }
    const auto line200 =
        rec.RecordBlockConnect(200, 10, 1000, kStart + seconds(200));
    EXPECT_FALSE(line200.empty());
}

TEST(SyncStatsRecorder, SummaryLineCarriesWindowAggregates) {
    SyncStatsRecorder rec(kStart);

    // 100 blocks, one every 2 seconds → window elapsed 200 s → 30.0 blk/min.
    // connect_ms = 10 each → avg 10.0; checkpoint bytes = 1000 each → avg 1000.
    std::string line;
    for (uint32_t i = 1; i <= 100; ++i) {
        line = rec.RecordBlockConnect(i, 10, 1000, kStart + seconds(2 * i));
    }

    EXPECT_NE(line.find("[SyncStats]"), std::string::npos) << line;
    EXPECT_NE(line.find("height=100"), std::string::npos) << line;
    EXPECT_NE(line.find("avg_connect_ms=10.0"), std::string::npos) << line;
    EXPECT_NE(line.find("avg_ckpt_bytes=1000"), std::string::npos) << line;
    EXPECT_NE(line.find("blk_min=30.0"), std::string::npos) << line;
}

TEST(SyncStatsRecorder, SecondWindowAggregatesAreWindowLocalNotCumulative) {
    SyncStatsRecorder rec(kStart);

    // First window: 10 ms / 1000 B blocks, 2 s apart.
    for (uint32_t i = 1; i <= 100; ++i) {
        rec.RecordBlockConnect(i, 10, 1000, kStart + seconds(2 * i));
    }
    // Second window: 30 ms / 3000 B blocks, 1 s apart → 60.0 blk/min.
    std::string line;
    for (uint32_t i = 101; i <= 200; ++i) {
        line = rec.RecordBlockConnect(i, 30, 3000,
                                      kStart + seconds(200) + seconds(i - 100));
    }

    EXPECT_NE(line.find("height=200"), std::string::npos) << line;
    EXPECT_NE(line.find("avg_connect_ms=30.0"), std::string::npos) << line;
    EXPECT_NE(line.find("avg_ckpt_bytes=3000"), std::string::npos) << line;
    EXPECT_NE(line.find("blk_min=60.0"), std::string::npos) << line;
}

TEST(SyncStatsRecorder, CsnCheckpointWritesAccumulateSeparately) {
    SyncStatsRecorder rec(kStart);

    rec.RecordCsnCheckpoint(5000);
    rec.RecordCsnCheckpoint(7000);

    const auto snap = rec.GetSnapshot();
    EXPECT_EQ(snap.csn_checkpoint_writes, 2u);
    EXPECT_EQ(snap.csn_checkpoint_bytes, 12000u);
    // CSN writes must not pollute the connect-path counters.
    EXPECT_EQ(snap.blocks_connected, 0u);
    EXPECT_EQ(snap.total_checkpoint_bytes, 0u);
}

TEST(SyncStatsRecorder, ZeroElapsedWindowDoesNotDivideByZero) {
    SyncStatsRecorder rec(kStart);

    std::string line;
    for (uint32_t i = 1; i <= 100; ++i) {
        line = rec.RecordBlockConnect(i, 10, 1000, kStart);  // no time passes
    }
    EXPECT_FALSE(line.empty());
    EXPECT_NE(line.find("blk_min=0.0"), std::string::npos) << line;
}

}  // namespace
