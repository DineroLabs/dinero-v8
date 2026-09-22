#include "consensus/release_profile.h"
#include <gtest/gtest.h>

namespace {
auto Profile(const char* name) {
    dinero::ChainParams p{}; p.name = name;
    p.shielded_activation_height = 1;
    p.shielded_input_binding_activation_height = 2;
    p.shielded_cv_binding_activation_height = p.shielded_epoch_reset_height = 3;
    p.shielded_spend_auth_activation_height = p.shielded_spend_auth_epoch_reset_height = 4;
    return p;
}
using namespace dinero::consensus;
TEST(ReleaseProfile, CoupledBoundaryReorgAndDormantSentinel) {
    for (auto name : {"mainnet", "testnet", "regtest"}) {
        auto p = Profile(name);
        EXPECT_TRUE(ReleaseProfileConfigurationValid(p));
        EXPECT_FALSE(ReleaseServiceCutoffActive(p, UINT32_MAX));
        ConfigureReleaseV8113(p, 10);
        EXPECT_EQ(p.shielded_compact_activation_height, 10u);
        EXPECT_EQ(p.sixty_second_activation_height, 10u);
        EXPECT_FALSE(ReleaseServiceCutoffActive(p, 8));
        EXPECT_TRUE(ReleaseServiceCutoffActive(p, 9));
        EXPECT_TRUE(ReleaseServiceCutoffActive(p, UINT32_MAX));
        EXPECT_FALSE(ReleaseServiceCutoffActive(p, 8));
        ConfigureReleaseV8113(p, UINT32_MAX);
        EXPECT_FALSE(ReleaseServiceCutoffActive(p, UINT32_MAX));
    }
}
TEST(ReleaseProfile, PublicPartialSchedulesAndMismatchedJointProfilesRefused) {
    for (auto name : {"mainnet", "testnet"}) {
        auto p = Profile(name);
        p.sixty_second_activation_height = 10;
        EXPECT_FALSE(ReleaseProfileConfigurationValid(p));
        p.shielded_compact_activation_height = 10;
        EXPECT_FALSE(ReleaseProfileConfigurationValid(p));
        ConfigureReleaseV8113(p, 10);
        for (auto field : {&dinero::ChainParams::sixty_second_activation_height,
                           &dinero::ChainParams::shielded_compact_activation_height}) {
            auto bad = p; bad.*field = 11;
            EXPECT_FALSE(ReleaseProfileConfigurationValid(bad));
            EXPECT_FALSE(ReleaseServiceCutoffActive(bad, 100));
        }
    }
}
TEST(ReleaseProfile, InvalidScheduleLeavesOriginalUntouched) {
    auto p = Profile("mainnet");
    for (uint32_t invalid : {0, 1, 4}) {
        EXPECT_THROW(ConfigureReleaseV8113(p, invalid), std::invalid_argument);
        EXPECT_EQ(p.release_v8113_activation_height, UINT32_MAX);
        EXPECT_EQ(p.shielded_compact_activation_height, UINT32_MAX);
        EXPECT_EQ(p.sixty_second_activation_height, UINT32_MAX);
    }
}
TEST(ReleaseProfile, IndependentRegtestExperimentsDoNotEnableReleaseCutoff) {
    auto p = Profile("regtest");
    p.sixty_second_activation_height = 4;
    p.shielded_compact_activation_height = 10;
    EXPECT_TRUE(ReleaseProfileConfigurationValid(p));
    EXPECT_FALSE(ReleaseServiceCutoffActive(p, 100));
}
} // namespace
