// Consistency gate for the signed mainnet v4 snapshot published at height
// 84131. CI does not carry the 31 MB artifact, so this test cannot hash it; the
// artifact was independently hashed, signature-verified, and imported through
// the real offline LoadSnapshot path before these constants were registered.

#include "consensus/assume_utxo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

using dinero::consensus::AssumeUTXORegistry;
using dinero::consensus::AssumeUTXOSnapshot;

constexpr char kManifestSha256[] =
    "7defc1897055ba24a92985e30ce35123181debc272c86e5fd2d190e436802845";
constexpr char kManifestBlockHash[] =
    "0000000023974d67c7a1a5dc04b7d63764b0f41b756796330615d39bc6792123";
constexpr uint32_t kManifestHeight = 84131;
constexpr uint64_t kHeaderUtxoCount = 252129;
constexpr char kChainwork[] =
    "0x00000000000000000000000000000000000000000000000000002733d734bd86";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

TEST(AssumeUtxoAnchor84131, RegistryMatchesVerifiedArtifact) {
    const auto anchor = AssumeUTXORegistry::GetSnapshot(kManifestHeight);
    ASSERT_TRUE(anchor.has_value());

    EXPECT_EQ(anchor->height, kManifestHeight);
    EXPECT_EQ(Lower(anchor->snapshot_hash.GetHex()), kManifestSha256);
    EXPECT_EQ(Lower(anchor->block_hash.GetHex()), kManifestBlockHash);
    EXPECT_EQ(anchor->utxo_count, kHeaderUtxoCount);

    const AssumeUTXOSnapshot expected(
        kManifestSha256, kManifestBlockHash, kManifestHeight, kChainwork,
        kHeaderUtxoCount, "expected-shape probe");
    EXPECT_TRUE(anchor->chainwork == expected.chainwork);

}

TEST(AssumeUtxoAnchor84131, OlderLifecycleFallbackRemainsRegistered) {
    const auto fallback = AssumeUTXORegistry::GetSnapshot(73035);
    ASSERT_TRUE(fallback.has_value())
        << "the v8.1.2 package retains height 73035 for interrupted older lifecycles";
    EXPECT_EQ(Lower(fallback->snapshot_hash.GetHex()),
              "0a98ab1bd544d333afae7c8d2b42b0a910fb5e7fcdefd40642c6d3e0c6aae8a4");
    EXPECT_EQ(Lower(fallback->block_hash.GetHex()),
              "0000004ba0e611b00543c4210f29e7b72d91fc35007c1bad5c13f7b3a06c2756");
}

TEST(AssumeUtxoAnchor84131, RegistryHeightsAreUniqueAndNeighborsUnregistered) {
    EXPECT_FALSE(AssumeUTXORegistry::GetSnapshot(84130).has_value());
    EXPECT_FALSE(AssumeUTXORegistry::GetSnapshot(84132).has_value());

    const auto all = AssumeUTXORegistry::GetAllSnapshots();
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            EXPECT_NE(all[i].height, all[j].height)
                << "duplicate registry height would make GetSnapshot order-dependent";
        }
    }
}

}  // namespace
