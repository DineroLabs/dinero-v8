#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "daemon/services/assumeutxo_lifecycle.h"
#include "primitives/uint256.h"
#include "wallet/utxo_index.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace dinero {

class AssumeUtxoLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto unique_id = std::to_string(
            static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        temp_dir_ = fs::temp_directory_path() / ("dinero_assumeutxo_lc_" + unique_id);
        fs::create_directories(temp_dir_);
        utxo_index_ = std::make_unique<UTXOIndex>((temp_dir_ / "wallet.db").string());
        ASSERT_TRUE(utxo_index_->Initialize());
        t0_ = std::chrono::steady_clock::time_point{};  // deterministic epoch
        base_block_ = uint256::FromHexUnsafe(
            "00000015f97a45f358fee1562317c05590b042b190e288a60ad7218b7e4efffa");
    }

    void TearDown() override {
        utxo_index_.reset();
        std::error_code ec;
        fs::remove_all(temp_dir_, ec);
    }

    // Fresh lifecycle over the SAME persistence — simulates a daemon restart.
    std::unique_ptr<assumeutxo::AssumeUtxoLifecycle> MakeLifecycle(
            std::chrono::seconds stall_timeout = 1800s) {
        return std::make_unique<assumeutxo::AssumeUtxoLifecycle>(
            utxo_index_.get(), /*logger=*/nullptr, stall_timeout);
    }

    std::unique_ptr<UTXOIndex> utxo_index_;
    fs::path temp_dir_;
    std::chrono::steady_clock::time_point t0_;
    uint256 base_block_;
    static constexpr uint32_t kBaseHeight = 48;  // small synthetic-fixture height
};

using State = assumeutxo::AssumeUtxoLifecycle::State;

TEST_F(AssumeUtxoLifecycleTest, HappyPathRetiresTrustMarker) {
    auto lc = MakeLifecycle();
    EXPECT_EQ(lc->GetState(), State::Disabled);

    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    EXPECT_EQ(lc->GetState(), State::SnapshotLoaded);

    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);

    for (uint32_t h = 0; h <= kBaseHeight; ++h) {
        lc->OnBlockValidated(h, t0_ + std::chrono::seconds(h));
    }

    ASSERT_TRUE(lc->OnReplayComplete(/*replay_performed=*/true,
                                     /*commitment_match=*/true,
                                     "aa", "aa",
                                     /*missing_body_count=*/0,
                                     t0_ + 100s));
    EXPECT_EQ(lc->GetState(), State::FullyValidated);

    const auto st = lc->GetStatus(t0_ + 101s);
    EXPECT_TRUE(st.history_fully_validated);
    EXPECT_FALSE(st.assumeutxo_active);
    EXPECT_FALSE(st.fatal);
    EXPECT_EQ(st.snapshot_base_height, kBaseHeight);
}

// Forbidden: snapshot_loaded -> fully_validated without replay+comparison.
TEST_F(AssumeUtxoLifecycleTest, CannotCompleteFromSnapshotLoaded) {
    auto lc = MakeLifecycle();
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    EXPECT_FALSE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_));
    EXPECT_EQ(lc->GetState(), State::SnapshotLoaded);
}

// Restart mid-validation must still be able to re-arm the stall clock.
TEST_F(AssumeUtxoLifecycleTest, RestartMidValidationStillDetectsStall) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(5, t0_ + 5s);
    }
    {
        auto lc2 = MakeLifecycle(/*stall_timeout=*/1800s);
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        ASSERT_EQ(lc2->GetState(), State::ValidatingHistory);
        // Worker restart re-arms the clock...
        ASSERT_TRUE(lc2->OnValidationStarted(t0_ + 100s));
        // ...so zero further progress past the window is a LOUD stall.
        lc2->Tick(t0_ + 100s + 1801s);
        EXPECT_EQ(lc2->GetState(), State::ValidationStalled);
    }
}

// Leaving fully_validated via Disable must not strand a stale durable marker.
TEST_F(AssumeUtxoLifecycleTest, DisableClearsStaleTrustMarker) {
    auto lc = MakeLifecycle();
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
    ASSERT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 20s));
    ASSERT_TRUE(utxo_index_->GetMetadata(assumeutxo::kFullyValidatedKey).has_value());

    lc->Disable();
    EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFullyValidatedKey).has_value());
    EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kLifecycleStateKey).has_value());

    // A fresh snapshot load must not resurrect the old marker.
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFullyValidatedKey).has_value());
}

// Spec Required Test 1: load-time gates pass, genesis replay does not match.
TEST_F(AssumeUtxoLifecycleTest, PoisonedSnapshotIsFatal) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(kBaseHeight, t0_ + 10s);

        // Replay recomputed a different commitment than the snapshot committed.
        EXPECT_FALSE(lc->OnReplayComplete(/*replay_performed=*/true,
                                          /*commitment_match=*/false,
                                          "deadbeef", "cafebabe", 0, t0_ + 20s));
        EXPECT_EQ(lc->GetState(), State::FatalMismatch);

        const auto st = lc->GetStatus(t0_ + 21s);
        EXPECT_TRUE(st.fatal);
        EXPECT_FALSE(st.history_fully_validated);
        // Log/RPC must carry both commitments (spec: Fatal Mismatch item 5).
        EXPECT_NE(st.fatal_reason.find("deadbeef"), std::string::npos);
        EXPECT_NE(st.fatal_reason.find("cafebabe"), std::string::npos);
        EXPECT_NE(st.fatal_reason.find(base_block_.GetHex()), std::string::npos);
    }
    // Restart preserves fatal_mismatch (spec: Persistence).
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);
        EXPECT_TRUE(lc2->GetStatus(t0_).fatal);
        EXPECT_FALSE(lc2->GetStatus(t0_).fatal_reason.empty());
    }
}

// Spec Required Test 2: missing bodies are never success.
TEST_F(AssumeUtxoLifecycleTest, MissingBodiesCannotComplete) {
    auto lc = MakeLifecycle(/*stall_timeout=*/1800s);
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(10, t0_ + 10s);

    // Scan finished but 3 bodies were unavailable; commitment "matched" anyway.
    EXPECT_FALSE(lc->OnReplayComplete(/*replay_performed=*/true,
                                      /*commitment_match=*/true,
                                      "aa", "aa",
                                      /*missing_body_count=*/3, t0_ + 20s));
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);
    EXPECT_FALSE(lc->GetStatus(t0_ + 21s).history_fully_validated);
    EXPECT_EQ(lc->GetStatus(t0_ + 21s).missing_body_count, 3u);

    // After the stall window with no progress -> validation_stalled.
    lc->Tick(t0_ + 20s + 1801s);  // stall clock runs from last OnBlockValidated (t0_+10s); this is 1811s elapsed
    EXPECT_EQ(lc->GetState(), State::ValidationStalled);
    EXPECT_FALSE(lc->GetStatus(t0_ + 20s + 1802s).history_fully_validated);
}

// Spec Required Test 3: stall transition, metadata, recovery, completion.
TEST_F(AssumeUtxoLifecycleTest, StallIsLoudAndRecoverable) {
    auto lc = MakeLifecycle(/*stall_timeout=*/1800s);
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(5, t0_ + 5s);

    // Just under the window (clock runs from the last OnBlockValidated): still validating.
    lc->Tick(t0_ + 5s + 1799s);
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);

    // Past the window: stalled, with machine-readable stall metadata.
    lc->Tick(t0_ + 5s + 1800s);
    EXPECT_EQ(lc->GetState(), State::ValidationStalled);
    auto st = lc->GetStatus(t0_ + 5s + 1900s);
    EXPECT_GE(st.stall_seconds, 1800);
    EXPECT_FALSE(st.history_fully_validated);
    EXPECT_TRUE(st.assumeutxo_active);  // snapshot may stay foreground-usable

    // One real validated block recovers the stall.
    lc->OnBlockValidated(6, t0_ + 4000s);
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);

    // Reaching base with full replay + match completes.
    lc->OnBlockValidated(kBaseHeight, t0_ + 4100s);
    EXPECT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 4200s));
    EXPECT_EQ(lc->GetState(), State::FullyValidated);
}

// replay_performed=false (today's availability+count scan) can never retire trust.
TEST_F(AssumeUtxoLifecycleTest, AvailabilityScanAloneCannotComplete) {
    auto lc = MakeLifecycle();
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
    EXPECT_FALSE(lc->OnReplayComplete(/*replay_performed=*/false,
                                      /*commitment_match=*/true, "aa", "aa", 0, t0_ + 20s));
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);
}

// Spec Required Test 4 (persistence half): restart preserves fully_validated.
TEST_F(AssumeUtxoLifecycleTest, RetirementSurvivesRestart) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
        ASSERT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 20s));
        ASSERT_EQ(lc->GetState(), State::FullyValidated);
        ASSERT_EQ(utxo_index_->GetMetadata(
            assumeutxo::kFullyValidatedKey).value_or(""), "true");
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        EXPECT_EQ(lc2->GetState(), State::FullyValidated);
        EXPECT_TRUE(lc2->GetStatus(t0_).history_fully_validated);
        EXPECT_FALSE(lc2->GetStatus(t0_).assumeutxo_active);
    }
}

// Spec Persistence rule: marker present + chainstate mismatch -> FATAL, not trust.
TEST_F(AssumeUtxoLifecycleTest, RetirementMarkerWithMismatchedChainstateIsFatal) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
        ASSERT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 20s));
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/false);
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);
        EXPECT_TRUE(lc2->GetStatus(t0_).fatal);
        EXPECT_NE(lc2->GetStatus(t0_).fatal_reason.find("marker"), std::string::npos);
    }
}

// Spec Persistence: validation_stalled remains stalled across restart.
TEST_F(AssumeUtxoLifecycleTest, StalledStateSurvivesRestart) {
    {
        auto lc = MakeLifecycle(/*stall_timeout=*/1800s);
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(5, t0_ + 5s);
        lc->Tick(t0_ + 5s + 1800s);
        ASSERT_EQ(lc->GetState(), State::ValidationStalled);
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        EXPECT_EQ(lc2->GetState(), State::ValidationStalled);
        // Real progress still recovers after restart.
        lc2->OnBlockValidated(6, t0_ + 4000s);
        EXPECT_EQ(lc2->GetState(), State::ValidatingHistory);
    }
}

// Spec Required Test 5: fatal gates everything until explicit, token-confirmed reset.
TEST_F(AssumeUtxoLifecycleTest, FatalStateRequiresExplicitReset) {
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnReplayComplete(true, /*commitment_match=*/false, "aa", "bb", 0, t0_ + 10s);
        ASSERT_EQ(lc->GetState(), State::FatalMismatch);
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(true);
        ASSERT_EQ(lc2->GetState(), State::FatalMismatch);

        // New snapshot refused while fatal.
        EXPECT_FALSE(lc2->OnSnapshotLoaded(base_block_, kBaseHeight));
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);

        // Wrong/missing token refused.
        EXPECT_FALSE(lc2->OperatorReset(""));
        EXPECT_FALSE(lc2->OperatorReset("yes"));
        EXPECT_EQ(lc2->GetState(), State::FatalMismatch);

        // Correct token resets to Disabled and clears persisted fatal state.
        EXPECT_TRUE(lc2->OperatorReset(assumeutxo::kResetToken));
        EXPECT_EQ(lc2->GetState(), State::Disabled);
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFatalReasonKey).has_value());
        // Reset must NOT mark the prior snapshot valid (spec: Operator Reset).
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFullyValidatedKey).has_value());

        // A fresh attempt is now permitted.
        EXPECT_TRUE(lc2->OnSnapshotLoaded(base_block_, kBaseHeight));
    }
}

}  // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
