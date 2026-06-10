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

// Spec Stall Semantics: worker restart alone must not exit a stall — only real progress.
TEST_F(AssumeUtxoLifecycleTest, WorkerRestartDoesNotExitStall) {
    auto lc = MakeLifecycle(/*stall_timeout=*/1800s);
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(5, t0_ + 5s);
    lc->Tick(t0_ + 5s + 1800s);
    ASSERT_EQ(lc->GetState(), State::ValidationStalled);

    // Worker restarts: clock re-arms, but the stall remains visible.
    ASSERT_TRUE(lc->OnValidationStarted(t0_ + 4000s));
    EXPECT_EQ(lc->GetState(), State::ValidationStalled);

    // Only a genuinely validated block recovers.
    lc->OnBlockValidated(6, t0_ + 4100s);
    EXPECT_EQ(lc->GetState(), State::ValidatingHistory);
}

// Spec Persistence: validating_history resumes from the last durable progress
// marker. Markers persist on a throttle (height % 100 == 0, or reaching base),
// so a crash loses at most the last <100 blocks of progress, never the run.
TEST_F(AssumeUtxoLifecycleTest, ProgressMarkerSurvivesRestart) {
    constexpr uint32_t kTallBase = 250;  // > one throttle period
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kTallBase));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        for (uint32_t h = 0; h <= 230; ++h) {
            lc->OnBlockValidated(h, t0_ + std::chrono::seconds(h));
        }
    }
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        EXPECT_EQ(lc2->GetState(), State::ValidatingHistory);
        // Throttle: persisted at h % 100 == 0 → last durable marker is 200, not 230.
        EXPECT_EQ(lc2->GetStatus(t0_).current_validation_height, 200u);

        // Resume and reach base: base height persists even off the %100 grid.
        ASSERT_TRUE(lc2->OnValidationStarted(t0_ + 300s));
        for (uint32_t h = 201; h <= kTallBase; ++h) {
            lc2->OnBlockValidated(h, t0_ + 300s + std::chrono::seconds(h));
        }
    }
    {
        auto lc3 = MakeLifecycle();
        lc3->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        EXPECT_EQ(lc3->GetStatus(t0_).current_validation_height, kTallBase);
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
        // Spec Test 5: RPC still reports fatal=true after the refused load.
        EXPECT_TRUE(lc2->GetStatus(t0_).fatal);

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
        // Spec Operator Reset: snapshot metadata + partial validation state cleared.
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kLifecycleStateKey).has_value());
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kLcBaseBlockKey).has_value());
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kLcBaseHeightKey).has_value());
        EXPECT_EQ(lc2->GetStatus(t0_).snapshot_base_height, 0u);
        EXPECT_EQ(lc2->GetStatus(t0_).target_validation_height, 0u);

        // A fresh attempt is now permitted.
        EXPECT_TRUE(lc2->OnSnapshotLoaded(base_block_, kBaseHeight));
    }
}

// Regression coverage for ResetAssumeUtxoFatalState (service-level).
//
// Service-level infeasibility verdict: ChainstateService::Init() requires
// LoggerService, ConfigService, and block_storage — all heavy production types
// with no lightweight test stubs.  No existing test constructs a
// ChainstateService (test_daemon_invariants.cpp explicitly skips such tests).
// Constructing one within ~100 lines is not feasible.  bg_validation_thread_ is
// private with no public accessor, so the join assertion from FIX 1 cannot be
// expressed at this level; that gap is carried into Task 9.
//
// What IS testable here: the two UTXOIndex-level operations that
// ResetAssumeUtxoFatalState calls (OperatorReset + ClearAll), sequenced as the
// service does, with assertions on state, metadata, and UTXO count.
TEST_F(AssumeUtxoLifecycleTest, OperatorResetThenClearAllWipesIndexAndMetadata) {
    // 1. Drive lifecycle into FatalMismatch (replicates service's precondition).
    {
        auto lc = MakeLifecycle();
        ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
        ASSERT_TRUE(lc->OnValidationStarted(t0_));
        lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
        ASSERT_FALSE(lc->OnReplayComplete(/*replay_performed=*/true,
                                          /*commitment_match=*/false,
                                          "deadbeef", "cafebabe", 0, t0_ + 20s));
        ASSERT_EQ(lc->GetState(), State::FatalMismatch);
    }

    // 2. Seed a UTXO into the index so GetUTXOCount() == 0 assertion is non-vacuous.
    {
        WalletUTXO dummy(
            TxId(uint256::FromHexUnsafe(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")),
            /*vout=*/0,
            AmountUna::DIN(1),
            /*spk=*/std::vector<uint8_t>{0x51},  // OP_1 (minimal scriptPubKey)
            /*path=*/"coinbase",
            /*height=*/1,
            /*is_coinbase=*/true);
        ASSERT_TRUE(utxo_index_->AddUTXO(dummy));
        const auto count_before = utxo_index_->GetUTXOCount();
        ASSERT_TRUE(count_before.isOk());
        ASSERT_GT(count_before.value(), 0u);
    }

    // 3. Restore and OperatorReset (lifecycle step of ResetAssumeUtxoFatalState).
    {
        auto lc2 = MakeLifecycle();
        lc2->RestoreFromPersistence(/*chainstate_matches_marker=*/true);
        ASSERT_EQ(lc2->GetState(), State::FatalMismatch);

        // Correct token transitions to Disabled and clears lifecycle metadata.
        ASSERT_TRUE(lc2->OperatorReset(assumeutxo::kResetToken));
        EXPECT_EQ(lc2->GetState(), State::Disabled);
        EXPECT_FALSE(lc2->GetStatus(t0_).fatal);

        // 4. ClearAll (UTXOIndex step of ResetAssumeUtxoFatalState).
        ASSERT_TRUE(utxo_index_->ClearAll());

        // 5. Assert all critical state is wiped (mirrors service postconditions).
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFatalReasonKey).has_value());
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kLifecycleStateKey).has_value());
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kLcBaseBlockKey).has_value());
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kLcBaseHeightKey).has_value());
        EXPECT_FALSE(utxo_index_->GetMetadata(assumeutxo::kFullyValidatedKey).has_value());

        // UTXO table wiped: the seeded dummy UTXO must be gone.
        const auto count_after = utxo_index_->GetUTXOCount();
        ASSERT_TRUE(count_after.isOk());
        EXPECT_EQ(count_after.value(), 0u);

        // bg_validation_thread_ join correctness (FIX 1) cannot be asserted here —
        // the thread member is private in ChainstateService with no public accessor,
        // and at this fixture level no background thread was ever started.  Task 9
        // carries this as a known coverage gap.
    }
}

// Spec Stall Semantics: server default stall timeout is 30 minutes. Pin it —
// every other test injects an explicit timeout, so without this a default
// change would pass the suite unnoticed.
TEST_F(AssumeUtxoLifecycleTest, DefaultStallTimeoutIsThirtyMinutes) {
    // Default-constructed timeout (no injection).
    assumeutxo::AssumeUtxoLifecycle lc(utxo_index_.get(), /*logger=*/nullptr);
    ASSERT_TRUE(lc.OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc.OnValidationStarted(t0_));
    lc.OnBlockValidated(5, t0_ + 5s);

    // 1799s after last progress: not yet stalled.
    lc.Tick(t0_ + 5s + 1799s);
    EXPECT_EQ(lc.GetState(), State::ValidatingHistory);
    // 1800s: stalled — pins the 30-minute default.
    lc.Tick(t0_ + 5s + 1800s);
    EXPECT_EQ(lc.GetState(), State::ValidationStalled);
}

}  // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
