#include "nodecore/sync_profile_policy.h"
#include "nodecore/nodecore_ffi.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

using dinero::nodecore::CapabilitiesForProfile;
using dinero::nodecore::DefaultSyncProfile;
using dinero::nodecore::DefaultUtreexoStateless;
using dinero::nodecore::ResolveSyncProfile;

namespace {

TEST(SyncProfilePolicy, MissingProfileUsesPlatformDefault) {
    const auto resolved = ResolveSyncProfile(std::nullopt, std::nullopt);

    ASSERT_TRUE(resolved.ok);
    EXPECT_EQ(resolved.profile, DefaultSyncProfile());
    EXPECT_EQ(resolved.utreexo_stateless, DefaultUtreexoStateless());
}

TEST(SyncProfilePolicy, InvalidProfileRejected) {
    const auto resolved = ResolveSyncProfile(std::optional<std::string>{"invalid_profile"}, std::nullopt);

    EXPECT_FALSE(resolved.ok);
    EXPECT_NE(resolved.error.find("Invalid sync_profile"), std::string::npos);
}

TEST(SyncProfilePolicy, ExplicitProfileWinsOverLegacyFlag) {
    const auto mac_overrides_legacy = ResolveSyncProfile(
        std::optional<std::string>{"mac_fullblock"},
        std::optional<bool>{true}
    );
    ASSERT_TRUE(mac_overrides_legacy.ok);
    EXPECT_EQ(mac_overrides_legacy.profile, "mac_fullblock");
    EXPECT_FALSE(mac_overrides_legacy.utreexo_stateless);

    const auto ios_overrides_legacy = ResolveSyncProfile(
        std::optional<std::string>{"ios_utreexo"},
        std::optional<bool>{false}
    );
    ASSERT_TRUE(ios_overrides_legacy.ok);
    EXPECT_EQ(ios_overrides_legacy.profile, "ios_utreexo");
    EXPECT_TRUE(ios_overrides_legacy.utreexo_stateless);
}

TEST(SyncProfilePolicy, LegacyFallbackWhenProfileMissing) {
    const auto legacy_true = ResolveSyncProfile(std::nullopt, std::optional<bool>{true});
    ASSERT_TRUE(legacy_true.ok);
    EXPECT_EQ(legacy_true.profile, "ios_utreexo");
    EXPECT_TRUE(legacy_true.utreexo_stateless);

    const auto legacy_false = ResolveSyncProfile(std::nullopt, std::optional<bool>{false});
    ASSERT_TRUE(legacy_false.ok);
    EXPECT_EQ(legacy_false.profile, "mac_fullblock");
    EXPECT_FALSE(legacy_false.utreexo_stateless);
}

TEST(SyncProfilePolicy, CapabilitiesMatchProfileContract) {
    EXPECT_EQ(CapabilitiesForProfile("ios_utreexo"), NODECORE_CAP_SYNC_STATELESS);

    const uint64_t mac_expected = NODECORE_CAP_SYNC_FULLBLOCK | NODECORE_CAP_MINING_LOCAL | NODECORE_CAP_MINING_POOL;
    EXPECT_EQ(CapabilitiesForProfile("mac_fullblock"), mac_expected);
}

}  // namespace

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
