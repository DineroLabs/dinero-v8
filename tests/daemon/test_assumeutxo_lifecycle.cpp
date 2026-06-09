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

}  // namespace dinero

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
