/**
 * Regression tests for UtreexoForest serialize/deserialize integrity.
 *
 * Incident (on-device, 2026-07-16): a stateless phone node halted at height
 * 62742 with `[StatelessNode] FAIL step 5 ... leaf not found in forest at
 * index 0/1` for a leaf created at 62006 by its own root-verified apply —
 * while every root check (step 2 stump continuity, step 6 root_after, #382
 * forward-connect header verification) kept PASSING. The only object shape
 * consistent with that evidence is a "husk": a forest whose roots_ (and thus
 * getCommitment()) match the canonical accumulator while nodes_ and
 * leaf_positions_ are empty or partial.
 *
 * UtreexoForest::deserialize can manufacture exactly that husk: the payload
 * parses numLeaves_ and roots_ FIRST, and several early returns between the
 * roots section and the nodes section ("backward compatibility: old format
 * without nodes_", assorted insufficient-data guards) return the partially
 * populated forest. A checkpoint blob that ends after the roots section — a
 * legacy writer or any truncation — restores as a forest that impersonates
 * the real one to every root check and fails every leaf lookup, permanently
 * wedging the first spend block after the restart.
 *
 * Contract these tests pin down: deserialize either reproduces the FULL
 * forest (every live leaf findable and provable) or fails LOUDLY by
 * returning an EMPTY forest (numLeaves == 0, empty commitment) — the same
 * convention the function's own tail-validation failures already use. It
 * must never return a rooted husk.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "consensus/utreexo_accumulator.h"

using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;

namespace {

UtreexoHash MakeLeaf(uint8_t seed) {
    UtreexoHash h(32, 0);
    for (size_t i = 0; i < h.size(); ++i) {
        h[i] = static_cast<uint8_t>(seed + i * 31 + 7);
    }
    return h;
}

// Build a forest with history: adds, then proof-based removals, then more
// adds — the shape a live CSN forest has after crossing spend blocks.
void BuildForestWithHistory(UtreexoForest& forest, std::vector<UtreexoHash>& live_out) {
    std::vector<UtreexoHash> leaves;
    for (uint8_t i = 0; i < 16; ++i) {
        leaves.push_back(MakeLeaf(i));
        ASSERT_NE(forest.add(leaves.back()), UINT64_MAX);
    }
    // Remove a few leaves via real proofs (tombstones deleted positions).
    for (uint8_t i : {2, 7, 11}) {
        auto pos = forest.findLeafPosition(leaves[i]);
        ASSERT_TRUE(pos.has_value()) << "setup: leaf " << int(i) << " must be findable";
        auto proof = forest.prove(pos.value());
        ASSERT_TRUE(proof.has_value()) << "setup: leaf " << int(i) << " must be provable";
        ASSERT_TRUE(forest.remove(leaves[i], proof.value()));
    }
    // Add more after the deletes (post-delete appends, like block 62006's adds).
    for (uint8_t i = 100; i < 105; ++i) {
        leaves.push_back(MakeLeaf(i));
        ASSERT_NE(forest.add(leaves.back()), UINT64_MAX);
    }

    live_out.clear();
    for (size_t i = 0; i < leaves.size(); ++i) {
        if (i == 2 || i == 7 || i == 11) continue;  // removed above
        live_out.push_back(leaves[i]);
    }
}

}  // namespace

// Baseline: a faithful round-trip must preserve every live leaf's
// findability and provability, not just the roots.
TEST(ForestSerializationIntegrity, RoundTripPreservesLiveLeafLookups)
{
    std::vector<UtreexoHash> live;
    UtreexoForest forest;
    BuildForestWithHistory(forest, live);
    if (::testing::Test::HasFatalFailure()) return;

    const auto blob = forest.serialize();
    UtreexoForest restored = UtreexoForest::deserialize(blob);

    EXPECT_EQ(restored.getCommitment(), forest.getCommitment());
    EXPECT_EQ(restored.getNumLeaves(), forest.getNumLeaves());
    for (const auto& leaf : live) {
        auto pos = restored.findLeafPosition(leaf);
        ASSERT_TRUE(pos.has_value())
            << "live leaf lost by serialize/deserialize round-trip";
        EXPECT_TRUE(restored.prove(pos.value()).has_value())
            << "live leaf unprovable after round-trip";
    }
}

// THE incident test: no truncation of the payload may deserialize into a
// forest that carries the ORIGINAL commitment while losing leaf lookups.
// Every prefix must restore either the full forest or an empty one.
TEST(ForestSerializationIntegrity, TruncatedPayloadNeverYieldsRootedHusk)
{
    std::vector<UtreexoHash> live;
    UtreexoForest forest;
    BuildForestWithHistory(forest, live);
    if (::testing::Test::HasFatalFailure()) return;
    const auto original_commitment = forest.getCommitment();

    const auto blob = forest.serialize();
    ASSERT_FALSE(blob.empty());

    for (size_t len = 0; len < blob.size(); ++len) {
        std::vector<uint8_t> prefix(blob.begin(), blob.begin() + len);
        UtreexoForest restored = UtreexoForest::deserialize(prefix);

        if (restored.getCommitment() != original_commitment) {
            continue;  // failed loudly (empty/garbage) — acceptable
        }

        // The restored forest claims to BE the original (same commitment):
        // then it must actually contain it. A rooted husk here is the
        // on-device wedge: passes every root check, fails every spend.
        for (const auto& leaf : live) {
            ASSERT_TRUE(restored.findLeafPosition(leaf).has_value())
                << "truncation at " << len << "/" << blob.size()
                << " produced a ROOTED HUSK: original commitment with a "
                   "missing live leaf — the 62742 on-device wedge shape";
        }
    }
}

// A payload that ends after the roots section (the legacy "old format
// without nodes_" shape) must fail loudly, not restore as a rooted husk.
TEST(ForestSerializationIntegrity, RootsOnlyLegacyPayloadFailsLoudly)
{
    std::vector<UtreexoHash> live;
    UtreexoForest forest;
    BuildForestWithHistory(forest, live);
    if (::testing::Test::HasFatalFailure()) return;

    // Reconstruct the exact byte offset where the nodes section begins:
    // version(1) + flag(1) + numLeaves(8) + numRoots(4) + 32*numRoots.
    const auto blob = forest.serialize();
    ASSERT_GE(blob.size(), 14u);
    const uint32_t num_roots = static_cast<uint32_t>(blob[10]) |
                               (static_cast<uint32_t>(blob[11]) << 8) |
                               (static_cast<uint32_t>(blob[12]) << 16) |
                               (static_cast<uint32_t>(blob[13]) << 24);
    const size_t roots_end = 14 + static_cast<size_t>(num_roots) * 32;
    ASSERT_LT(roots_end, blob.size()) << "fixture must have a nodes section";

    std::vector<uint8_t> roots_only(blob.begin(), blob.begin() + roots_end);
    UtreexoForest restored = UtreexoForest::deserialize(roots_only);

    // Loud failure = empty forest (the tail-validation convention).
    EXPECT_EQ(restored.getNumLeaves(), 0u)
        << "roots-only payload must not restore a rooted husk";
    EXPECT_NE(restored.getCommitment(), forest.getCommitment())
        << "husk impersonates the original accumulator to every root check";
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
