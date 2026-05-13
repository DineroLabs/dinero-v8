#include <gtest/gtest.h>

#include "consensus/assume_utxo.h"
#include "p2p/multi_peer_headers_sync.h"

namespace dinero::p2p {

TEST(MainnetCheckpoints, UsesRealDineroChainAnchors) {
    const auto checkpoints = getCheckpoints();

    ASSERT_GE(checkpoints.size(), 2u);

    for (size_t i = 1; i < checkpoints.size(); ++i) {
        EXPECT_LT(checkpoints[i - 1].height, checkpoints[i].height);
        EXPECT_NE(checkpoints[i - 1].block_hash, checkpoints[i].block_hash);
    }

    EXPECT_EQ(checkpoints.front().height, 0u);
    EXPECT_EQ(checkpoints.front().block_hash,
              "0000001c36abf27e2c233ff40ed0c08888926c24450f3bff82a047ae1528b76f");
    EXPECT_NE(checkpoints.front().block_hash,
              "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f");

    EXPECT_EQ(checkpoints.back().height, 13000u);
    EXPECT_EQ(checkpoints.back().block_hash,
              "0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3");
}

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
