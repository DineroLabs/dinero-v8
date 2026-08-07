#include "nodecore/runtime_guards.h"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

void CallbackA(int32_t, const char*, void*) {}
void CallbackB(int32_t, const char*, void*) {}

struct FakeHash {
    std::string value;

    std::string GetHex() const { return value; }
};

struct DivergedAssumeUtxoSnapshot {
    bool has_active_tip = true;
    uint32_t active_tip_height = 83572;
    FakeHash active_tip_hash{"snapshot-base"};
    uint32_t storage_height = 0;
    std::string storage_hash = "genesis";
};

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

TEST(NodeCoreRuntimeGuards, ReportedTipUsesConsensusActiveViewNotStorageFrontier) {
    const DivergedAssumeUtxoSnapshot snapshot;
    const auto tip = dinero::nodecore::CaptureAuthoritativeTip(snapshot);

    EXPECT_EQ(tip.height, snapshot.active_tip_height);
    EXPECT_EQ(tip.hash, snapshot.active_tip_hash.value);
    EXPECT_NE(tip.height, snapshot.storage_height);
    EXPECT_NE(tip.hash, snapshot.storage_hash);
}

TEST(NodeCoreRuntimeGuards, ReportedTipIsEmptyUntilActiveTipIsPublished) {
    DivergedAssumeUtxoSnapshot snapshot;
    snapshot.has_active_tip = false;

    const auto tip = dinero::nodecore::CaptureAuthoritativeTip(snapshot);
    EXPECT_EQ(tip.height, 0U);
    EXPECT_TRUE(tip.hash.empty());
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
