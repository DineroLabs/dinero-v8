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

// ─────────────────────────────────────────────────────────────────────────
// DisconnectBlockShieldedSection — the disconnect twin. Round-trip tests
// pair it against ConnectBlockShieldedSection, mimicking the two call sites
// in BlockValidator::DisconnectBlock (snapshot path + legacy path both
// delegate to the same free function, so exercising the function directly
// covers both).
// ─────────────────────────────────────────────────────────────────────────

// (g) Ordinary-block round trip: connect a block with a shield bundle + a
// transfer bundle (spend + output) at height N, capturing the pre-block
// frontier the way ConnectBlockInternal does (block_validation.cpp:804-808,
// BEFORE calling the connect funnel); then disconnect with that frontier
// and no reset snapshot. Full pre-block state (tree root, nullifier count,
// anchor window) must be restored exactly.
TEST(ShieldedBlockSection, DisconnectOrdinaryBlockRestoresPreBlockStateExactly) {
    Fixture f;
    const uint32_t height_prev = kActivation + 4;
    const uint32_t height = height_prev + 1;

    // Pre-existing pool state before the block under test.
    f.tree.Append(MakeHash(0x11));
    f.anchors.RecordRoot(height_prev, f.tree.Root());
    const Hash h0_root = f.tree.Root();
    const uint64_t h0_tree_size = f.tree.Size();
    const uint64_t h0_nullifier_count = f.nullifiers.Size();
    const size_t h0_anchor_count = f.anchors.Size();
    ASSERT_TRUE(f.anchors.Contains(h0_root));

    // Mimic the connect-path capture: SerializeFrontier() BEFORE applying.
    const std::vector<uint8_t> frontier_snapshot = f.tree.SerializeFrontier();

    std::vector<ShieldedBundle> bundles{
        MakeShieldBundle(0x01, 5),
        MakeTransferBundle(0x99, 0x42),
    };
    std::vector<int64_t> deltas{5, 0};

    std::optional<ShieldedEpochSnapshot> connect_snap;
    std::string error;
    ASSERT_TRUE(ConnectBlockShieldedSection(
        bundles, deltas, height, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, connect_snap, error)) << error;

    // Sanity: the block actually changed state (otherwise the round trip
    // would trivially pass even with a broken disconnect).
    ASSERT_NE(f.tree.Root(), h0_root);
    ASSERT_EQ(f.nullifiers.Size(), h0_nullifier_count + 1);
    ASSERT_EQ(f.anchors.Size(), h0_anchor_count + 1);
    ASSERT_FALSE(connect_snap.has_value())
        << "height is not the reset height; no pre-reset snapshot expected";

    const bool ok = DisconnectBlockShieldedSection(
        height, /*pre_reset_snapshot=*/std::nullopt, frontier_snapshot,
        f.tree, f.nullifiers, &f.anchors, error);

    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(f.tree.Root(), h0_root);
    EXPECT_EQ(f.tree.Size(), h0_tree_size);
    EXPECT_EQ(f.nullifiers.Size(), h0_nullifier_count);
    EXPECT_FALSE(f.nullifiers.Contains(MakeHash(0x99)));
    EXPECT_EQ(f.anchors.Size(), h0_anchor_count);
    EXPECT_TRUE(f.anchors.Contains(h0_root));
}

// ── Byte-identical round trip (content, not counts) ───────────────────
//
// The test above pins root + sizes + a couple of Contains() probes. That is
// weaker than it looks: two anchor windows of equal SIZE but different
// MEMBERS satisfy every one of those assertions, as do two nullifier sets of
// equal count holding different nullifiers.
//
// ComputeShieldedReorgStateHash (DSR2) already defines the strong property —
// it hashes the shielded tree, the nullifier set's serialized CONTENT, and
// the anchor history's serialized bytes, precisely so equal-count-different-
// member drift cannot hide. But nothing asserted that hash is INVARIANT
// across connect-then-disconnect; the per-height journal row exists and is
// simply never compared.
//
// This fingerprint mirrors DSR2's composition over the three shielded
// containers reachable at this layer (the utreexo forest is the validator's,
// not the section's) and asserts byte-identity, which is what "reverse-apply
// is the exact inverse of apply" actually means.
std::vector<uint8_t> ShieldedStateFingerprint(const CommitmentTree& tree,
                                              const NullifierSet& nullifiers,
                                              const AnchorHistory& anchors) {
    std::vector<uint8_t> out;
    const auto frontier = tree.SerializeFrontier();
    const auto nulls    = nullifiers.SerializeContent();
    const auto anchor_b = anchors.SerializeBytes();
    out.insert(out.end(), frontier.begin(), frontier.end());
    out.insert(out.end(), nulls.begin(), nulls.end());
    out.insert(out.end(), anchor_b.begin(), anchor_b.end());
    return out;
}

TEST(ShieldedBlockSection, DisconnectRestoresByteIdenticalShieldedState) {
    Fixture f;
    const uint32_t height_prev = kActivation + 4;
    const uint32_t height = height_prev + 1;

    // Non-trivial pre-block state: a note, an anchor, and a spent nullifier,
    // so the fingerprint covers all three containers with real content.
    f.tree.Append(MakeHash(0x11));
    f.anchors.RecordRoot(height_prev, f.tree.Root());
    ASSERT_TRUE(f.nullifiers.Insert(MakeHash(0xF1), height_prev));

    const std::vector<uint8_t> before =
        ShieldedStateFingerprint(f.tree, f.nullifiers, f.anchors);
    ASSERT_FALSE(before.empty());

    const std::vector<uint8_t> frontier_snapshot = f.tree.SerializeFrontier();

    std::vector<ShieldedBundle> bundles{
        MakeShieldBundle(0x01, 5),
        MakeTransferBundle(0x99, 0x42),
    };
    std::vector<int64_t> deltas{5, 0};

    std::optional<ShieldedEpochSnapshot> connect_snap;
    std::string error;
    ASSERT_TRUE(ConnectBlockShieldedSection(
        bundles, deltas, height, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, connect_snap, error)) << error;

    // The block must actually have moved state, or the round trip is vacuous.
    const std::vector<uint8_t> after_connect =
        ShieldedStateFingerprint(f.tree, f.nullifiers, f.anchors);
    ASSERT_NE(after_connect, before)
        << "connect did not change shielded state — round trip proves nothing";

    ASSERT_TRUE(DisconnectBlockShieldedSection(
        height, /*pre_reset_snapshot=*/std::nullopt, frontier_snapshot,
        f.tree, f.nullifiers, &f.anchors, error)) << error;

    EXPECT_EQ(ShieldedStateFingerprint(f.tree, f.nullifiers, f.anchors), before)
        << "disconnect must restore shielded state BYTE-IDENTICALLY: equal "
           "counts are not enough, since equal-size/different-member anchor "
           "windows and nullifier sets would pass a counts-only check while "
           "diverging from every peer's DSR2 state hash";
}

TEST(ShieldedBlockSection, DisconnectAcrossEpochCutoverRestoresByteIdenticalState) {
    Fixture f;
    f.tree.Append(MakeHash(0x11));
    f.tree.Append(MakeHash(0x12));
    f.anchors.RecordRoot(kReset - 2, f.tree.Root());
    f.anchors.RecordRoot(kReset - 1, f.tree.Root());
    ASSERT_TRUE(f.nullifiers.Insert(MakeHash(0xAA), kReset - 1));
    ASSERT_TRUE(f.nullifiers.Insert(MakeHash(0xAB), kReset - 1));

    const std::vector<uint8_t> before =
        ShieldedStateFingerprint(f.tree, f.nullifiers, f.anchors);
    const std::vector<uint8_t> frontier_snapshot = f.tree.SerializeFrontier();

    std::optional<ShieldedEpochSnapshot> connect_snap;
    std::string error;
    ASSERT_TRUE(ConnectBlockShieldedSection(
        {}, {}, kReset, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, connect_snap, error)) << error;
    ASSERT_TRUE(connect_snap.has_value());

    // The reset wiped the pool.
    ASSERT_NE(ShieldedStateFingerprint(f.tree, f.nullifiers, f.anchors), before);

    ASSERT_TRUE(DisconnectBlockShieldedSection(
        kReset, connect_snap, frontier_snapshot,
        f.tree, f.nullifiers, &f.anchors, error)) << error;

    EXPECT_EQ(ShieldedStateFingerprint(f.tree, f.nullifiers, f.anchors), before)
        << "restoring across the epoch cutover must reproduce the pre-cutover "
           "pool byte-identically — RollbackAbove cannot undo a reset, so this "
           "is entirely the snapshot restore path";
}

// Demonstrates WHY the fingerprint is stronger, rather than asserting it.
//
// Two anchor windows of equal SIZE holding different MEMBERS, and two
// nullifier sets of equal COUNT holding different nullifiers, satisfy every
// assertion the counts-only round-trip test makes — same Size(), and its
// Contains() probes only spot-check the handful of values it happens to name.
// The fingerprint separates them, because DSR2 hashes serialized CONTENT.
//
// This is the drift class that matters in practice: it produces a different
// daemon.shieldedstatehash than every peer at the same tip while looking
// perfectly healthy to a size comparison.
TEST(ShieldedBlockSection, CountsAgreeButContentDiffersIsCaughtOnlyByFingerprint) {
    Fixture a;
    Fixture b;

    // Identical shapes, different contents.
    a.tree.Append(MakeHash(0x11));
    b.tree.Append(MakeHash(0x11));  // same note so the trees match
    a.anchors.RecordRoot(kActivation, MakeHash(0xA1));
    b.anchors.RecordRoot(kActivation, MakeHash(0xB1));  // different root
    ASSERT_TRUE(a.nullifiers.Insert(MakeHash(0xC1), kActivation));
    ASSERT_TRUE(b.nullifiers.Insert(MakeHash(0xC2), kActivation));  // different

    // What a counts-only comparison sees: no difference at all.
    EXPECT_EQ(a.tree.Size(), b.tree.Size());
    EXPECT_EQ(a.tree.Root(), b.tree.Root());
    EXPECT_EQ(a.anchors.Size(), b.anchors.Size());
    EXPECT_EQ(a.nullifiers.Size(), b.nullifiers.Size());

    // What the content fingerprint sees: two different states.
    EXPECT_NE(ShieldedStateFingerprint(a.tree, a.nullifiers, a.anchors),
              ShieldedStateFingerprint(b.tree, b.nullifiers, b.anchors))
        << "equal-count/different-member drift MUST be distinguishable — this "
           "is exactly the divergence a counts-only round-trip check cannot "
           "see, and it is what DSR2 hashes content to catch";
}

// (h) Epoch-cutover round trip: connect the reset at height H (capturing
// the pre-reset snapshot), then disconnect with that snapshot. The full
// pre-cutover pool (tree, nullifiers, anchors) must be restored, not just
// rolled back — RollbackAbove cannot undo a reset.
TEST(ShieldedBlockSection, DisconnectAcrossEpochCutoverRestoresFullPrecutoverPool) {
    Fixture f;
    f.tree.Append(MakeHash(0x11));
    const Hash pre_reset_root = f.tree.Root();
    f.anchors.RecordRoot(kReset - 1, pre_reset_root);
    ASSERT_TRUE(f.nullifiers.Insert(MakeHash(0xAA), kReset - 1));

    const uint64_t pre_reset_tree_size = f.tree.Size();
    const uint64_t pre_reset_nullifier_count = f.nullifiers.Size();
    const size_t pre_reset_anchor_count = f.anchors.Size();

    std::optional<ShieldedEpochSnapshot> connect_snap;
    std::string error;
    ASSERT_TRUE(ConnectBlockShieldedSection(
        {}, {}, kReset, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, connect_snap, error)) << error;
    ASSERT_TRUE(connect_snap.has_value());

    // Sanity: the reset actually wiped the pool.
    ASSERT_EQ(f.tree.Size(), 0u);
    ASSERT_FALSE(f.nullifiers.Contains(MakeHash(0xAA)));

    const bool ok = DisconnectBlockShieldedSection(
        kReset, connect_snap, /*pre_block_frontier=*/std::nullopt,
        f.tree, f.nullifiers, &f.anchors, error);

    ASSERT_TRUE(ok) << error;
    EXPECT_EQ(f.tree.Root(), pre_reset_root);
    EXPECT_EQ(f.tree.Size(), pre_reset_tree_size);
    EXPECT_EQ(f.nullifiers.Size(), pre_reset_nullifier_count);
    EXPECT_TRUE(f.nullifiers.Contains(MakeHash(0xAA)));
    EXPECT_EQ(f.anchors.Size(), pre_reset_anchor_count);
    EXPECT_TRUE(f.anchors.Contains(pre_reset_root));
}

// (i) Neither optional set -> no shielded activity was recorded for this
// block at connect time (e.g. shielded state was unwired, or the block
// predates activation) -> pure no-op, state untouched.
TEST(ShieldedBlockSection, DisconnectWithNeitherOptionalIsNoOp) {
    Fixture f;
    f.tree.Append(MakeHash(0x11));
    f.anchors.RecordRoot(kActivation, f.tree.Root());
    ASSERT_TRUE(f.nullifiers.Insert(MakeHash(0xAA), kActivation));

    const Hash root_before = f.tree.Root();
    const uint64_t tree_size_before = f.tree.Size();
    const uint64_t nullifier_count_before = f.nullifiers.Size();
    const size_t anchor_count_before = f.anchors.Size();

    std::string error;
    const bool ok = DisconnectBlockShieldedSection(
        kActivation + 1, /*pre_reset_snapshot=*/std::nullopt,
        /*pre_block_frontier=*/std::nullopt,
        f.tree, f.nullifiers, &f.anchors, error);

    EXPECT_TRUE(ok) << error;
    EXPECT_EQ(f.tree.Root(), root_before);
    EXPECT_EQ(f.tree.Size(), tree_size_before);
    EXPECT_EQ(f.nullifiers.Size(), nullifier_count_before);
    EXPECT_EQ(f.anchors.Size(), anchor_count_before);
}

// (j) Cutover disconnect with null anchors -> refused (mirrors the connect
// side's RejectsResetWithNullAnchors: a reset restore REQUIRES anchor state
// to write into, matching BlockValidator's existing null-container guard).
TEST(ShieldedBlockSection, DisconnectAcrossCutoverRejectsNullAnchors) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    snap.emplace();  // any non-empty optional exercises the branch

    std::string error;
    const bool ok = DisconnectBlockShieldedSection(
        kReset, snap, /*pre_block_frontier=*/std::nullopt,
        f.tree, f.nullifiers, /*anchors=*/nullptr, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "shielded epoch reset restore: missing state containers");
}

// ── Cross-tx nullifier uniqueness ─────────────────────────────────────
//
// Per-tx validation catches a nullifier repeated INSIDE one bundle, and
// catches one already in the persisted set. Neither sees the same nullifier
// appearing in two DIFFERENT transactions of the same block — that is a
// block-scope rule, and ValidateBlockShielded is the only thing enforcing it.
//
// It is the most important invariant at this layer (two txs double-spending
// one note in a single block) and had no gtest: the only assertion lived in
// the tools/pq_bench binaries. These drive it through
// ConnectBlockShieldedSection, the same funnel BlockValidator and the
// reindexer both call.

TEST(ShieldedBlockSection, CrossTxDuplicateNullifierRejected) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    // Two DISTINCT transactions spending the SAME nullifier. Each bundle is
    // individually well-formed — the duplicate only exists across them.
    std::vector<ShieldedBundle> bundles{
        MakeTransferBundle(/*nullifier_seed=*/0x77, /*commitment_seed=*/0x11),
        MakeTransferBundle(/*nullifier_seed=*/0x77, /*commitment_seed=*/0x22),
    };
    const std::vector<int64_t> deltas{0, 0};

    const bool ok = ConnectBlockShieldedSection(
        bundles, deltas, /*height=*/kActivation + 1, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error);

    EXPECT_FALSE(ok)
        << "two transactions in one block spending the same nullifier is a "
           "double-spend and must be rejected at block scope";
    EXPECT_NE(error.find("shielded-block-validation-failed"), std::string::npos)
        << error;

    // And nothing may have been applied.
    EXPECT_EQ(f.tree.Size(), 0u);
    EXPECT_EQ(f.nullifiers.Size(), 0u);
}

TEST(ShieldedBlockSection, NullifierAlreadyInPreBlockSetRejected) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    // Already spent in an earlier block.
    ASSERT_TRUE(f.nullifiers.Insert(MakeHash(0x88), kActivation));

    std::vector<ShieldedBundle> bundles{
        MakeTransferBundle(/*nullifier_seed=*/0x88, /*commitment_seed=*/0x33)};
    const std::vector<int64_t> deltas{0};

    const bool ok = ConnectBlockShieldedSection(
        bundles, deltas, /*height=*/kActivation + 1, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error);

    EXPECT_FALSE(ok) << "re-spending a nullifier already in the set is a "
                        "double-spend and must be rejected";
    EXPECT_EQ(f.tree.Size(), 0u);
}

// Control: the same shape WITHOUT the collision must connect, so the two
// tests above cannot pass for an unrelated reason (a malformed fixture, the
// activation gate, or the conservation check firing instead).
TEST(ShieldedBlockSection, DistinctNullifiersInSeparateTxsConnect) {
    Fixture f;
    std::optional<ShieldedEpochSnapshot> snap;
    std::string error;

    std::vector<ShieldedBundle> bundles{
        MakeTransferBundle(/*nullifier_seed=*/0x77, /*commitment_seed=*/0x11),
        MakeTransferBundle(/*nullifier_seed=*/0x78, /*commitment_seed=*/0x22),
    };
    const std::vector<int64_t> deltas{0, 0};

    const bool ok = ConnectBlockShieldedSection(
        bundles, deltas, /*height=*/kActivation + 1, kReset, kActivation,
        f.tree, f.nullifiers, &f.anchors, snap, error);

    EXPECT_TRUE(ok) << error;
    EXPECT_EQ(f.tree.Size(), 2u);
    EXPECT_EQ(f.nullifiers.Size(), 2u);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
