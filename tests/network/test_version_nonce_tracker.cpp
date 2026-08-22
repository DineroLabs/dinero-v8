#include "network/version_nonce_tracker.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace dinero::network::test {

TEST(VersionNonceTracker, DetectsRememberedNonceAndRejectsZero) {
    VersionNonceTracker tracker;
    tracker.Remember(42);
    EXPECT_TRUE(tracker.Contains(42));
    EXPECT_FALSE(tracker.Contains(41));
    tracker.Remember(0);
    EXPECT_FALSE(tracker.Contains(0));
}

TEST(VersionNonceTracker, IsBoundedAndThreadSafe) {
    VersionNonceTracker tracker;
    std::vector<std::thread> writers;
    for (uint64_t shard = 0; shard < 8; ++shard) {
        writers.emplace_back([&tracker, shard] {
            for (uint64_t i = 1; i <= 256; ++i) {
                tracker.Remember(shard * 256 + i);
            }
        });
    }
    for (auto& writer : writers) writer.join();
    EXPECT_LE(tracker.Size(), VersionNonceTracker::kCapacity);
    tracker.Remember(999999);
    EXPECT_TRUE(tracker.Contains(999999));
}

} // namespace dinero::network::test
