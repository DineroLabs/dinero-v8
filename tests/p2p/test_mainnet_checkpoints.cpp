#include <gtest/gtest.h>

#include "consensus/assume_utxo.h"

namespace dinero::p2p {

// The former hard-coded-checkpoint-anchors test was removed with the dead
// header-sync module it depended on; its anchors were redundant with the live
// AssumeUTXO registry asserted below and the header-replay tests (genesis).
// See git history for the removal rationale.
//
// The production AssumeUTXO trust anchor is h=52241 (v4: UXTO+UTRX+SHLD). Any
// snapshot whose base is above the shielded activation height (8650) MUST be
// v4+ and carry the shielded (SHLD) section, so the earlier v3/no-shielded
// anchors (13000/33048/47176/52066) were removed — a v3 snapshot fast-syncs a
// node into a shielded-blind state. These tests pin the live anchor against
// accidental mutation and guard against reintroducing the v3 anchors.

TEST(MainnetCheckpoints, AssumeUTXOSnapshot52287NeverChanges) {
    auto snapshot = dinero::consensus::AssumeUTXORegistry::GetSnapshot(52287);
    ASSERT_TRUE(snapshot.has_value())
        << "CRITICAL: h=52287 v4 AssumeUTXO registry entry missing";
    EXPECT_EQ(snapshot->snapshot_hash.GetHex(),
              "48f7672cc855c83cd8968fddab85a87d4cd8c41aa8562c91bff475a318db399c");
    EXPECT_EQ(snapshot->block_hash.GetHex(),
              "000000739c14918aae1985948b1d800cbab8473edf117c155ba9ada186cba71e");
    EXPECT_EQ(snapshot->height, 52287u);
    EXPECT_EQ(snapshot->utxo_count, 154475u);
}

TEST(MainnetCheckpoints, V3AnchorsRemoved) {
    // v3/no-shielded anchors must never be reintroduced: they fast-sync a node
    // into a shielded-blind state above the shielded activation height (8650).
    EXPECT_FALSE(dinero::consensus::AssumeUTXORegistry::GetSnapshot(13000).has_value())
        << "v3 anchor h=13000 was reintroduced";
    EXPECT_FALSE(dinero::consensus::AssumeUTXORegistry::GetSnapshot(33048).has_value())
        << "v3 anchor h=33048 was reintroduced";
    EXPECT_FALSE(dinero::consensus::AssumeUTXORegistry::GetSnapshot(47176).has_value())
        << "v3 anchor h=47176 was reintroduced";
    EXPECT_FALSE(dinero::consensus::AssumeUTXORegistry::GetSnapshot(52066).has_value())
        << "v3 anchor h=52066 was reintroduced";
}

}  // namespace dinero::p2p
