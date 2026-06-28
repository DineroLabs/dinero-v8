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

TEST(MainnetCheckpoints, AssumeUTXOSnapshot52241NeverChanges) {
    auto snapshot = dinero::consensus::AssumeUTXORegistry::GetSnapshot(52241);
    ASSERT_TRUE(snapshot.has_value())
        << "CRITICAL: h=52241 v4 AssumeUTXO registry entry missing";
    EXPECT_EQ(snapshot->snapshot_hash.GetHex(),
              "23d987253c3eefb9d8521d6c4086350e0ac5d96e80296be4b31dbd15382063f6");
    EXPECT_EQ(snapshot->block_hash.GetHex(),
              "00000088a18ee05d5fdeaa452a1efaa1845b2d6feb8a3046c139262b7f4c2a7a");
    EXPECT_EQ(snapshot->height, 52241u);
    EXPECT_EQ(snapshot->utxo_count, 154337u);
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
