// Copyright (c) 2026 Dinero Labs.
//
// Phase 0 wave 1 — sqlite-backed nullifier set tests. Verifies the
// double-spend prevention contract: insert, contains, duplicate
// rejection, and reorg-driven RollbackAbove(height). Persistence
// is checked by close-and-reopen.

#include <gtest/gtest.h>

#include "consensus/shielded/nullifier_set.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define DINERO_GETPID _getpid
#else
#include <unistd.h>
#define DINERO_GETPID getpid
#endif

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::Hash;
using shielded::NullifierSet;

// Portable replacement for the original POSIX mkstemps()-based path. The
// previous implementation hard-coded "/tmp" and called mkstemps + close —
// neither available on MSVC. Same uniqueness guarantees via pid +
// nanosecond timestamp + atomic counter, and we let sqlite create the
// file (the test only needs a valid unique path).
std::string TempDbPath() {
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<unsigned long long>(DINERO_GETPID());
    const auto ts = static_cast<long long>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    char name[96];
    std::snprintf(name, sizeof(name),
                  "dinero_nullifier_test_%llu_%lld_%llu.db",
                  pid, ts, static_cast<unsigned long long>(seq));
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

class NullifierSetFixture : public ::testing::Test {
protected:
    std::string path = TempDbPath();
    NullifierSet set;

    void SetUp() override {
        ASSERT_EQ(set.Open(path), NullifierSet::OpenResult::Ok);
    }

    void TearDown() override {
        set.Close();
        std::filesystem::remove(path);
    }

    static Hash N(uint8_t seed) {
        Hash h{};
        h[0] = seed;
        h[31] = 0xAB;  // distinct tail to avoid silent zero-collisions.
        return h;
    }
};

TEST_F(NullifierSetFixture, EmptySetContainsNothing) {
    EXPECT_FALSE(set.Contains(N(0x01)));
    EXPECT_EQ(set.Size(), 0u);
}

// Contains() is the double-spend gate: both consensus callers reject a spend
// when it returns true. So when the set cannot answer, the only safe reply is
// "present" — refuse the spend. Returning false on an unusable database means
// "not spent, go ahead", i.e. silently admitting a double-spend.
//
// Only SQLITE_DONE — the database positively answering "no such row" — may
// produce false, which is what EmptySetContainsNothing above pins.
TEST_F(NullifierSetFixture, ContainsFailsClosedOnUnusableDatabase) {
    ASSERT_TRUE(set.Insert(N(0x30), 300));
    ASSERT_TRUE(set.Contains(N(0x30)));
    ASSERT_FALSE(set.Contains(N(0x31)));

    set.Close();  // db_ == nullptr: every query now unanswerable

    EXPECT_TRUE(set.Contains(N(0x31)))
        << "a nullifier the set cannot vouch for must read as PRESENT so the "
           "spend is rejected; returning false admits a double-spend";
    EXPECT_TRUE(set.Contains(N(0x30)));
}

TEST_F(NullifierSetFixture, InsertAndContains) {
    EXPECT_TRUE(set.Insert(N(0x05), 100));
    EXPECT_TRUE(set.Contains(N(0x05)));
    EXPECT_FALSE(set.Contains(N(0x06)));
    EXPECT_EQ(set.Size(), 1u);
}

TEST_F(NullifierSetFixture, DuplicateInsertReturnsFalse) {
    ASSERT_TRUE(set.Insert(N(0x10), 200));
    EXPECT_FALSE(set.Insert(N(0x10), 201));
    EXPECT_EQ(set.Size(), 1u);
}

TEST_F(NullifierSetFixture, RollbackAboveRemovesLaterEntries) {
    ASSERT_TRUE(set.Insert(N(0x11), 100));
    ASSERT_TRUE(set.Insert(N(0x12), 200));
    ASSERT_TRUE(set.Insert(N(0x13), 300));
    EXPECT_EQ(set.Size(), 3u);

    set.RollbackAbove(150);
    EXPECT_TRUE(set.Contains(N(0x11)));
    EXPECT_FALSE(set.Contains(N(0x12)));
    EXPECT_FALSE(set.Contains(N(0x13)));
    EXPECT_EQ(set.Size(), 1u);
}

TEST_F(NullifierSetFixture, RollbackAboveTipIsNoOp) {
    ASSERT_TRUE(set.Insert(N(0x21), 100));
    set.RollbackAbove(100);  // strictly greater than 100 — keeps height-100 entry.
    EXPECT_TRUE(set.Contains(N(0x21)));
    EXPECT_EQ(set.Size(), 1u);
}

TEST_F(NullifierSetFixture, RollbackEmptySetIsSafe) {
    set.RollbackAbove(0);
    EXPECT_EQ(set.Size(), 0u);
}

TEST(NullifierSetPersistenceTest, ContainsSurvivesReopen) {
    const std::string path = TempDbPath();
    {
        NullifierSet a;
        ASSERT_EQ(a.Open(path), NullifierSet::OpenResult::Ok);
        Hash h{};
        h[0] = 0x99;
        h[31] = 0xCD;
        ASSERT_TRUE(a.Insert(h, 500));
        a.Close();
    }
    {
        NullifierSet b;
        ASSERT_EQ(b.Open(path), NullifierSet::OpenResult::Ok);
        Hash h{};
        h[0] = 0x99;
        h[31] = 0xCD;
        EXPECT_TRUE(b.Contains(h));
        EXPECT_EQ(b.Size(), 1u);
        b.Close();
    }
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
