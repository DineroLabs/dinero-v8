/**
 * @file test_proof_generation_correctness.cpp
 * @brief Phase U.1: Utreexo Proof Generation Correctness Tests
 *
 * CONSENSUS CRITICAL - These tests validate that Utreexo proofs are:
 *   - Structurally valid (correct sibling count, no duplicates)
 *   - Minimal (no unnecessary siblings)
 *   - Deterministic (same forest + leaf → same proof)
 *   - Round-trip correct (generate → verify succeeds)
 *   - Rejection-safe (modified proofs fail verification)
 *
 * Hard Invariants Tested:
 *   INV-1: Proof siblings count == forest height for that leaf
 *   INV-2: No duplicate siblings in proof
 *   INV-3: Deterministic sibling ordering
 *   INV-4: Root recomputation from proof == forest.root()
 *   INV-5: Proof fails if any bit is flipped
 *
 * If any test fails → CONSENSUS BUG → Network fork risk!
 */

#include <gtest/gtest.h>
#include "consensus/utreexo_accumulator.h"
#include "primitives/uint256.h"
#include <unordered_set>
#include <algorithm>

using namespace dinero;
using namespace dinero::consensus;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// Create deterministic test hash from integer
UtreexoHash makeTestHash(uint64_t value) {
    UtreexoHash hash(32, 0);
    for (int i = 0; i < 8; i++) {
        hash[i] = (value >> (i * 8)) & 0xFF;
    }
    // Add some entropy to rest of hash
    for (int i = 8; i < 32; i++) {
        hash[i] = static_cast<uint8_t>((value * 31 + i * 17) % 256);
    }
    return hash;
}

// Create UTXO leaf hash (realistic format)
UtreexoHash makeUTXOLeaf(uint64_t utxo_id, uint64_t value) {
    uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &utxo_id, sizeof(utxo_id));

    std::vector<uint8_t> scriptPubKey = {0x76, 0xa9, 0x14}; // P2PKH prefix
    for (int i = 0; i < 20; i++) {
        scriptPubKey.push_back(static_cast<uint8_t>((utxo_id >> (i % 8)) & 0xFF));
    }
    scriptPubKey.push_back(0x88);
    scriptPubKey.push_back(0xac);

    return HashUTXO(txid, 0, value, scriptPubKey);
}

// Count unique hashes
size_t countUnique(const std::vector<UtreexoHash>& hashes) {
    std::unordered_set<std::string> seen;
    for (const auto& h : hashes) {
        seen.insert(std::string(h.begin(), h.end()));
    }
    return seen.size();
}

// Calculate expected tree height for N leaves at position pos
uint8_t expectedTreeHeight(uint64_t numLeaves, uint64_t pos) {
    // Find which tree this position belongs to
    uint64_t offset = 0;
    for (int height = 63; height >= 0; height--) {
        uint64_t treeSize = 1ULL << height;
        if (numLeaves & treeSize) {
            if (pos < offset + treeSize) {
                return static_cast<uint8_t>(height);
            }
            offset += treeSize;
        }
    }
    return 0;
}

void removeLeafOrAssert(UtreexoForest& forest, const UtreexoHash& leaf) {
    auto position = forest.findLeafPosition(leaf);
    ASSERT_TRUE(position.has_value());

    auto proof = forest.prove(*position);
    ASSERT_TRUE(proof.has_value());
    ASSERT_TRUE(forest.remove(leaf, *proof));
}

void expectEquivalentForestState(
    UtreexoForest& live,
    UtreexoForest& restored,
    const std::vector<UtreexoHash>& activeTargets
) {
    EXPECT_EQ(restored.getNumLeaves(), live.getNumLeaves());
    EXPECT_EQ(restored.getActiveLeaves(), live.getActiveLeaves());
    EXPECT_EQ(restored.getCommitment(), live.getCommitment());

    for (const auto& leaf : activeTargets) {
        auto livePos = live.findLeafPosition(leaf);
        auto restoredPos = restored.findLeafPosition(leaf);
        ASSERT_TRUE(livePos.has_value());
        ASSERT_TRUE(restoredPos.has_value());
        EXPECT_EQ(*restoredPos, *livePos);

        auto liveProof = live.prove(*livePos);
        auto restoredProof = restored.prove(*restoredPos);
        ASSERT_TRUE(liveProof.has_value());
        ASSERT_TRUE(restoredProof.has_value());

        EXPECT_EQ(restoredProof->siblings, liveProof->siblings);
        EXPECT_EQ(restoredProof->position, liveProof->position);
        EXPECT_EQ(restoredProof->numLeaves, liveProof->numLeaves);
        EXPECT_TRUE(restoredProof->verify(leaf, restored.getRoots()));
        EXPECT_TRUE(liveProof->verify(leaf, live.getRoots()));
    }

    auto liveBlockProof = live.generateBlockProof(activeTargets);
    auto restoredBlockProof = restored.generateBlockProof(activeTargets);

    EXPECT_EQ(restoredBlockProof.targets, liveBlockProof.targets);
    EXPECT_EQ(restoredBlockProof.positions, liveBlockProof.positions);
    EXPECT_EQ(restoredBlockProof.proof_hashes, liveBlockProof.proof_hashes);
    EXPECT_EQ(restoredBlockProof.numLeaves, liveBlockProof.numLeaves);

    EXPECT_TRUE(restored.verifyBatchProofStateless(
        restoredBlockProof.targets,
        restoredBlockProof.positions,
        restoredBlockProof.proof_hashes,
        restoredBlockProof.numLeaves,
        restored.getRoots()));
    EXPECT_TRUE(live.verifyBatchProofStateless(
        liveBlockProof.targets,
        liveBlockProof.positions,
        liveBlockProof.proof_hashes,
        liveBlockProof.numLeaves,
        live.getRoots()));
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// INV-1: Proof siblings count == forest height for that leaf
// ═══════════════════════════════════════════════════════════════════════════════

class ProofSiblingsCountTest : public ::testing::Test {
protected:
    UtreexoForest forest;
};

TEST_F(ProofSiblingsCountTest, SingleLeafHasZeroSiblings) {
    forest.add(makeTestHash(1));

    auto proof = forest.prove(0);
    ASSERT_TRUE(proof.has_value());

    // Single leaf → tree height 0 → 0 siblings
    EXPECT_EQ(proof->siblings.size(), 0)
        << "Single leaf tree has height 0, should have 0 siblings";
}

TEST_F(ProofSiblingsCountTest, TwoLeavesHaveOneSibling) {
    forest.add(makeTestHash(1));
    forest.add(makeTestHash(2));

    for (uint64_t pos = 0; pos < 2; pos++) {
        auto proof = forest.prove(pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(proof->siblings.size(), 1)
            << "Tree of 2 leaves has height 1, proof at pos " << pos << " should have 1 sibling";
    }
}

TEST_F(ProofSiblingsCountTest, FourLeavesHaveTwoSiblings) {
    for (int i = 0; i < 4; i++) {
        forest.add(makeTestHash(i));
    }

    for (uint64_t pos = 0; pos < 4; pos++) {
        auto proof = forest.prove(pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(proof->siblings.size(), 2)
            << "Tree of 4 leaves has height 2, proof at pos " << pos << " should have 2 siblings";
    }
}

TEST_F(ProofSiblingsCountTest, MixedForestCorrectSiblingCounts) {
    // 5 leaves = tree(4) + tree(1) = heights 2 and 0
    for (int i = 0; i < 5; i++) {
        forest.add(makeTestHash(i));
    }

    // Positions 0-3 in tree of height 2
    for (uint64_t pos = 0; pos < 4; pos++) {
        auto proof = forest.prove(pos);
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(proof->siblings.size(), 2)
            << "Position " << pos << " in tree(4) should have 2 siblings";
    }

    // Position 4 in tree of height 0
    auto proof4 = forest.prove(4);
    ASSERT_TRUE(proof4.has_value());
    EXPECT_EQ(proof4->siblings.size(), 0)
        << "Position 4 in tree(1) should have 0 siblings";
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-2: No duplicate siblings in proof
// ═══════════════════════════════════════════════════════════════════════════════

class ProofNoDuplicatesTest : public ::testing::Test {
protected:
    UtreexoForest forest;
};

TEST_F(ProofNoDuplicatesTest, NoDuplicateSiblingsSmallTree) {
    for (int i = 0; i < 8; i++) {
        forest.add(makeTestHash(i));
    }

    for (uint64_t pos = 0; pos < 8; pos++) {
        auto proof = forest.prove(pos);
        ASSERT_TRUE(proof.has_value());

        size_t unique = countUnique(proof->siblings);
        EXPECT_EQ(unique, proof->siblings.size())
            << "Proof at position " << pos << " has duplicate siblings";
    }
}

TEST_F(ProofNoDuplicatesTest, NoDuplicateSiblingsLargeTree) {
    for (int i = 0; i < 100; i++) {
        forest.add(makeUTXOLeaf(i, 1000 * i));
    }

    // Test random positions
    for (uint64_t pos : {0UL, 50UL, 99UL, 63UL, 64UL}) {
        auto proof = forest.prove(pos);
        ASSERT_TRUE(proof.has_value());

        size_t unique = countUnique(proof->siblings);
        EXPECT_EQ(unique, proof->siblings.size())
            << "Proof at position " << pos << " has duplicate siblings";
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-3: Deterministic sibling ordering
// ═══════════════════════════════════════════════════════════════════════════════

class ProofDeterminismTest : public ::testing::Test {};

TEST_F(ProofDeterminismTest, SameForestSameLeafSameProof) {
    // Create two identical forests
    UtreexoForest forest1;
    UtreexoForest forest2;

    for (int i = 0; i < 16; i++) {
        auto hash = makeTestHash(i);
        forest1.add(hash);
        forest2.add(hash);
    }

    // Generate proofs from both - should be identical
    for (uint64_t pos = 0; pos < 16; pos++) {
        auto proof1 = forest1.prove(pos);
        auto proof2 = forest2.prove(pos);

        ASSERT_TRUE(proof1.has_value());
        ASSERT_TRUE(proof2.has_value());

        EXPECT_EQ(proof1->siblings.size(), proof2->siblings.size());
        EXPECT_EQ(proof1->position, proof2->position);
        EXPECT_EQ(proof1->numLeaves, proof2->numLeaves);

        for (size_t i = 0; i < proof1->siblings.size(); i++) {
            EXPECT_EQ(proof1->siblings[i], proof2->siblings[i])
                << "Sibling " << i << " differs at position " << pos;
        }
    }
}

TEST_F(ProofDeterminismTest, MultipleGenerationsIdentical) {
    UtreexoForest forest;
    for (int i = 0; i < 32; i++) {
        forest.add(makeTestHash(i));
    }

    // Generate same proof 10 times
    for (uint64_t pos = 0; pos < 32; pos++) {
        auto baseline = forest.prove(pos);
        ASSERT_TRUE(baseline.has_value());

        for (int run = 0; run < 10; run++) {
            auto proof = forest.prove(pos);
            ASSERT_TRUE(proof.has_value());

            EXPECT_EQ(proof->siblings, baseline->siblings)
                << "Proof generation is non-deterministic at pos " << pos << " run " << run;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-4: Generate → Verify round-trip
// ═══════════════════════════════════════════════════════════════════════════════

class ProofRoundTripTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        for (int i = 0; i < 64; i++) {
            auto leaf = makeUTXOLeaf(i, 50000000 + i * 1000);
            forest.add(leaf);
            leaves.push_back(leaf);
        }
    }
};

TEST_F(ProofRoundTripTest, AllLeavesVerifySuccessfully) {
    auto roots = forest.getRoots();

    for (size_t i = 0; i < leaves.size(); i++) {
        auto proof = forest.prove(i);
        ASSERT_TRUE(proof.has_value())
            << "Failed to generate proof for position " << i;

        bool verified = proof->verify(leaves[i], roots);
        EXPECT_TRUE(verified)
            << "Valid proof failed verification at position " << i;
    }
}

TEST_F(ProofRoundTripTest, BatchProofRoundTrip) {
    // Generate batch proof for multiple leaves using the stateful API
    // (This is what miners use internally)
    std::vector<UtreexoHash> targets;
    for (int i = 0; i < 10; i++) {
        targets.push_back(leaves[i * 5]);  // Every 5th leaf
    }

    // Use the simpler stateful batch proof API
    std::vector<UtreexoHash> proof_hashes = forest.generateBatchProof(targets);

    // The proof should be reasonably sized (deduplicated)
    EXPECT_GT(proof_hashes.size(), 0);
    EXPECT_LE(proof_hashes.size(), targets.size() * 7);  // ~log2(64) * targets

    // Verify using stateful batch verification
    bool verified = forest.verifyBatchProof(targets, proof_hashes);

    EXPECT_TRUE(verified)
        << "Batch proof failed verification";
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-5: Proof fails if any bit is flipped
// ═══════════════════════════════════════════════════════════════════════════════

class ProofTamperDetectionTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        for (int i = 0; i < 16; i++) {
            auto leaf = makeTestHash(i);
            forest.add(leaf);
            leaves.push_back(leaf);
        }
    }
};

TEST_F(ProofTamperDetectionTest, FlippedSiblingBitCausesFailure) {
    auto roots = forest.getRoots();
    auto proof = forest.prove(7);
    ASSERT_TRUE(proof.has_value());
    ASSERT_GT(proof->siblings.size(), 0);

    // Verify original works
    EXPECT_TRUE(proof->verify(leaves[7], roots));

    // Flip one bit in first sibling
    UtreexoProof tampered = *proof;
    tampered.siblings[0][0] ^= 0x01;

    EXPECT_FALSE(tampered.verify(leaves[7], roots))
        << "Tampered proof (flipped sibling bit) should fail";
}

TEST_F(ProofTamperDetectionTest, WrongLeafCausesFailure) {
    auto roots = forest.getRoots();
    auto proof = forest.prove(5);
    ASSERT_TRUE(proof.has_value());

    // Verify with correct leaf
    EXPECT_TRUE(proof->verify(leaves[5], roots));

    // Verify with wrong leaf
    EXPECT_FALSE(proof->verify(leaves[7], roots))
        << "Proof should fail when verified against wrong leaf";
}

TEST_F(ProofTamperDetectionTest, WrongRootCausesFailure) {
    auto proof = forest.prove(3);
    ASSERT_TRUE(proof.has_value());

    // Verify with correct roots
    auto correct_roots = forest.getRoots();
    EXPECT_TRUE(proof->verify(leaves[3], correct_roots));

    // Verify with tampered root
    auto tampered_roots = correct_roots;
    tampered_roots[0][0] ^= 0xFF;

    EXPECT_FALSE(proof->verify(leaves[3], tampered_roots))
        << "Proof should fail against tampered root";
}

TEST_F(ProofTamperDetectionTest, WrongPositionCausesFailure) {
    auto roots = forest.getRoots();
    auto proof = forest.prove(2);
    ASSERT_TRUE(proof.has_value());

    // Original verification works
    EXPECT_TRUE(proof->verify(leaves[2], roots));

    // Modify position
    UtreexoProof tampered = *proof;
    tampered.position = 5;  // Wrong position

    EXPECT_FALSE(tampered.verify(leaves[2], roots))
        << "Proof with wrong position should fail";
}

TEST_F(ProofTamperDetectionTest, TruncatedSiblingsCausesFailure) {
    auto roots = forest.getRoots();
    auto proof = forest.prove(0);
    ASSERT_TRUE(proof.has_value());
    ASSERT_GT(proof->siblings.size(), 1);

    // Original works
    EXPECT_TRUE(proof->verify(leaves[0], roots));

    // Truncate siblings
    UtreexoProof truncated = *proof;
    truncated.siblings.pop_back();

    EXPECT_FALSE(truncated.verify(leaves[0], roots))
        << "Truncated proof should fail";
}

TEST_F(ProofTamperDetectionTest, ExtraSiblingsCausesFailure) {
    auto roots = forest.getRoots();
    auto proof = forest.prove(0);
    ASSERT_TRUE(proof.has_value());

    // Original works
    EXPECT_TRUE(proof->verify(leaves[0], roots));

    // Add extra sibling
    UtreexoProof padded = *proof;
    padded.siblings.push_back(makeTestHash(999));

    EXPECT_FALSE(padded.verify(leaves[0], roots))
        << "Proof with extra siblings should fail";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Edge Cases
// ═══════════════════════════════════════════════════════════════════════════════

class ProofEdgeCasesTest : public ::testing::Test {};

TEST_F(ProofEdgeCasesTest, EmptyForestProofFails) {
    UtreexoForest forest;
    auto proof = forest.prove(0);
    EXPECT_FALSE(proof.has_value())
        << "Empty forest should not generate proof";
}

TEST_F(ProofEdgeCasesTest, OutOfBoundsPositionFails) {
    UtreexoForest forest;
    forest.add(makeTestHash(1));

    auto proof = forest.prove(100);  // Position doesn't exist
    EXPECT_FALSE(proof.has_value())
        << "Out of bounds position should not generate proof";
}

TEST_F(ProofEdgeCasesTest, SingleLeafProofVerifies) {
    UtreexoForest forest;
    auto leaf = makeTestHash(42);
    forest.add(leaf);

    auto proof = forest.prove(0);
    ASSERT_TRUE(proof.has_value());

    auto roots = forest.getRoots();
    EXPECT_TRUE(proof->verify(leaf, roots))
        << "Single leaf proof should verify";
}

TEST_F(ProofEdgeCasesTest, LargeTreeProofVerifies) {
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    for (int i = 0; i < 1000; i++) {
        auto leaf = makeUTXOLeaf(i, i * 1000);
        forest.add(leaf);
        leaves.push_back(leaf);
    }

    auto roots = forest.getRoots();

    // Test positions at various points
    for (uint64_t pos : {0UL, 1UL, 500UL, 999UL, 512UL, 511UL}) {
        auto proof = forest.prove(pos);
        ASSERT_TRUE(proof.has_value())
            << "Failed to generate proof for position " << pos;

        EXPECT_TRUE(proof->verify(leaves[pos], roots))
            << "Proof failed verification at position " << pos;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Regression Coverage: Duplicate-live-leaf hardening and leaf_positions_ rebuild
// ═══════════════════════════════════════════════════════════════════════════════

class LeafIndexRegressionTest : public ::testing::Test {};

TEST_F(LeafIndexRegressionTest, DuplicateLeafRejectionDoesNotPoisonForestAndAllowsReAddAfterRestore) {
    UtreexoForest forest;

    auto retained = makeUTXOLeaf(100, 11'000);
    auto reused = makeUTXOLeaf(200, 22'000);

    ASSERT_EQ(forest.add(retained), 0u);
    ASSERT_EQ(forest.add(reused), 1u);

    auto original_proof = forest.prove(1);
    ASSERT_TRUE(original_proof.has_value());
    EXPECT_TRUE(original_proof->verify(reused, forest.getRoots()));

    // Regression: rejecting a duplicate live leaf must not corrupt leaf_positions_.
    EXPECT_EQ(forest.add(reused), UINT64_MAX);
    EXPECT_EQ(forest.getNumLeaves(), 2u);

    auto live_pos = forest.findLeafPosition(reused);
    ASSERT_TRUE(live_pos.has_value());
    EXPECT_EQ(*live_pos, 1u);

    auto proof_after_reject = forest.prove(*live_pos);
    ASSERT_TRUE(proof_after_reject.has_value());
    EXPECT_TRUE(proof_after_reject->verify(reused, forest.getRoots()));

    ASSERT_TRUE(forest.remove(reused, *proof_after_reject));
    EXPECT_FALSE(forest.findLeafPosition(reused).has_value());

    auto restored = UtreexoForest::deserialize(forest.serialize());
    EXPECT_EQ(restored.getCommitment(), forest.getCommitment());
    EXPECT_FALSE(restored.findLeafPosition(reused).has_value());

    const uint64_t readded_pos = restored.add(reused);
    EXPECT_EQ(readded_pos, 2u);

    auto readded_lookup = restored.findLeafPosition(reused);
    ASSERT_TRUE(readded_lookup.has_value());
    EXPECT_EQ(*readded_lookup, readded_pos);

    auto readded_proof = restored.prove(readded_pos);
    ASSERT_TRUE(readded_proof.has_value());
    EXPECT_TRUE(readded_proof->verify(reused, restored.getRoots()));

    auto retained_lookup = restored.findLeafPosition(retained);
    ASSERT_TRUE(retained_lookup.has_value());
    auto retained_proof = restored.prove(*retained_lookup);
    ASSERT_TRUE(retained_proof.has_value());
    EXPECT_TRUE(retained_proof->verify(retained, restored.getRoots()));
}

TEST_F(LeafIndexRegressionTest, DeserializedForestMatchesLivePositionsAndBlockProofsAfterDeletesAndAdds) {
    UtreexoForest live;
    std::vector<UtreexoHash> original_leaves;

    for (uint64_t i = 0; i < 12; i++) {
        auto leaf = makeUTXOLeaf(1'000 + i, 50'000 + i);
        ASSERT_NE(live.add(leaf), UINT64_MAX);
        original_leaves.push_back(leaf);
    }

    for (uint64_t pos : {9u, 5u, 2u}) {
        auto proof = live.prove(pos);
        ASSERT_TRUE(proof.has_value());
        ASSERT_TRUE(live.remove(original_leaves[pos], *proof));
    }

    std::vector<UtreexoHash> appended_leaves;
    for (uint64_t i = 0; i < 3; i++) {
        auto leaf = makeUTXOLeaf(2'000 + i, 90'000 + i);
        ASSERT_NE(live.add(leaf), UINT64_MAX);
        appended_leaves.push_back(leaf);
    }

    auto restored = UtreexoForest::deserialize(live.serialize());

    EXPECT_EQ(restored.getNumLeaves(), live.getNumLeaves());
    EXPECT_EQ(restored.getActiveLeaves(), live.getActiveLeaves());
    EXPECT_EQ(restored.getCommitment(), live.getCommitment());

    for (uint64_t deleted : {2u, 5u, 9u}) {
        EXPECT_FALSE(live.findLeafPosition(original_leaves[deleted]).has_value());
        EXPECT_FALSE(restored.findLeafPosition(original_leaves[deleted]).has_value());
    }

    std::vector<UtreexoHash> active_targets = {
        original_leaves[0],
        original_leaves[3],
        original_leaves[7],
        original_leaves[11],
        appended_leaves[1],
    };

    for (const auto& leaf : active_targets) {
        auto live_pos = live.findLeafPosition(leaf);
        auto restored_pos = restored.findLeafPosition(leaf);
        ASSERT_TRUE(live_pos.has_value());
        ASSERT_TRUE(restored_pos.has_value());
        EXPECT_EQ(*live_pos, *restored_pos);

        auto live_proof = live.prove(*live_pos);
        auto restored_proof = restored.prove(*restored_pos);
        ASSERT_TRUE(live_proof.has_value());
        ASSERT_TRUE(restored_proof.has_value());

        EXPECT_EQ(restored_proof->siblings, live_proof->siblings);
        EXPECT_EQ(restored_proof->position, live_proof->position);
        EXPECT_EQ(restored_proof->numLeaves, live_proof->numLeaves);
        EXPECT_TRUE(restored_proof->verify(leaf, restored.getRoots()));
    }

    auto live_block_proof = live.generateBlockProof(active_targets);
    auto restored_block_proof = restored.generateBlockProof(active_targets);

    EXPECT_EQ(restored_block_proof.targets, live_block_proof.targets);
    EXPECT_EQ(restored_block_proof.positions, live_block_proof.positions);
    EXPECT_EQ(restored_block_proof.proof_hashes, live_block_proof.proof_hashes);
    EXPECT_EQ(restored_block_proof.numLeaves, live_block_proof.numLeaves);

    EXPECT_TRUE(restored.verifyBatchProofStateless(
        restored_block_proof.targets,
        restored_block_proof.positions,
        restored_block_proof.proof_hashes,
        restored_block_proof.numLeaves,
        restored.getRoots()));
}

TEST_F(LeafIndexRegressionTest, RestoredForestMatchesLiveProofsAfterRestartAndReorg) {
    UtreexoForest live;
    std::vector<UtreexoHash> baseLeaves;

    for (uint64_t i = 0; i < 10; i++) {
        auto leaf = makeUTXOLeaf(3'000 + i, 70'000 + i);
        ASSERT_NE(live.add(leaf), UINT64_MAX);
        baseLeaves.push_back(leaf);
    }

    removeLeafOrAssert(live, baseLeaves[8]);
    removeLeafOrAssert(live, baseLeaves[3]);

    std::vector<UtreexoHash> connectedLeaves;
    for (uint64_t i = 0; i < 2; i++) {
        auto leaf = makeUTXOLeaf(4'000 + i, 90'000 + i);
        ASSERT_NE(live.add(leaf), UINT64_MAX);
        connectedLeaves.push_back(leaf);
    }

    auto restored = UtreexoForest::deserialize(live.serialize());
    expectEquivalentForestState(live, restored, {
        baseLeaves[0],
        baseLeaves[2],
        baseLeaves[5],
        baseLeaves[9],
        connectedLeaves[1],
    });

    ASSERT_TRUE(live.removeLastNLeaves(connectedLeaves.size()));
    ASSERT_TRUE(restored.removeLastNLeaves(connectedLeaves.size()));
    ASSERT_TRUE(live.restoreDeletedLeaf(3, baseLeaves[3]));
    ASSERT_TRUE(live.restoreDeletedLeaf(8, baseLeaves[8]));
    ASSERT_TRUE(restored.restoreDeletedLeaf(3, baseLeaves[3]));
    ASSERT_TRUE(restored.restoreDeletedLeaf(8, baseLeaves[8]));

    removeLeafOrAssert(live, baseLeaves[6]);
    removeLeafOrAssert(restored, baseLeaves[6]);

    std::vector<UtreexoHash> reorgLeaves;
    for (uint64_t i = 0; i < 3; i++) {
        auto leaf = makeUTXOLeaf(5'000 + i, 120'000 + i);
        ASSERT_NE(live.add(leaf), UINT64_MAX);
        ASSERT_NE(restored.add(leaf), UINT64_MAX);
        reorgLeaves.push_back(leaf);
    }

    expectEquivalentForestState(live, restored, {
        baseLeaves[0],
        baseLeaves[3],
        baseLeaves[8],
        baseLeaves[9],
        reorgLeaves[0],
        reorgLeaves[2],
    });
}

TEST_F(LeafIndexRegressionTest, RestartedReorgedForestRejectsDuplicateLeafWithoutLeafIndexDivergence) {
    UtreexoForest live;
    std::vector<UtreexoHash> baseLeaves;

    for (uint64_t i = 0; i < 7; i++) {
        auto leaf = makeUTXOLeaf(6'000 + i, 150'000 + i);
        ASSERT_NE(live.add(leaf), UINT64_MAX);
        baseLeaves.push_back(leaf);
    }

    auto restarted = UtreexoForest::deserialize(live.serialize());
    removeLeafOrAssert(live, baseLeaves[5]);
    removeLeafOrAssert(restarted, baseLeaves[5]);

    std::vector<UtreexoHash> reorgLeaves;
    for (uint64_t i = 0; i < 2; i++) {
        auto leaf = makeUTXOLeaf(7'000 + i, 180'000 + i);
        ASSERT_NE(live.add(leaf), UINT64_MAX);
        ASSERT_NE(restarted.add(leaf), UINT64_MAX);
        reorgLeaves.push_back(leaf);
    }

    expectEquivalentForestState(live, restarted, {
        baseLeaves[0],
        baseLeaves[2],
        baseLeaves[6],
        reorgLeaves[0],
        reorgLeaves[1],
    });

    EXPECT_EQ(live.add(reorgLeaves[1]), UINT64_MAX);
    EXPECT_EQ(restarted.add(reorgLeaves[1]), UINT64_MAX);

    expectEquivalentForestState(live, restarted, {
        baseLeaves[0],
        baseLeaves[2],
        baseLeaves[6],
        reorgLeaves[0],
        reorgLeaves[1],
    });
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Utreexo Proof Generation Correctness Tests\n";
    std::cout << "  Phase U.1: CONSENSUS CRITICAL\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
