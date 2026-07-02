// Copyright (c) 2026 Dinero Labs.
//
// Test-first: ConnectBlockShieldedSection is the single shared connect-tail
// implementation (epoch-reset gate -> ValidateBlockShielded ->
// ApplyBlockShielded -> RecordRoot). These tests exercise it directly,
// standing in for the two call sites (BlockValidator, reindexer) that a
// later task delegates to this function. Throwing/exit-nonzero checks only
// (gtest EXPECT/ASSERT) — never bare assert(), which is a no-op under
// NDEBUG in this Release build.
#include <gtest/gtest.h>

#include "consensus/shielded/anchor_history.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/nullifier_set.h"
#include "consensus/shielded/shielded_block_section.h"
#include "consensus/shielded/shielded_epoch.h"
#include "consensus/shielded/shielded_tx.h"

namespace dinero::consensus::shielded::testing {
namespace {

constexpr uint32_t kActivation = 100;
constexpr uint32_t kReset = 200;

Hash MakeHash(uint8_t seed) {
    Hash h{};
    h[0] = seed;
    return h;
}

// A minimal shield-only bundle: one output, no spends. ConnectBlockShielded
// Section's ValidateBlockShielded/ApplyBlockShielded only check block-level
// invariants (nullifier uniqueness, value-balance conservation) and append
// commitments/nullifiers — they do not verify zk proofs (that is per-tx
// validation, owned by callers per the header contract), so a bundle with
// no proof bytes is sufficient here.
ShieldedBundle MakeShieldBundle(uint8_t commitment_seed, int64_t value) {
    ShieldedBundle bundle;
    ShieldedOutput output;
    output.commitment = MakeHash(commitment_seed);
    bundle.outputs.push_back(output);
    bundle.value_balance = value;
    return bundle;
}

// A shielded TRANSFER bundle: one spend (nullifier) + one output
// (commitment), net value_balance 0. Exercises both halves of
// ApplyBlockShielded (commitment append AND nullifier insert) in one call.
ShieldedBundle MakeTransferBundle(uint8_t nullifier_seed, uint8_t commitment_seed) {
    ShieldedBundle bundle;
    ShieldedSpend spend;
    spend.nullifier = MakeHash(nullifier_seed);
    bundle.spends.push_back(spend);
    ShieldedOutput output;
    output.commitment = MakeHash(commitment_seed);
    bundle.outputs.push_back(output);
    bundle.value_balance = 0;
    return bundle;
}

struct Fixture {
    CommitmentTree tree;
    NullifierSet nullifiers;
    AnchorHistory anchors;

    Fixture() {
        EXPECT_EQ(nullifiers.Open(":memory:"), NullifierSet::OpenResult::Ok);
    }
};

// (a) Empty block below activation height -> no-op, returns true, no anchor
// recorded (RecordRoot is gated on height >= activation_height).
TEST(ShieldedBlockSection, EmptyBlockBelowActivationIsNoOp) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    const bool ok = ConnectBlockShieldedSection(
        {}, {}, /*height=*/kActivation - 1, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error);

    EXPECT_TRUE(ok) << error;
    EXPECT_EQ(f.anchors.Size(), 0u);
    EXPECT_FALSE(snap.has_value());
    EXPECT_EQ(f.tree.Size(), 0u);
}

// (b) Block at/after activation with no bundles -> RecordRoot recorded
// exactly once for that height (idempotent re-call at the same height keeps
// the window size at 1, matching AnchorHistory::RecordRoot's documented
// overwrite-on-repeat-height behavior).
TEST(ShieldedBlockSection, EmptyBlockAtActivationRecordsRootOnce) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    ASSERT_TRUE(ConnectBlockShieldedSection(
        {}, {}, kActivation, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error)) << error;

    EXPECT_EQ(f.anchors.Size(), 1u);
    EXPECT_TRUE(f.anchors.Contains(f.tree.Root()));

    // Re-running at the SAME height must overwrite, not append a second entry.
    ASSERT_TRUE(ConnectBlockShieldedSection(
        {}, {}, kActivation, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error)) << error;
    EXPECT_EQ(f.anchors.Size(), 1u);
}

// (c) Reset height + non-empty bundles -> rejected by the wall rule.
TEST(ShieldedBlockSection, RejectsShieldedTxAtResetHeight) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    std::vector<ShieldedBundle> bundles{MakeShieldBundle(0x01, 5)};
    std::vector<int64_t> deltas{5};

    const bool ok = ConnectBlockShieldedSection(
        bundles, deltas, kReset, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "shielded-tx-at-epoch-reset-height");
    EXPECT_FALSE(snap.has_value());
}

// (d) Reset height + empty bundles -> pool captured then reset; the caller's
// snapshot out-param is populated with the pre-reset pool.
TEST(ShieldedBlockSection, ResetHeightWithEmptyBlockCapturesAndResetsPool) {
    Fixture f;
    f.tree.Append(MakeHash(0x11));
    const Hash old_root = f.tree.Root();
    f.anchors.RecordRoot(kReset - 1, old_root);
    ASSERT_TRUE(f.nullifiers.Insert(MakeHash(0xAA), kReset - 1));

    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    const bool ok = ConnectBlockShieldedSection(
        {}, {}, kReset, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error);

    EXPECT_TRUE(ok) << error;
    ASSERT_TRUE(snap.has_value());
    EXPECT_FALSE(snap->tree_frontier.empty());
    EXPECT_FALSE(snap->nullifiers.empty());

    // Pool is now a fresh empty epoch — old root/nullifier gone.
    EXPECT_EQ(f.tree.Size(), 0u);
    EXPECT_EQ(f.tree.Root(), CommitmentTree{}.Root());
    EXPECT_FALSE(f.nullifiers.Contains(MakeHash(0xAA)));

    // kReset >= kActivation here, so the post-reset (empty) root is also
    // recorded for this height.
    EXPECT_TRUE(f.anchors.Contains(f.tree.Root()));
}

// (e) Reset height + null anchors -> refused (matches BlockValidator's
// existing behavior: a reset REQUIRES anchor state to gate on).
TEST(ShieldedBlockSection, RejectsResetWithNullAnchors) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    const bool ok = ConnectBlockShieldedSection(
        {}, {}, kReset, kReset, kActivation,
        f.tree, f.nullifiers, /*anchors=*/nullptr, snap, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "shielded-epoch-reset-missing-anchor-state");
    EXPECT_FALSE(snap.has_value());
}

// (f) Apply path: one shield bundle + matching delta -> tree grows,
// nullifiers reflect any spends, and RecordRoot captures the POST-apply
// root (not the pre-apply root).
TEST(ShieldedBlockSection, AppliesShieldBundleAndRecordsPostApplyRoot) {
    Fixture f;
    const uint32_t height = kActivation + 10;
    const Hash spend_nullifier = MakeHash(0x99);

    std::vector<ShieldedBundle> bundles{MakeTransferBundle(0x99, 0x42)};
    std::vector<int64_t> deltas{0};

    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    const bool ok = ConnectBlockShieldedSection(
        bundles, deltas, height, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error);

    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(f.tree.Size(), 1u);
    EXPECT_NE(f.tree.Root(), CommitmentTree{}.Root())
        << "tree must have grown past the empty-tree root";
    EXPECT_TRUE(f.nullifiers.Contains(spend_nullifier))
        << "the bundle's spend nullifier must be inserted";
    // The recorded anchor must be the POST-apply root, i.e. it must contain
    // the tree's root AFTER the bundle's output was appended.
    EXPECT_TRUE(f.anchors.Contains(f.tree.Root()));
    EXPECT_EQ(f.anchors.Size(), 1u);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
