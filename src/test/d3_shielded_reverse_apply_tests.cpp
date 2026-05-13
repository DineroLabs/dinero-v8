// Copyright (c) 2026 Dinero Labs.
//
// D.3-shielded reverse-apply property test.
//
// The "sleep at night" proof for shielded undo regeneration.
// ChainstateService::RegenerateUndoFromBlock reconstructs
// pre_block_shielded_frontier by:
//
//   1. cloning the live commitment tree (post-block state)
//   2. calling Truncate(live_size - N) where N = number of shielded
//      outputs in the block being disconnected
//   3. serializing the truncated frontier
//
// The atomicity invariant this test pins: the serialized frontier
// after that procedure is BYTE-IDENTICAL to the serialized frontier
// of an alternate-history tree that never had those N leaves
// appended. If it isn't, DisconnectBlock applies a wrong frontier
// and consensus state diverges.
//
// Tests are pure: no chain setup, no I/O. Pin the deterministic
// behavior of the math at the level it's executed in production.

#include <gtest/gtest.h>

#include "consensus/shielded/commitment_tree.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::CommitmentTree;
using shielded::Hash;

Hash MakeLeaf(uint32_t seed) {
    Hash h{};
    // Deterministic spread across the leaf so trees with adjacent
    // seeds produce distinct commitments at the byte level.
    h[0]  = static_cast<uint8_t>(seed & 0xFF);
    h[1]  = static_cast<uint8_t>((seed >> 8) & 0xFF);
    h[2]  = static_cast<uint8_t>((seed >> 16) & 0xFF);
    h[3]  = static_cast<uint8_t>((seed >> 24) & 0xFF);
    h[31] = 0xC7;
    return h;
}

// The core invariant: appending leaves [0, M) to one tree, then
// cloning + Truncate(M - N) on the clone, must produce the
// byte-identical SerializeFrontier output as appending only [0, M-N)
// to a separate tree from scratch.
//
// This is the operation D.3-shielded performs against the live
// CommitmentTree during DisconnectTip when ReadStoredUndo fails for
// the active tip — it recovers pre_block_shielded_frontier with no
// access to a prior snapshot.
void AssertReverseApplyRoundTrip(uint32_t total_M, uint32_t block_N) {
    ASSERT_LE(block_N, total_M) << "block_N must be ≤ total_M";

    // Build the post-block tree: append every leaf [0, M).
    CommitmentTree post_block;
    for (uint32_t i = 0; i < total_M; ++i) {
        post_block.Append(MakeLeaf(i));
    }
    ASSERT_EQ(post_block.Size(), total_M);

    // D.3-shielded path: clone, truncate by block_N, serialize.
    CommitmentTree truncated_clone(post_block);
    ASSERT_TRUE(truncated_clone.Truncate(total_M - block_N));
    ASSERT_EQ(truncated_clone.Size(), total_M - block_N);
    const auto reverse_applied_frontier = truncated_clone.SerializeFrontier();

    // Reference path: build a separate tree by appending only [0, M-N).
    CommitmentTree pre_block_reference;
    for (uint32_t i = 0; i < total_M - block_N; ++i) {
        pre_block_reference.Append(MakeLeaf(i));
    }
    ASSERT_EQ(pre_block_reference.Size(), total_M - block_N);
    const auto reference_frontier = pre_block_reference.SerializeFrontier();

    EXPECT_EQ(reverse_applied_frontier, reference_frontier)
        << "Reverse-apply frontier diverges from reference at total_M=" << total_M
        << " block_N=" << block_N;

    // Also assert the live tree was NOT mutated by the clone+truncate.
    EXPECT_EQ(post_block.Size(), total_M)
        << "live tree mutated by D.3-shielded clone+truncate";
}

TEST(D3ShieldedReverseApply, SingleOutputBlock) {
    AssertReverseApplyRoundTrip(/*total_M=*/100, /*block_N=*/1);
}

TEST(D3ShieldedReverseApply, MultipleOutputsSameBlock) {
    // Typical shielded tx: 1-3 outputs. Multi-tx block: 4-10 outputs.
    AssertReverseApplyRoundTrip(/*total_M=*/100, /*block_N=*/4);
    AssertReverseApplyRoundTrip(/*total_M=*/100, /*block_N=*/10);
}

TEST(D3ShieldedReverseApply, BlockAtRightEdge) {
    // Truncate every leaf in the post-block tree (block was first
    // block ever, M == N). Edge of "no prior shielded state."
    AssertReverseApplyRoundTrip(/*total_M=*/8, /*block_N=*/8);
}

TEST(D3ShieldedReverseApply, BlockAtSubtreeBoundary) {
    // Truncate boundaries that cross internal subtree levels exercise
    // the right-edge frontier reconstruction more thoroughly:
    // size-7 → size-3 (crosses 4-leaf subtree boundary)
    // size-15 → size-7 (crosses 8-leaf subtree boundary)
    // size-31 → size-15 (crosses 16-leaf subtree boundary)
    AssertReverseApplyRoundTrip(/*total_M=*/7, /*block_N=*/4);
    AssertReverseApplyRoundTrip(/*total_M=*/15, /*block_N=*/8);
    AssertReverseApplyRoundTrip(/*total_M=*/31, /*block_N=*/16);
}

TEST(D3ShieldedReverseApply, LargeTreeSparseBlock) {
    // Realistic mainnet shape: long chain with a small reorg block.
    AssertReverseApplyRoundTrip(/*total_M=*/4096, /*block_N=*/3);
}

TEST(D3ShieldedReverseApply, ZeroOutputBlock) {
    // A "shielded" block with no outputs (only spends): D.3 still
    // computes shielded_outputs_in_block = 0, and the field stays
    // unset. We simulate by truncating by zero — frontier must
    // be byte-identical to the un-cloned live tree.
    CommitmentTree post_block;
    for (uint32_t i = 0; i < 50; ++i) {
        post_block.Append(MakeLeaf(i));
    }
    const auto live_frontier = post_block.SerializeFrontier();

    CommitmentTree truncated_clone(post_block);
    ASSERT_TRUE(truncated_clone.Truncate(50));  // no-op truncation
    EXPECT_EQ(truncated_clone.SerializeFrontier(), live_frontier);
}

TEST(D3ShieldedReverseApply, RootMatchAcrossPaths) {
    // Stronger property: not just frontier bytes, but the Merkle ROOT
    // of the truncated tree matches the reference. Roots are what
    // future spends' anchors will reference, so root-equivalence is
    // the consensus-level invariant.
    constexpr uint32_t kTotal = 200;
    constexpr uint32_t kBlock = 7;

    CommitmentTree post_block;
    for (uint32_t i = 0; i < kTotal; ++i) {
        post_block.Append(MakeLeaf(i));
    }
    CommitmentTree truncated_clone(post_block);
    ASSERT_TRUE(truncated_clone.Truncate(kTotal - kBlock));

    CommitmentTree pre_block_reference;
    for (uint32_t i = 0; i < kTotal - kBlock; ++i) {
        pre_block_reference.Append(MakeLeaf(i));
    }
    EXPECT_EQ(truncated_clone.Root(), pre_block_reference.Root());
}

TEST(D3ShieldedReverseApply, FrontierDeserializeRoundTrip) {
    // After D.3-shielded writes pre_block_shielded_frontier into the
    // UndoRecord and DisconnectBlock applies it, the live tree gets
    // its frontier deserialized back. Round-trip must preserve root.
    CommitmentTree post_block;
    for (uint32_t i = 0; i < 64; ++i) {
        post_block.Append(MakeLeaf(i));
    }
    CommitmentTree truncated_clone(post_block);
    ASSERT_TRUE(truncated_clone.Truncate(60));
    const auto serialized = truncated_clone.SerializeFrontier();

    CommitmentTree restored;
    ASSERT_TRUE(restored.DeserializeFrontier(serialized.data(), serialized.size()));
    EXPECT_EQ(restored.Size(), 60u);
    EXPECT_EQ(restored.Root(), truncated_clone.Root());
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
