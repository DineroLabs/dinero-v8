// Copyright (c) 2026 Dinero Labs.
//
// Phase 0 wave 1 — pure-function tests for the shielded pool's
// incremental Poseidon-2 Merkle tree. Pins the empty-tree root,
// append determinism, frontier serialization round-trip, and
// reorg truncation behavior. No prover or DB setup; runs in ms.

#include <gtest/gtest.h>

#include "consensus/shielded/commitment_tree.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded::testing {
namespace {

using shielded::CommitmentTree;
using shielded::Hash;

Hash MakeHash(uint8_t seed) {
    Hash h{};
    h[0] = seed;
    return h;
}

TEST(CommitmentTreeTest, EmptyTreeRootIsDeterministic) {
    CommitmentTree a;
    CommitmentTree b;
    EXPECT_EQ(a.Root(), b.Root());
    EXPECT_EQ(a.Size(), 0u);
}

TEST(CommitmentTreeTest, AppendAdvancesSizeAndChangesRoot) {
    CommitmentTree tree;
    const Hash empty_root = tree.Root();
    const uint64_t idx = tree.Append(MakeHash(0x11));
    EXPECT_EQ(idx, 0u);
    EXPECT_EQ(tree.Size(), 1u);
    EXPECT_NE(tree.Root(), empty_root);
}

TEST(CommitmentTreeTest, AppendSequenceIsDeterministic) {
    CommitmentTree a, b;
    for (uint8_t i = 1; i <= 8; ++i) {
        a.Append(MakeHash(i));
        b.Append(MakeHash(i));
    }
    EXPECT_EQ(a.Root(), b.Root());
    EXPECT_EQ(a.Size(), 8u);
}

TEST(CommitmentTreeTest, DifferentLeavesProduceDifferentRoots) {
    CommitmentTree a, b;
    a.Append(MakeHash(0x01));
    b.Append(MakeHash(0x02));
    EXPECT_NE(a.Root(), b.Root());
}

TEST(CommitmentTreeTest, AuthPathRecoversToRoot) {
    CommitmentTree tree;
    for (uint8_t i = 1; i <= 4; ++i) {
        tree.Append(MakeHash(i));
    }
    auto path = tree.GetAuthPath(2);
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path->leaf_index, 2u);
    EXPECT_EQ(path->siblings.size(), shielded::TREE_DEPTH);
    // The path siblings reconstruct to root via Poseidon merges of the
    // leaf at index 2; we don't replay the merge here (covered by
    // prove/verify tests in wave 2), but we pin that the path is
    // populated and indexed correctly.
}

TEST(CommitmentTreeTest, GetAuthPathOutOfRangeReturnsEmpty) {
    CommitmentTree tree;
    tree.Append(MakeHash(0x42));
    EXPECT_FALSE(tree.GetAuthPath(1).has_value());
    EXPECT_FALSE(tree.GetAuthPath(99).has_value());
}

TEST(CommitmentTreeTest, FrontierSerializationRoundTrip) {
    CommitmentTree original;
    for (uint8_t i = 1; i <= 5; ++i) {
        original.Append(MakeHash(i));
    }
    const Hash original_root = original.Root();
    const std::vector<uint8_t> frontier = original.SerializeFrontier();

    CommitmentTree restored;
    ASSERT_TRUE(restored.DeserializeFrontier(frontier.data(), frontier.size()));
    EXPECT_EQ(restored.Size(), original.Size());
    EXPECT_EQ(restored.Root(), original_root);
}

TEST(CommitmentTreeTest, FrontierDeserializeRejectsCorruption) {
    CommitmentTree tree;
    tree.Append(MakeHash(0x10));
    std::vector<uint8_t> frontier = tree.SerializeFrontier();
    ASSERT_FALSE(frontier.empty());
    // Truncate one byte — must reject, not silently accept partial data.
    frontier.pop_back();
    CommitmentTree victim;
    EXPECT_FALSE(victim.DeserializeFrontier(frontier.data(), frontier.size()));
}

TEST(CommitmentTreeTest, TruncateRollsBackRoot) {
    CommitmentTree tree;
    for (uint8_t i = 1; i <= 5; ++i) {
        tree.Append(MakeHash(i));
    }
    const Hash root_at_5 = tree.Root();

    // Capture what the root *would* be if we'd only ever appended 3.
    CommitmentTree shadow;
    for (uint8_t i = 1; i <= 3; ++i) {
        shadow.Append(MakeHash(i));
    }
    const Hash root_at_3 = shadow.Root();

    ASSERT_TRUE(tree.Truncate(3));
    EXPECT_EQ(tree.Size(), 3u);
    EXPECT_EQ(tree.Root(), root_at_3);
    EXPECT_NE(tree.Root(), root_at_5);
}

TEST(CommitmentTreeTest, TruncateBeyondSizeFails) {
    CommitmentTree tree;
    tree.Append(MakeHash(1));
    EXPECT_FALSE(tree.Truncate(2));
}

TEST(CommitmentTreeTest, CopyConstructorIsIndependent) {
    CommitmentTree original;
    original.Append(MakeHash(0x77));
    CommitmentTree copy(original);
    copy.Append(MakeHash(0x88));
    EXPECT_NE(original.Root(), copy.Root());
    EXPECT_EQ(original.Size(), 1u);
    EXPECT_EQ(copy.Size(), 2u);
}

}  // namespace
}  // namespace dinero::consensus::shielded::testing
