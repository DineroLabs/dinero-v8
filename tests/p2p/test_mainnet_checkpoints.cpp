#include <gtest/gtest.h>

#include "consensus/assume_utxo.h"

namespace dinero::p2p {

// The former hard-coded-checkpoint-anchors test was removed with the dead
// header-sync module it depended on; its anchors were redundant with the live
// AssumeUTXO registry asserted below (h=13000) and the header-replay tests
// (genesis). See git history for the removal rationale.

TEST(MainnetCheckpoints, AssumeUTXOSnapshot13000NeverChanges) {
    auto snapshot = dinero::consensus::AssumeUTXORegistry::GetSnapshot(13000);
    ASSERT_TRUE(snapshot.has_value())
        << "CRITICAL: h=13000 AssumeUTXO registry entry missing";
    EXPECT_EQ(snapshot->snapshot_hash.GetHex(),
              "04afcb937b07ccab469dd6ade5151cd06431b30111d813c4392303cc7b1b2426");
    EXPECT_EQ(snapshot->block_hash.GetHex(),
              "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3");
    EXPECT_EQ(snapshot->height, 13000u);
    EXPECT_EQ(snapshot->utxo_count, 38700u);
}

}  // namespace dinero::p2p
