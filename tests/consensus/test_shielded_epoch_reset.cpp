// Copyright (c) 2026 Dinero Labs.
//
// Test-first: the shielded epoch reset (hard-fork cutover) must discard ALL
// pre-cutover pool state so old notes become unspendable and the new epoch
// starts from a deterministic empty pool. These FAIL against the no-op stub in
// shielded_epoch.cpp and pass once ResetShieldedEpoch is implemented.
#include <gtest/gtest.h>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_epoch.h"

namespace dinero::consensus::shielded::testing {
namespace {

Hash MakeHash(uint8_t seed) {
    Hash h{};
    h[0] = seed;
    return h;
}

// The reset must leave a fresh empty epoch: tree emptied, anchor history and
// nullifier set cleared. The cleared anchor is what makes old notes unspendable.
TEST(ShieldedEpochReset, ResetLeavesFreshEmptyEpoch) {
    CommitmentTree tree;
    AnchorHistory anchors;
    NullifierSet nullifiers;
    ASSERT_EQ(nullifiers.Open(":memory:"), NullifierSet::OpenResult::Ok);

    tree.Append(MakeHash(0x11));
    tree.Append(MakeHash(0x22));
    const Hash old_root = tree.Root();
    anchors.RecordRoot(10, old_root);
    ASSERT_TRUE(nullifiers.Insert(MakeHash(0xAA), 10)) << "nullifier insert failed";

    // Sanity: the pool is populated pre-reset.
    ASSERT_EQ(tree.Size(), 2u);
    ASSERT_NE(tree.Root(), CommitmentTree{}.Root());
    ASSERT_TRUE(anchors.Contains(old_root));
    ASSERT_TRUE(nullifiers.Contains(MakeHash(0xAA)));

    ResetShieldedEpoch(tree, anchors, nullifiers);

    EXPECT_EQ(tree.Size(), 0u);
    EXPECT_EQ(tree.Root(), CommitmentTree{}.Root());
    EXPECT_FALSE(anchors.Contains(old_root))
        << "pre-cutover anchor must be gone → old notes become unspendable";
    EXPECT_FALSE(nullifiers.Contains(MakeHash(0xAA)));
}

// Determinism: the reset result must be byte-identical to a from-scratch empty
// pool — every node must compute the same shieldedStateHash at the cutover, or
// shielded consensus splits at the boundary block.
TEST(ShieldedEpochReset, ResetIsBitIdenticalToScratchEmpty) {
    CommitmentTree tree;
    AnchorHistory anchors;
    NullifierSet nullifiers;
    ASSERT_EQ(nullifiers.Open(":memory:"), NullifierSet::OpenResult::Ok);
    tree.Append(MakeHash(1));
    tree.Append(MakeHash(2));
    tree.Append(MakeHash(3));
    anchors.RecordRoot(5, tree.Root());
    nullifiers.Insert(MakeHash(4), 5);

    ResetShieldedEpoch(tree, anchors, nullifiers);

    const CommitmentTree fresh;
    EXPECT_EQ(tree.Size(), fresh.Size());
    EXPECT_EQ(tree.Root(), fresh.Root());
}

TEST(ShieldedEpochReset, IsResetHeightMatchesOnlyTheCutover) {
    // Dormant (no reset scheduled): never a reset height, even at UINT32_MAX.
    EXPECT_FALSE(IsShieldedEpochResetHeight(0, kShieldedEpochResetDormant));
    EXPECT_FALSE(IsShieldedEpochResetHeight(61000, kShieldedEpochResetDormant));
    EXPECT_FALSE(IsShieldedEpochResetHeight(kShieldedEpochResetDormant, kShieldedEpochResetDormant));

    // Scheduled at 61000: true only at exactly 61000.
    EXPECT_FALSE(IsShieldedEpochResetHeight(60999, 61000));
    EXPECT_TRUE(IsShieldedEpochResetHeight(61000, 61000));
    EXPECT_FALSE(IsShieldedEpochResetHeight(61001, 61000));
}

TEST(ShieldedEpochReset, ParamsConsistentRequiresResetEqualsCvBinding) {
    EXPECT_TRUE(ShieldedEpochParamsConsistent(kShieldedEpochResetDormant,
                                              kShieldedEpochResetDormant));  // both dormant
    EXPECT_TRUE(ShieldedEpochParamsConsistent(61000, 61000));               // aligned
    EXPECT_FALSE(ShieldedEpochParamsConsistent(61000, 60000));              // cv != reset
    EXPECT_FALSE(ShieldedEpochParamsConsistent(61000, kShieldedEpochResetDormant));
}

// The cutover block must be a clean wall: no shielded tx allowed at exactly the
// reset height, but shielded txs are fine everywhere else (incl. H-1 and H+1).
TEST(ShieldedEpochReset, ShieldedTxWallOnlyAtCutover) {
    // Dormant network: shielded txs always allowed, even at UINT32_MAX.
    EXPECT_TRUE(ShieldedTxAllowedAtHeight(61000, kShieldedEpochResetDormant));
    EXPECT_TRUE(ShieldedTxAllowedAtHeight(kShieldedEpochResetDormant,
                                          kShieldedEpochResetDormant));

    // Scheduled at 61000: blocked at exactly 61000, allowed on both sides.
    EXPECT_TRUE(ShieldedTxAllowedAtHeight(60999, 61000));
    EXPECT_FALSE(ShieldedTxAllowedAtHeight(61000, 61000))
        << "the cutover block must be shielded-empty";
    EXPECT_TRUE(ShieldedTxAllowedAtHeight(61001, 61000))
        << "new-epoch shielded activity resumes at reset_height + 1";
}

// Reorg across the cutover: capture the full pre-reset pool, reset (destructive),
// then restore from the snapshot and assert the pool is byte-identical to before.
// This is the invertibility that lets a reorg disconnect the cutover block.
TEST(ShieldedEpochReset, CaptureRestoreRoundTripReconstructsOldEpoch) {
    CommitmentTree tree;
    AnchorHistory anchors;
    NullifierSet nullifiers;
    ASSERT_EQ(nullifiers.Open(":memory:"), NullifierSet::OpenResult::Ok);

    // Populate a non-trivial pool: several notes, several recorded roots at
    // distinct heights, several nullifiers spent at distinct heights.
    tree.Append(MakeHash(0x11));
    const Hash r1 = tree.Root();
    anchors.RecordRoot(10, r1);
    tree.Append(MakeHash(0x22));
    const Hash r2 = tree.Root();
    anchors.RecordRoot(11, r2);
    tree.Append(MakeHash(0x33));
    const Hash root_before = tree.Root();
    anchors.RecordRoot(12, root_before);
    ASSERT_TRUE(nullifiers.Insert(MakeHash(0xA1), 10));
    ASSERT_TRUE(nullifiers.Insert(MakeHash(0xB2), 11));
    ASSERT_TRUE(nullifiers.Insert(MakeHash(0xC3), 12));

    const size_t size_before = tree.Size();
    const size_t nf_count_before = nullifiers.Size();
    const auto nf_content_before = nullifiers.SerializeContent();
    const auto anchor_bytes_before = anchors.SerializeBytes();

    // Capture BEFORE the destructive reset.
    const ShieldedEpochSnapshot snap =
        CaptureShieldedEpoch(tree, anchors, nullifiers);

    ResetShieldedEpoch(tree, anchors, nullifiers);
    // Sanity: the reset really was destructive.
    ASSERT_EQ(tree.Size(), 0u);
    ASSERT_FALSE(anchors.Contains(root_before));
    ASSERT_FALSE(nullifiers.Contains(MakeHash(0xA1)));

    ASSERT_TRUE(RestoreShieldedEpoch(snap, tree, anchors, nullifiers))
        << "restore of captured pre-cutover pool must succeed";

    // Tree: same size and same root.
    EXPECT_EQ(tree.Size(), size_before);
    EXPECT_EQ(tree.Root(), root_before);
    // Anchors: every pre-reset root membership restored, byte-identical stream.
    EXPECT_TRUE(anchors.Contains(r1));
    EXPECT_TRUE(anchors.Contains(r2));
    EXPECT_TRUE(anchors.Contains(root_before));
    EXPECT_EQ(anchors.SerializeBytes(), anchor_bytes_before);
    // Nullifiers: same membership, same count, byte-identical content stream.
    EXPECT_EQ(nullifiers.Size(), nf_count_before);
    EXPECT_TRUE(nullifiers.Contains(MakeHash(0xA1)));
    EXPECT_TRUE(nullifiers.Contains(MakeHash(0xB2)));
    EXPECT_TRUE(nullifiers.Contains(MakeHash(0xC3)));
    EXPECT_EQ(nullifiers.SerializeContent(), nf_content_before);
}

// DeserializeContent must reject a corrupt payload (wrong tag) and leave the set
// empty rather than half-populated — a partial restore would silently diverge.
TEST(ShieldedEpochReset, NullifierDeserializeRejectsCorruptPayload) {
    NullifierSet nullifiers;
    ASSERT_EQ(nullifiers.Open(":memory:"), NullifierSet::OpenResult::Ok);
    ASSERT_TRUE(nullifiers.Insert(MakeHash(0xA1), 10));

    std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
    EXPECT_FALSE(nullifiers.DeserializeContent(garbage));
    EXPECT_EQ(nullifiers.Size(), 0u) << "rejected payload must leave set cleared";

    // Empty payload is a valid empty set.
    EXPECT_TRUE(nullifiers.DeserializeContent(std::vector<uint8_t>{}));
    EXPECT_EQ(nullifiers.Size(), 0u);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
