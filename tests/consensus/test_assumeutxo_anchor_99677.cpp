// Consistency gate for the signed mainnet v4 snapshot published at height
// 99677. CI does not carry the 38 MB artifact, so this test cannot hash it; the
// artifact was independently downloaded, re-hashed, header-decoded, cross-checked
// against a fully synced fleet node, and Ed25519 signature-verified against the
// dedicated fleet snapshot-signing key before these constants were registered.

#include "consensus/assume_utxo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

using dinero::consensus::AssumeUTXORegistry;
using dinero::consensus::AssumeUTXOSnapshot;

constexpr char kManifestSha256[] =
    "d4b8d88c2fe765aa627ed0bc12205b2cfba6e4fa50aeb72fd9924990e83c29fd";
constexpr char kManifestBlockHash[] =
    "0000003863308eb65aff8a1915ef50febc74d834e444e56b26d89d987999268b";
constexpr uint32_t kManifestHeight = 99677;
constexpr uint64_t kHeaderUtxoCount = 309354;
constexpr char kChainwork[] =
    "0x00000000000000000000000000000000000000000000000000003eead5aa12b0";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

TEST(AssumeUtxoAnchor99677, RegistryMatchesVerifiedArtifact) {
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

TEST(AssumeUtxoAnchor99677, OlderLifecycleFallbacksRemainRegistered) {
    // Every previously shipped anchor must stay registered so a node that was
    // interrupted mid-lifecycle can restart against its exact original base
    // instead of being forced onto this newer one.
    for (const uint32_t height : {52287u, 65300u, 73035u, 84131u}) {
        EXPECT_TRUE(AssumeUTXORegistry::GetSnapshot(height).has_value())
            << "anchor " << height << " must remain registered as a fallback";
    }
}

TEST(AssumeUtxoAnchor99677, RegistryHeightsAreUniqueAndNeighborsUnregistered) {
    EXPECT_FALSE(AssumeUTXORegistry::GetSnapshot(99676).has_value());
    EXPECT_FALSE(AssumeUTXORegistry::GetSnapshot(99678).has_value());

    const auto all = AssumeUTXORegistry::GetAllSnapshots();
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            EXPECT_NE(all[i].height, all[j].height)
                << "duplicate registry height would make GetSnapshot order-dependent";
        }
    }
}

TEST(AssumeUtxoAnchor99677, IsTheNewestRegisteredAnchor) {
    // The whole point of adding it: a fresh install should land near the tip.
    // If a newer anchor is added later without updating this, that is a signal
    // to re-point the shipped snapshot too.
    const auto all = AssumeUTXORegistry::GetAllSnapshots();
    ASSERT_FALSE(all.empty());
    uint32_t newest = 0;
    for (const auto& s : all) {
        newest = std::max(newest, s.height);
    }
    EXPECT_EQ(newest, kManifestHeight);
}

}  // namespace
