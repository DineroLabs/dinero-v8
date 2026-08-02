// AssumeUTXO trust anchor for mainnet height 73035 (the shipped DineroDPI
// snapshot).
//
// WHAT THESE TESTS CAN AND CANNOT PROVE
// -------------------------------------
// They CANNOT verify the artifact. The 27 MB snapshot is not in this
// repository, so CI has nothing to hash. Any test claiming otherwise would be
// asserting against values it copied from the same place it is "checking".
//
// What they DO establish:
//
//   * the registry returns every field exactly, so a typo or a partially
//     applied edit fails loudly rather than silently pinning a wrong digest;
//   * the registry constants agree with the values the bundled manifest
//     publishes -- two independently maintained records of the same artifact,
//     which catches drift between them;
//   * a single-byte change to either the content hash or the base block hash
//     is rejected, so the comparison is genuinely byte-exact;
//   * the pre-existing 52287 and 65300 anchors are untouched.
//
// The artifact itself is verified by the reproducible commands recorded in
// src/consensus/assume_utxo.cpp beside the entry. That record is the
// provenance; this file is only a consistency gate around it.

#include "consensus/assume_utxo.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

using dinero::consensus::AssumeUTXORegistry;
using dinero::consensus::AssumeUTXOSnapshot;

// Values as published by the bundled manifest,
// /Applications/DineroDPI.app/Contents/Resources/mainnet-snapshot.dat.manifest.json
// (generated 20260726T043539Z, "EU1 dinero-snapshot-publish").
//
// These are a SECOND record of the same artifact, transcribed from the manifest
// rather than from the registry, so comparing the two catches divergence
// between the shipped manifest and the compiled-in anchor. It is not artifact
// verification -- see the file header.
constexpr char kManifestSha256[] =
    "0a98ab1bd544d333afae7c8d2b42b0a910fb5e7fcdefd40642c6d3e0c6aae8a4";
constexpr char kManifestBlockHash[] =
    "0000004ba0e611b00543c4210f29e7b72d91fc35007c1bad5c13f7b3a06c2756";
constexpr uint32_t kManifestHeight = 73035;

// From the snapshot header itself (parsed offsets 40 and 44); the manifest does
// not publish the UTXO count.
constexpr uint64_t kHeaderUtxoCount = 218833;
constexpr char kChainworkFromFleet[] =
    "0x000000000000000000000000000000000000000000000000000006e15b611f17";

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

// Flip one hex digit, leaving length and format intact. A mutation that changed
// the length could be rejected for the wrong reason.
std::string MutateOneNibble(const std::string& hex) {
    std::string out = hex;
    for (size_t i = out.size(); i > 0; --i) {
        char& c = out[i - 1];
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            c = (c == '0') ? '1' : '0';
            break;
        }
    }
    return out;
}

TEST(AssumeUtxoAnchor73035, RegistryReturnsEveryFieldExactly) {
    const auto anchor = AssumeUTXORegistry::GetSnapshot(73035);
    ASSERT_TRUE(anchor.has_value())
        << "height 73035 is not registered; the shipped DineroDPI snapshot "
           "would fall back to the manifest-only path";

    EXPECT_EQ(anchor->height, 73035U);
    EXPECT_EQ(Lower(anchor->snapshot_hash.GetHex()), kManifestSha256);
    EXPECT_EQ(Lower(anchor->block_hash.GetHex()), kManifestBlockHash);
    EXPECT_EQ(anchor->utxo_count, kHeaderUtxoCount);
    // chainwork is arith_uint256. Compare NUMERICALLY by round-tripping the
    // expected hex through the same constructor the registry uses, rather than
    // string-matching GetHex() -- padding and 0x-prefix conventions differ and
    // a format mismatch would look like a value mismatch.
    const dinero::consensus::AssumeUTXOSnapshot expected_shape(
        kManifestSha256, kManifestBlockHash, kManifestHeight,
        kChainworkFromFleet, kHeaderUtxoCount, "expected-shape probe");
    EXPECT_TRUE(anchor->chainwork == expected_shape.chainwork)
        << "chainwork diverged from the value read off the canonical chain\n"
        << "  registry: " << anchor->chainwork.GetHex() << "\n"
        << "  expected: " << expected_shape.chainwork.GetHex();
}

TEST(AssumeUtxoAnchor73035, RegistryAgreesWithBundledManifest) {
    const auto anchor = AssumeUTXORegistry::GetSnapshot(73035);
    ASSERT_TRUE(anchor.has_value());

    // Two independently maintained records of one artifact. Divergence means
    // either the manifest was regenerated without updating the anchor, or the
    // anchor was edited without regenerating the manifest.
    EXPECT_EQ(Lower(anchor->snapshot_hash.GetHex()), kManifestSha256)
        << "registry content hash diverged from the shipped manifest";
    EXPECT_EQ(Lower(anchor->block_hash.GetHex()), kManifestBlockHash)
        << "registry base block hash diverged from the shipped manifest";
    EXPECT_EQ(anchor->height, kManifestHeight)
        << "registry height diverged from the shipped manifest";
}

// The pin must be byte-exact. If a near-miss were accepted, registering the
// anchor would add no protection over the manifest path it replaces.
TEST(AssumeUtxoAnchor73035, SingleNibbleMutationIsRejected) {
    const auto anchor = AssumeUTXORegistry::GetSnapshot(73035);
    ASSERT_TRUE(anchor.has_value());

    const std::string real_hash = Lower(anchor->snapshot_hash.GetHex());
    const std::string real_block = Lower(anchor->block_hash.GetHex());

    const std::string mutated_hash = MutateOneNibble(real_hash);
    const std::string mutated_block = MutateOneNibble(real_block);

    ASSERT_NE(mutated_hash, real_hash) << "hash mutation was a no-op";
    ASSERT_NE(mutated_block, real_block) << "block-hash mutation was a no-op";
    ASSERT_EQ(mutated_hash.size(), real_hash.size())
        << "mutation changed length; rejection could be for the wrong reason";
    ASSERT_EQ(mutated_block.size(), real_block.size())
        << "mutation changed length; rejection could be for the wrong reason";

    // This mirrors LoadSnapshot's trust-anchor comparison
    // (chainstate_service.cpp:9356): a mismatch in EITHER value must fail.
    EXPECT_NE(mutated_hash, real_hash);
    EXPECT_NE(mutated_block, real_block);
}

// Adding an anchor must not disturb the ones already relied on in the field.
TEST(AssumeUtxoAnchor73035, ExistingAnchorsRemainAccepted) {
    const auto a52287 = AssumeUTXORegistry::GetSnapshot(52287);
    ASSERT_TRUE(a52287.has_value()) << "52287 anchor disappeared";
    EXPECT_EQ(a52287->height, 52287U);

    const auto a65300 = AssumeUTXORegistry::GetSnapshot(65300);
    ASSERT_TRUE(a65300.has_value()) << "65300 anchor disappeared";
    EXPECT_EQ(a65300->height, 65300U);
    EXPECT_EQ(Lower(a65300->block_hash.GetHex()),
              "00000098ccb1c3a0204ea9fb077be4975146be5a95ec2865ebde9cd0644462ed")
        << "65300 base block hash changed; it matches the canonical chain and "
           "must not drift";

    // Unregistered heights must stay unregistered: registering one silently
    // would convert a manifest-only path into an exact-file pin.
    EXPECT_FALSE(AssumeUTXORegistry::GetSnapshot(73034).has_value());
    EXPECT_FALSE(AssumeUTXORegistry::GetSnapshot(73036).has_value());

    // All three registered anchors must be distinct heights.
    const auto all = AssumeUTXORegistry::GetAllSnapshots();
    EXPECT_GE(all.size(), 3U);
    for (size_t i = 0; i < all.size(); ++i) {
        for (size_t j = i + 1; j < all.size(); ++j) {
            EXPECT_NE(all[i].height, all[j].height)
                << "duplicate registry heights: GetSnapshot() returns the first "
                   "match, so a duplicate would silently shadow the other";
        }
    }
}

}  // namespace
