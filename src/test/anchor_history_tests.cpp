// Copyright (c) 2026 Dinero Labs.
//
// Phase 2 wave 1 — anchor depth window unit tests. Pins:
//   - RecordRoot appends, evicts oldest beyond kDepth
//   - Same-height re-record overwrites (idempotent re-validation)
//   - Contains is a linear membership check
//   - RollbackAbove drops every entry above the threshold (reorg)
//
// These are pure-function tests; the integration with
// ValidateShieldedBundle's anchor check lives in the validation
// suite (HistoricalAnchorAccepted).

#include <gtest/gtest.h>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#include <process.h>
#define DINERO_GETPID _getpid
#else
#include <unistd.h>
#define DINERO_GETPID getpid
#endif
#include <fstream>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::AnchorHistory;
using shielded::Hash;

Hash R(uint8_t seed) {
    Hash h{};
    h[0]  = seed;
    h[31] = 0x42;
    return h;
}

TEST(AnchorHistoryTest, EmptyContainsNothing) {
    AnchorHistory h;
    EXPECT_EQ(h.Size(), 0u);
    EXPECT_FALSE(h.Contains(R(0x01)));
}

TEST(AnchorHistoryTest, RecordAndContains) {
    AnchorHistory h;
    h.RecordRoot(100, R(0x10));
    h.RecordRoot(101, R(0x11));
    EXPECT_EQ(h.Size(), 2u);
    EXPECT_TRUE(h.Contains(R(0x10)));
    EXPECT_TRUE(h.Contains(R(0x11)));
    EXPECT_FALSE(h.Contains(R(0x99)));
}

TEST(AnchorHistoryTest, EvictsBeyondDepth) {
    AnchorHistory h;
    // Push kDepth + 5 distinct entries; oldest 5 should evict.
    for (uint32_t i = 0; i < AnchorHistory::kDepth + 5; ++i) {
        h.RecordRoot(i, R(static_cast<uint8_t>(i & 0xFF)));
    }
    EXPECT_EQ(h.Size(), AnchorHistory::kDepth);
    // The very first 5 roots should have been evicted.
    EXPECT_FALSE(h.Contains(R(0)));
    EXPECT_FALSE(h.Contains(R(4)));
    // The most recent one is still in.
    EXPECT_TRUE(h.Contains(R(static_cast<uint8_t>(
        (AnchorHistory::kDepth + 4) & 0xFF))));
}

TEST(AnchorHistoryTest, SameHeightReRecordOverwrites) {
    AnchorHistory h;
    h.RecordRoot(50, R(0x55));
    h.RecordRoot(50, R(0x66));  // re-validation pass at same height
    EXPECT_EQ(h.Size(), 1u);
    EXPECT_FALSE(h.Contains(R(0x55)));
    EXPECT_TRUE(h.Contains(R(0x66)));
}

TEST(AnchorHistoryTest, RollbackAboveDropsLater) {
    AnchorHistory h;
    h.RecordRoot(10, R(0xA0));
    h.RecordRoot(20, R(0xA1));
    h.RecordRoot(30, R(0xA2));
    h.RecordRoot(40, R(0xA3));

    h.RollbackAbove(20);  // strict greater-than 20
    EXPECT_EQ(h.Size(), 2u);
    EXPECT_TRUE(h.Contains(R(0xA0)));
    EXPECT_TRUE(h.Contains(R(0xA1)));
    EXPECT_FALSE(h.Contains(R(0xA2)));
    EXPECT_FALSE(h.Contains(R(0xA3)));
}

TEST(AnchorHistoryTest, RollbackAboveTipIsNoOp) {
    AnchorHistory h;
    h.RecordRoot(10, R(0xB0));
    h.RollbackAbove(10);  // tip itself stays
    EXPECT_EQ(h.Size(), 1u);
    EXPECT_TRUE(h.Contains(R(0xB0)));
}

TEST(AnchorHistoryTest, RollbackEmptyIsSafe) {
    AnchorHistory h;
    h.RollbackAbove(0);
    EXPECT_EQ(h.Size(), 0u);
}

TEST(AnchorHistoryTest, ClearResets) {
    AnchorHistory h;
    h.RecordRoot(7, R(0x07));
    h.RecordRoot(8, R(0x08));
    h.Clear();
    EXPECT_EQ(h.Size(), 0u);
    EXPECT_FALSE(h.Contains(R(0x07)));
}

// ── Persistence (Phase 3 wave 2) ────────────────────────────────────

std::string TempPath() {
    // Portable replacement for POSIX mkstemp + close. Same uniqueness
    // (pid + ns-timestamp + atomic counter); returns a path the caller
    // will create.
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<unsigned long long>(DINERO_GETPID());
    const auto ts = static_cast<long long>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    char name[96];
    std::snprintf(name, sizeof(name),
                  "dinero_anchor_history_%llu_%lld_%llu",
                  pid, ts, static_cast<unsigned long long>(seq));
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

TEST(AnchorHistoryPersistenceTest, SaveLoadRoundTrip) {
    const std::string path = TempPath();
    AnchorHistory original;
    original.RecordRoot(10, R(0xA0));
    original.RecordRoot(20, R(0xA1));
    original.RecordRoot(30, R(0xA2));

    ASSERT_EQ(original.Save(path), AnchorHistory::IoResult::Ok);

    AnchorHistory restored;
    ASSERT_EQ(restored.Load(path), AnchorHistory::IoResult::Ok);
    EXPECT_EQ(restored.Size(), 3u);
    EXPECT_TRUE(restored.Contains(R(0xA0)));
    EXPECT_TRUE(restored.Contains(R(0xA1)));
    EXPECT_TRUE(restored.Contains(R(0xA2)));

    std::filesystem::remove(path);
}

TEST(AnchorHistoryPersistenceTest, EmptySaveLoadIsClean) {
    const std::string path = TempPath();
    AnchorHistory empty;
    ASSERT_EQ(empty.Save(path), AnchorHistory::IoResult::Ok);

    AnchorHistory loaded;
    loaded.RecordRoot(99, R(0xEE));  // pre-existing state
    ASSERT_EQ(loaded.Load(path), AnchorHistory::IoResult::Ok);
    EXPECT_EQ(loaded.Size(), 0u);
    EXPECT_FALSE(loaded.Contains(R(0xEE)));

    std::filesystem::remove(path);
}

TEST(AnchorHistoryPersistenceTest, LoadMissingFileIsIoError) {
    AnchorHistory h;
    EXPECT_EQ(h.Load("/tmp/dinero_anchor_definitely_does_not_exist_xyz"),
              AnchorHistory::IoResult::IoError);
}

TEST(AnchorHistoryPersistenceTest, LoadCorruptMagicRejected) {
    const std::string path = TempPath();
    {
        std::ofstream out(path, std::ios::binary);
        // wrong magic
        out << "BADMG";
    }
    AnchorHistory h;
    EXPECT_EQ(h.Load(path), AnchorHistory::IoResult::FormatError);
    std::filesystem::remove(path);
}

TEST(AnchorHistoryPersistenceTest, LoadTruncatedRejected) {
    const std::string path = TempPath();
    AnchorHistory original;
    original.RecordRoot(7, R(0x77));
    ASSERT_EQ(original.Save(path), AnchorHistory::IoResult::Ok);

    // Chop the file in half — should be truncated.
    auto sz = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, sz / 2);

    AnchorHistory h;
    EXPECT_NE(h.Load(path), AnchorHistory::IoResult::Ok);
    std::filesystem::remove(path);
}

TEST(AnchorHistoryPersistenceTest, SaveIsAtomicOnSuccess) {
    const std::string path = TempPath();
    AnchorHistory h;
    h.RecordRoot(1, R(0x01));
    ASSERT_EQ(h.Save(path), AnchorHistory::IoResult::Ok);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
