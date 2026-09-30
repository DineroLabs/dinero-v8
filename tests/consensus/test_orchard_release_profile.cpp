#include "consensus/orchard_profile.h"
#include <gtest/gtest.h>

namespace {
dinero::ChainParams OrchardReleaseFixture(const char* name) {
    dinero::ChainParams p{};
    p.name = name;
    p.shielded_activation_height = 1;
    p.shielded_input_binding_activation_height = 2;
    p.shielded_cv_binding_activation_height = p.shielded_epoch_reset_height = 3;
    p.shielded_spend_auth_activation_height = p.shielded_spend_auth_epoch_reset_height = 4;
    return p;
}
using namespace dinero::consensus;
TEST(OrchardReleaseProfile, PublicReleaseRequiresExplicitMatchingOrchardOwner) {
    for (const char* name : {"mainnet", "testnet"}) {
        auto p = OrchardReleaseFixture(name);
        EXPECT_TRUE(OrchardProfileConfigurationValid(p));
        ConfigureReleaseV8113(p, 10);
        EXPECT_TRUE(ReleaseProfileConfigurationValid(p));
        EXPECT_FALSE(OrchardProfileConfigurationValid(p));
        EXPECT_FALSE(OrchardServiceCutoffActive(p, 9));
        p.orchard_activation_height = 10;
        EXPECT_FALSE(OrchardProfileConfigurationValid(p));
        p.orchard_branch_id = 1; // synthetic local copy, never a production branch ID
        EXPECT_TRUE(OrchardProfileConfigurationValid(p));
        for (auto field : {&dinero::ChainParams::release_v8113_activation_height,
                           &dinero::ChainParams::shielded_compact_activation_height,
                           &dinero::ChainParams::sixty_second_activation_height,
                           &dinero::ChainParams::orchard_activation_height}) {
            auto invalid = p;
            invalid.*field = 11;
            EXPECT_FALSE(OrchardProfileConfigurationValid(invalid));
            EXPECT_FALSE(OrchardServiceCutoffActive(invalid, 100));
        }
    }
}
TEST(OrchardReleaseProfile, ValidatedNextHeightRewindAndDormantSentinel) {
    for (const char* name : {"mainnet", "testnet", "regtest"}) {
        auto p = OrchardReleaseFixture(name);
        EXPECT_FALSE(OrchardServiceCutoffActive(p, UINT32_MAX));
        ConfigureOrchardRelease(p, 10, 1);
        EXPECT_FALSE(OrchardServiceCutoffActive(p, 8));
        EXPECT_TRUE(OrchardServiceCutoffActive(p, 9));
        EXPECT_TRUE(OrchardServiceCutoffActive(p, UINT32_MAX));
        EXPECT_FALSE(OrchardServiceCutoffActive(p, 8));
        ConfigureOrchardRelease(p, UINT32_MAX, 0);
        EXPECT_TRUE(OrchardProfileConfigurationValid(p));
        EXPECT_FALSE(OrchardServiceCutoffActive(p, UINT32_MAX));
        EXPECT_EQ(p.release_v8113_activation_height, UINT32_MAX);
        EXPECT_EQ(p.shielded_compact_activation_height, UINT32_MAX);
        EXPECT_EQ(p.sixty_second_activation_height, UINT32_MAX);
        EXPECT_EQ(p.orchard_branch_id, 0u);
    }
}
TEST(OrchardReleaseProfile, IndependentRegtestSchedulesRemainExplicit) {
    auto p = OrchardReleaseFixture("regtest");
    ConfigureReleaseV8113(p, 10);
    EXPECT_TRUE(OrchardProfileConfigurationValid(p));
    EXPECT_TRUE(ReleaseServiceCutoffActive(p, 9));
    EXPECT_FALSE(OrchardServiceCutoffActive(p, 9));
    ConfigureReleaseV8113(p, UINT32_MAX);
    p.orchard_activation_height = 7;
    p.orchard_branch_id = 1;
    EXPECT_TRUE(OrchardProfileConfigurationValid(p));
    EXPECT_FALSE(ReleaseServiceCutoffActive(p, 100));
    EXPECT_FALSE(OrchardServiceCutoffActive(p, 5));
    EXPECT_TRUE(OrchardServiceCutoffActive(p, 6));
    p.name = "unknown";
    EXPECT_FALSE(OrchardProfileConfigurationValid(p));
    EXPECT_FALSE(OrchardServiceCutoffActive(p, 100));
}
TEST(OrchardReleaseProfile, InvalidUpdatesPreserveTheCompletePriorSchedule) {
    auto p = OrchardReleaseFixture("mainnet");
    ConfigureOrchardRelease(p, 10, 1);
    for (auto invalid : {std::pair<uint32_t, uint32_t>{0, 1}, {10, 0},
                         {uint32_t(INT32_MAX) + 1, 1}, {UINT32_MAX, 1}}) {
        EXPECT_THROW(ConfigureOrchardRelease(p, invalid.first, invalid.second), std::invalid_argument);
        EXPECT_EQ(p.release_v8113_activation_height, 10u);
        EXPECT_EQ(p.shielded_compact_activation_height, 10u);
        EXPECT_EQ(p.sixty_second_activation_height, 10u);
        EXPECT_EQ(p.orchard_activation_height, 10u);
        EXPECT_EQ(p.orchard_branch_id, 1u);
    }
}
} // namespace
