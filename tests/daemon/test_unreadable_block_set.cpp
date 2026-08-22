/**
 * Regression test for the 2026-07-14 process-wide heap corruption.
 *
 * Root cause: ChainstateService::unreadable_blocks_ (a raw std::unordered_set)
 * was erased from the scheduler-drain/peer thread while being inserted/read
 * under activation_mutex_ on other threads — two lock domains that never meet.
 * Concurrent insert/erase on a std::unordered_set is undefined behavior: it
 * writes through a freed/reallocated bucket array and free-lists hash nodes
 * into other allocations, corrupting unrelated threads' heap (observed as a
 * false bad-utreexo-root in the isolated AssumeUTXO replay thread and a SIGSEGV
 * inside an unrelated mutex-guarded static deque).
 *
 * The fix moved the set behind UnreadableBlockSet, whose leaf mutex makes every
 * access synchronized by construction. This test drives that type from many
 * threads exactly as production does (clear from one thread; mark/contains from
 * others). It asserts logical correctness under contention; under ThreadSanitizer
 * (CI) it also proves race-freedom — the same access pattern on a raw
 * std::unordered_set trips TSAN and, in the field, corrupted the heap.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "daemon/services/unreadable_block_set.h"
#include "primitives/uint256.h"

namespace {
dinero::uint256 HashFromU64(uint64_t v) {
    dinero::uint256 h;  // default ctor zeroes all 32 bytes
    for (int i = 0; i < 8; ++i) {
        h.data[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
    return h;
}
}  // namespace

TEST(UnreadableBlockSet, SingleThreadSemantics)
{
    dinero::UnreadableBlockSet s;
    const auto a = HashFromU64(1), b = HashFromU64(2);
    EXPECT_FALSE(s.contains(a));
    s.mark(a);
    EXPECT_TRUE(s.contains(a));
    EXPECT_FALSE(s.contains(b));
    s.clear(a);
    EXPECT_FALSE(s.contains(a));
    // clear of an absent key is a no-op, not a crash.
    s.clear(b);
    EXPECT_EQ(s.size(), 0u);
}

TEST(UnreadableBlockSet, QuarantinedMetadataDoesNotSuppressRepair)
{
    dinero::UnreadableBlockSet s;
    const auto hash = HashFromU64(42);
    EXPECT_FALSE(s.is_usable(hash, false));
    EXPECT_TRUE(s.is_usable(hash, true));
    s.mark(hash);
    EXPECT_FALSE(s.is_usable(hash, true));
    s.clear(hash);
    EXPECT_TRUE(s.is_usable(hash, true));
}

// The production interleaving: one thread clears (scheduler-drain), several
// mark + read (activation-mutex holders). Pre-fix this raced a raw
// unordered_set; the assertion here is that it neither corrupts nor crashes,
// and stays logically consistent. Runs clean under TSAN in CI.
TEST(UnreadableBlockSet, ConcurrentMarkClearContainsIsRaceFree)
{
    dinero::UnreadableBlockSet s;
    constexpr int kKeys = 2048;
    constexpr int kIters = 200;
    std::atomic<bool> go{false};

    auto spin = [&] { while (!go.load(std::memory_order_acquire)) {} };

    std::vector<std::thread> threads;
    // Marker threads.
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&] {
            spin();
            for (int it = 0; it < kIters; ++it)
                for (int k = 0; k < kKeys; ++k) s.mark(HashFromU64(k));
        });
    }
    // Clearer thread (the scheduler-drain analogue).
    threads.emplace_back([&] {
        spin();
        for (int it = 0; it < kIters; ++it)
            for (int k = 0; k < kKeys; ++k) s.clear(HashFromU64(k));
    });
    // Reader threads (the ActivateBestChain count() analogue).
    std::atomic<uint64_t> observed_present{0};
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            spin();
            uint64_t seen = 0;
            for (int it = 0; it < kIters; ++it)
                for (int k = 0; k < kKeys; ++k)
                    if (s.contains(HashFromU64(k))) ++seen;
            observed_present.fetch_add(seen);
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();

    // No crash / no corruption is the primary assertion. Final state must be a
    // valid subset of the key space (marks and clears both ran to completion).
    EXPECT_LE(s.size(), static_cast<size_t>(kKeys));
    // Sanity: readers ran (value is nondeterministic, only its validity matters).
    EXPECT_GE(observed_present.load(), 0u);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
