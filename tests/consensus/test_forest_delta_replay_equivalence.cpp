// Forest checkpoint delta campaign — phase 2 equivalence suite
// (docs/design/forest-checkpoint-deltas.md, test plan item 1).
//
// THE campaign invariant: forest(tip) == checkpoint(K) + deltas(K+1..tip).
// Random block sequences applied (a) continuously and (b) via
// checkpoint-every-N + ApplyUtreexoDeltaForward replay must yield
// byte-identical serialize() output and identical findLeafPosition/prove
// behavior for every live leaf. Blocks are applied in the production
// order — the validator's two-pass shape (ConnectBlockInternal PASS 1:
// remove all spends in order, PASS 2: add all outputs in order) — which
// is exactly the order the UD:<blockhash> sidecar records.

#include "consensus/utreexo_delta_codec.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_delta.h"

using dinero::ApplyUtreexoDeltaForward;
using dinero::consensus::UtreexoDelta;
using dinero::consensus::UtreexoForest;
using dinero::consensus::UtreexoHash;

namespace {

UtreexoHash MakeLeaf(uint64_t ordinal) {
    UtreexoHash h(32);
    for (size_t i = 0; i < 32; ++i) {
        h[i] = static_cast<uint8_t>((ordinal >> ((i % 8) * 8)) ^ (0xA5 + i));
    }
    // Ensure global uniqueness even where the pattern above collides.
    h[24] = static_cast<uint8_t>(ordinal >> 0);
    h[25] = static_cast<uint8_t>(ordinal >> 8);
    h[26] = static_cast<uint8_t>(ordinal >> 16);
    h[27] = static_cast<uint8_t>(ordinal >> 24);
    return h;
}

struct SimChain {
    UtreexoForest forest;
    std::vector<UtreexoHash> live_leaves;
    uint64_t next_leaf_ordinal = 0;
    std::mt19937_64 rng;

    explicit SimChain(uint64_t seed) : rng(seed) {}

    // Apply one synthetic block in the production two-pass order and
    // return the delta record exactly as the validator would build it.
    UtreexoDelta ApplyBlock(size_t deletes, size_t adds) {
        UtreexoDelta delta;
        delta.numLeavesBefore = forest.getNumLeaves();

        // PASS 1: remove spends (order matters; recorded in that order).
        deletes = std::min(deletes, live_leaves.size());
        for (size_t i = 0; i < deletes; ++i) {
            std::uniform_int_distribution<size_t> pick(0, live_leaves.size() - 1);
            const size_t victim = pick(rng);
            const UtreexoHash leaf = live_leaves[victim];
            live_leaves.erase(live_leaves.begin() +
                              static_cast<std::ptrdiff_t>(victim));

            const auto position = forest.findLeafPosition(leaf);
            EXPECT_TRUE(position.has_value());
            EXPECT_TRUE(forest.removeAtKnownPosition(*position, leaf));
            delta.recordDelete(*position, leaf);
        }

        // PASS 2: add new outputs.
        for (size_t i = 0; i < adds; ++i) {
            const UtreexoHash leaf = MakeLeaf(next_leaf_ordinal++);
            const uint64_t position = forest.add(leaf);
            EXPECT_NE(position, UINT64_MAX);
            delta.recordAdd(leaf, position);
            live_leaves.push_back(leaf);
        }

        return delta;
    }
};

void ExpectForestsEquivalent(const UtreexoForest& a, const UtreexoForest& b,
                             const std::vector<UtreexoHash>& live_leaves) {
    // Byte-identical persisted form — the strongest equivalence.
    EXPECT_EQ(a.serialize(), b.serialize());
    EXPECT_EQ(a.getCommitment(), b.getCommitment());
    EXPECT_EQ(a.getNumLeaves(), b.getNumLeaves());

    // Every live leaf must be provable identically in both forests.
    for (const auto& leaf : live_leaves) {
        const auto pos_a = a.findLeafPosition(leaf);
        const auto pos_b = b.findLeafPosition(leaf);
        ASSERT_TRUE(pos_a.has_value());
        ASSERT_TRUE(pos_b.has_value());
        EXPECT_EQ(*pos_a, *pos_b);

        const auto proof_a = a.prove(*pos_a);
        const auto proof_b = b.prove(*pos_b);
        ASSERT_TRUE(proof_a.has_value());
        ASSERT_TRUE(proof_b.has_value());
        EXPECT_EQ(proof_a->serialize(), proof_b->serialize());
    }
}

// Drive `blocks` synthetic blocks; checkpoint every `interval`; replay from
// the last checkpoint and compare against the continuous forest.
void RunEquivalenceScenario(uint64_t seed, size_t blocks, size_t interval,
                            size_t max_deletes, size_t max_adds,
                            std::optional<size_t> canonical_flip_block) {
    SimChain chain(seed);
    if (!canonical_flip_block.has_value()) {
        // Most mainnet history runs with canonical roots on from genesis
        // of the scenario.
        chain.forest.setCanonicalEmptyRoots(true);
    }

    std::vector<uint8_t> checkpoint;
    size_t checkpoint_block = 0;  // number of blocks applied at checkpoint
    bool checkpoint_canonical = chain.forest.isCanonicalEmptyRoots();
    std::vector<UtreexoDelta> deltas_since_checkpoint;
    // Continuous forest's serialized form after every block, for per-block
    // divergence localization during replay.
    std::vector<std::vector<uint8_t>> serialized_after_block(blocks + 1);

    std::mt19937_64 shape_rng(seed ^ 0x5EED);
    for (size_t b = 1; b <= blocks; ++b) {
        if (canonical_flip_block.has_value() && b == *canonical_flip_block &&
            !chain.forest.isCanonicalEmptyRoots()) {
            // Mirror ConnectBlockInternal's fork activation: flip the flag
            // and re-canonicalize BEFORE applying the activation block.
            chain.forest.setCanonicalEmptyRoots(true);
            chain.forest.rebuildRoots();
        }

        // Pre-fork (flag-off) checkpoints containing deletions may not
        // deserialize at all — the legacy ghost-slot ambiguity is the very
        // reason the canonical-roots fork exists, and it constrains today's
        // production restore identically. Boundary-crossing scenarios
        // therefore keep the pre-flip history add-only so the legacy
        // checkpoint is deserializable; deletions resume from the flip.
        const bool deletes_allowed =
            !canonical_flip_block.has_value() || b >= *canonical_flip_block;
        std::uniform_int_distribution<size_t> del_dist(
            0, deletes_allowed ? max_deletes : 0);
        std::uniform_int_distribution<size_t> add_dist(1, max_adds);
        deltas_since_checkpoint.push_back(
            chain.ApplyBlock(del_dist(shape_rng), add_dist(shape_rng)));
        serialized_after_block[b] = chain.forest.serialize();

        if (b % interval == 0) {
            checkpoint = chain.forest.serialize();
            checkpoint_block = b;
            checkpoint_canonical = chain.forest.isCanonicalEmptyRoots();
            deltas_since_checkpoint.clear();
        }
    }
    ASSERT_FALSE(checkpoint.empty()) << "scenario never crossed a checkpoint";

    // Restore-side: latest checkpoint + forward delta replay.
    UtreexoForest replayed = UtreexoForest::deserialize(checkpoint);
    if (checkpoint_canonical) {
        // Restore path sets the flag from the checkpoint height without
        // rebuilding (chainstate_service Phase 2.1 restore).
        replayed.setCanonicalEmptyRoots(true);
    }
    for (size_t i = 0; i < deltas_since_checkpoint.size(); ++i) {
        const size_t replay_block = checkpoint_block + 1 + i;
        if (canonical_flip_block.has_value() &&
            replay_block == *canonical_flip_block &&
            !replayed.isCanonicalEmptyRoots()) {
            replayed.setCanonicalEmptyRoots(true);
            replayed.rebuildRoots();
        }
        std::string error;
        ASSERT_TRUE(ApplyUtreexoDeltaForward(replayed,
                                             deltas_since_checkpoint[i], error))
            << "replay failed at block " << replay_block << ": " << error;
        const auto& delta = deltas_since_checkpoint[i];
        ASSERT_EQ(replayed.serialize(), serialized_after_block[replay_block])
            << "first byte divergence at replayed block " << replay_block
            << " (deletes=" << delta.deletedLeaves.size()
            << " adds=" << delta.addedLeaves.size() << ")";
    }

    ExpectForestsEquivalent(chain.forest, replayed, chain.live_leaves);
}

// Control: does serialize→deserialize→serialize round-trip byte-identically
// at all (no replay involved)? This bounds what the replay equivalence can
// promise: if plain restore already re-serializes differently, byte-identity
// across a restore is not a property of the CODEC, and the meaningful
// invariants are commitment + positions + proofs + identical future
// evolution.
TEST(ForestDeltaReplayEquivalence, DeserializeRoundTripControl) {
    SimChain chain(4242);
    chain.forest.setCanonicalEmptyRoots(true);
    chain.ApplyBlock(0, 12);
    chain.ApplyBlock(4, 6);

    const auto s1 = chain.forest.serialize();
    UtreexoForest restored = UtreexoForest::deserialize(s1);
    restored.setCanonicalEmptyRoots(true);
    const auto s2 = restored.serialize();

    EXPECT_EQ(s1.size(), s2.size()) << "roundtrip changes serialized size";
    EXPECT_EQ(s1, s2) << "roundtrip changes serialized bytes";

    // Regardless of byte drift, both must evolve identically from here.
    const UtreexoHash extra = MakeLeaf(999999);
    const uint64_t pa = chain.forest.add(extra);
    const uint64_t pb = restored.add(extra);
    EXPECT_EQ(pa, pb);
    EXPECT_EQ(chain.forest.getCommitment(), restored.getCommitment());

    // Diagnostic pin: does a POST-restore op serialize the same as the
    // never-restored forest? (This is the status quo for every node that
    // has ever restarted — today's per-block-checkpoint restore also does
    // ops on a deserialized forest.)
    const auto post_cont = chain.forest.serialize();
    const auto post_rest = restored.serialize();
    EXPECT_EQ(post_cont.size(), post_rest.size()) << "post-restore op size drift";
    EXPECT_EQ(post_cont == post_rest, true) << "post-restore op byte drift";
}

TEST(ForestDeltaReplayEquivalence, RandomMixedSequencesOverSeeds) {
    for (uint64_t seed = 1; seed <= 8; ++seed) {
        SCOPED_TRACE("seed=" + std::to_string(seed));
        RunEquivalenceScenario(seed, /*blocks=*/41, /*interval=*/7,
                               /*max_deletes=*/4, /*max_adds=*/6,
                               /*canonical_flip_block=*/std::nullopt);
    }
}

TEST(ForestDeltaReplayEquivalence, DeleteHeavySequences) {
    for (uint64_t seed = 100; seed <= 104; ++seed) {
        SCOPED_TRACE("seed=" + std::to_string(seed));
        RunEquivalenceScenario(seed, /*blocks=*/30, /*interval=*/5,
                               /*max_deletes=*/9, /*max_adds=*/3,
                               /*canonical_flip_block=*/std::nullopt);
    }
}

TEST(ForestDeltaReplayEquivalence, ReplayCrossesCanonicalRootsBoundary) {
    // Checkpoint lands at block 10 (flag off); the flip happens at block 13,
    // INSIDE the replayed range — replay must re-canonicalize exactly like
    // live connect did.
    RunEquivalenceScenario(/*seed=*/777, /*blocks=*/15, /*interval=*/10,
                           /*max_deletes=*/3, /*max_adds=*/5,
                           /*canonical_flip_block=*/13);
}

TEST(ForestDeltaReplayEquivalence, CheckpointAfterBoundaryReplaysCanonical) {
    // Flip at block 4, checkpoint at 6, replay 7..11 fully canonical.
    RunEquivalenceScenario(/*seed=*/778, /*blocks=*/11, /*interval=*/6,
                           /*max_deletes=*/3, /*max_adds=*/5,
                           /*canonical_flip_block=*/4);
}

TEST(ForestDeltaReplayEquivalence, ReplayRefusesWrongPreState) {
    SimChain chain(9);
    chain.forest.setCanonicalEmptyRoots(true);
    chain.ApplyBlock(0, 5);
    UtreexoDelta next = chain.ApplyBlock(2, 3);

    // Tamper: claim the block applied on a different-size forest.
    next.numLeavesBefore += 1;
    UtreexoForest copy = UtreexoForest::deserialize(chain.forest.serialize());
    copy.setCanonicalEmptyRoots(true);
    std::string error;
    EXPECT_FALSE(ApplyUtreexoDeltaForward(copy, next, error));
    EXPECT_FALSE(error.empty());
}

TEST(ForestDeltaReplayEquivalence, ReplayRefusesPositionDrift) {
    SimChain chain(10);
    chain.forest.setCanonicalEmptyRoots(true);
    chain.ApplyBlock(0, 6);
    const std::vector<uint8_t> checkpoint = chain.forest.serialize();
    UtreexoDelta delta = chain.ApplyBlock(1, 2);

    // Tamper with a recorded ADD position: replay's cross-check must
    // catch the drift instead of silently building a divergent forest.
    ASSERT_FALSE(delta.addedLeaves.empty());
    delta.addedLeaves.back().position += 1;

    UtreexoForest replayed = UtreexoForest::deserialize(checkpoint);
    replayed.setCanonicalEmptyRoots(true);
    std::string error;
    EXPECT_FALSE(ApplyUtreexoDeltaForward(replayed, delta, error));
    EXPECT_FALSE(error.empty());
}

}  // namespace
