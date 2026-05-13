#include "nodecore/runtime_guards.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

void CallbackA(int32_t, const char*, void*) {}
void CallbackB(int32_t, const char*, void*) {}

TEST(NodeCoreRuntimeGuards, QueryGateRequiresLiveAppAndNoShutdown) {
    dinero::nodecore::RuntimeQueryState state;

    EXPECT_FALSE(dinero::nodecore::IsQueryable(state));

    state.running = true;
    EXPECT_FALSE(dinero::nodecore::IsQueryable(state));

    state.has_app = true;
    EXPECT_TRUE(dinero::nodecore::IsQueryable(state));

    state.shutdown_requested = true;
    EXPECT_FALSE(dinero::nodecore::IsQueryable(state));
}

TEST(NodeCoreRuntimeGuards, EventCallbackSlotSnapshotsCoherentPairsUnderContention) {
    dinero::nodecore::EventCallbackSlot slot;
    int token_a = 1;
    int token_b = 2;

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> bad_snapshot{false};

    std::thread writer([&] {
        while (!start.load()) {
        }
        for (int i = 0; i < 20000; ++i) {
            slot.Set(&CallbackA, &token_a);
            slot.Set(&CallbackB, &token_b);
        }
        stop.store(true);
    });

    std::thread reader([&] {
        start.store(true);
        while (!stop.load()) {
            const auto snapshot = slot.Snapshot();
            if (!snapshot.callback) {
                continue;
            }

            const bool valid_pair =
                (snapshot.callback == &CallbackA && snapshot.user_data == &token_a) ||
                (snapshot.callback == &CallbackB && snapshot.user_data == &token_b);
            if (!valid_pair) {
                bad_snapshot.store(true);
                break;
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_FALSE(bad_snapshot.load());
    const auto final_snapshot = slot.Snapshot();
    EXPECT_EQ(final_snapshot.callback, &CallbackB);
    EXPECT_EQ(final_snapshot.user_data, &token_b);
}

TEST(NodeCoreRuntimeGuards, WatchedScriptSnapshotIsStableAcrossMutation) {
    dinero::nodecore::WatchedScriptRegistry registry;
    registry.Add("aa");
    registry.Add("bb");

    const auto snapshot = registry.Snapshot();

    registry.Remove("aa");
    registry.Add("cc");

    EXPECT_EQ(snapshot, (std::unordered_set<std::string>{"aa", "bb"}));
    EXPECT_FALSE(registry.Contains("aa"));
    EXPECT_TRUE(registry.Contains("bb"));
    EXPECT_TRUE(registry.Contains("cc"));
}

}  // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
