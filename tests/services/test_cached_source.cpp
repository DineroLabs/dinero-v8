// CachedSource (include/p2p/cached_source.h): the Orchard history target as
// the download's worker and the wallet threads read it.
#include "p2p/cached_source.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using dinero::p2p::CachedSource;

namespace {
// Like the real target: several words that only make sense together.
struct Target {
    std::array<uint64_t, 16> words{};
    static Target Of(uint64_t n) { Target t; t.words.fill(n); return t; }
    bool Whole() const { for (auto w : words) if (w != words[0]) return false; return true; }
};
}  // namespace

TEST(CachedSource, ReadsTheSourceOncePerInterval) {
    int reads = 0;
    CachedSource<int> cached([&] { return std::optional<int>(++reads); }, std::chrono::hours(1));
    EXPECT_EQ(cached.Get(), 1);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(cached.Get(), 1);
    EXPECT_EQ(reads, 1);

    int fresh_reads = 0;
    CachedSource<int> uncached([&] { return std::optional<int>(++fresh_reads); }, std::chrono::seconds(0));
    EXPECT_EQ(uncached.Get(), 1);
    EXPECT_EQ(uncached.Get(), 2);  // the interval has passed: asked again
}

TEST(CachedSource, NoTargetIsAnAnswerToo) {
    int reads = 0;
    CachedSource<int> cached([&] { ++reads; return std::optional<int>(); }, std::chrono::hours(1));
    EXPECT_FALSE(cached.Get().has_value());
    EXPECT_FALSE(cached.Get().has_value());
    EXPECT_EQ(reads, 1);
}

// The fault this replaces: a caller handed half of one target and half of the next.
TEST(CachedSource, ManyThreadsAlwaysGetAWholeValue) {
    std::atomic<uint64_t> counter{0};
    CachedSource<Target> cached([&] { return std::optional<Target>(Target::Of(++counter)); }, std::chrono::seconds(0));
    std::atomic<bool> torn{false};
    std::atomic<uint64_t> gets{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 50'000; ++i) {
                const auto got = cached.Get();
                if (!got || !got->Whole()) torn = true;
                ++gets;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_FALSE(torn.load());
    EXPECT_EQ(gets.load(), 400'000u);
    EXPECT_GT(counter.load(), 1u);
}

// A source that takes a lock which a caller may hold must not deadlock:
// the source is called with this object's own lock released.
TEST(CachedSource, TheSourceIsCalledWithoutTheCacheLock) {
    CachedSource<int>* self = nullptr;
    int depth = 0;
    CachedSource<int> cached([&]() -> std::optional<int> {
        if (++depth == 1) return self->Get();  // re-entry from inside the source: would deadlock under the lock
        return 7;
    }, std::chrono::hours(1));
    self = &cached;
    EXPECT_NO_THROW(cached.Get());
}

TEST(CachedSource, AFailingSourceIsAskedAgainAfterTheInterval) {
    int reads = 0;
    CachedSource<int> cached([&]() -> std::optional<int> {
        if (++reads == 1) throw std::runtime_error("source unavailable");
        return 5;
    }, std::chrono::seconds(0));
    EXPECT_THROW(cached.Get(), std::runtime_error);
    EXPECT_EQ(cached.Get(), 5);
}
