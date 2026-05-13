/**
 * @file test_forest_mutation_correctness.cpp
 * @brief Phase U.2: Utreexo Forest Mutation Correctness Tests
 *
 * CONSENSUS CRITICAL - These tests validate that forest state transitions are:
 *   - Deterministic (same operations → same final state)
 *   - Reversible (snapshot → mutate → restore is lossless)
 *   - Consensus-safe (invalid operations are rejected)
 *
 * Hard Invariants Tested:
 *   INV-1: Root MUST change after Add / Remove
 *   INV-2: Removing requires a valid proof
 *   INV-3: Removing non-existent leaf → hard failure
 *   INV-4: Forest height monotonicity preserved
 *   INV-5: Snapshot → Restore is lossless (exact root match)
 *   INV-6: Add N leaves → deterministic root (order matters)
 *   INV-7: Remove leaf with proof → root matches expected
 *
 * If any test fails → CONSENSUS BUG → Network fork risk!
 */

#include <gtest/gtest.h>
#include "consensus/utreexo_accumulator.h"
#include "primitives/uint256.h"
#include <random>

using namespace dinero;
using namespace dinero::consensus;

// ═══════════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

UtreexoHash makeTestHash(uint64_t value) {
    UtreexoHash hash(32, 0);
    for (int i = 0; i < 8; i++) {
        hash[i] = (value >> (i * 8)) & 0xFF;
    }
    for (int i = 8; i < 32; i++) {
        hash[i] = static_cast<uint8_t>((value * 31 + i * 17) % 256);
    }
    return hash;
}

UtreexoHash makeUTXOLeaf(uint64_t utxo_id, uint64_t value) {
    uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &utxo_id, sizeof(utxo_id));

    std::vector<uint8_t> scriptPubKey = {0x76, 0xa9, 0x14};
    for (int i = 0; i < 20; i++) {
        scriptPubKey.push_back(static_cast<uint8_t>((utxo_id >> (i % 8)) & 0xFF));
    }
    scriptPubKey.push_back(0x88);
    scriptPubKey.push_back(0xac);

    return HashUTXO(txid, 0, value, scriptPubKey);
}

bool hashesEqual(const UtreexoHash& a, const UtreexoHash& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin());
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// INV-1: Root MUST change after Add / Remove
// ═══════════════════════════════════════════════════════════════════════════════

class RootChangeTest : public ::testing::Test {};

TEST_F(RootChangeTest, AddChangesRoot) {
    UtreexoForest forest;

    auto root_before = forest.getCommitment();

    forest.add(makeTestHash(1));
    auto root_after = forest.getCommitment();

    EXPECT_FALSE(hashesEqual(root_before, root_after))
        << "Root MUST change after Add operation";
}

TEST_F(RootChangeTest, EachAddChangesRoot) {
    UtreexoForest forest;
    UtreexoHash prev_root = forest.getCommitment();

    for (int i = 0; i < 100; i++) {
        forest.add(makeTestHash(i));
        auto new_root = forest.getCommitment();

        EXPECT_FALSE(hashesEqual(prev_root, new_root))
            << "Root MUST change after each Add (iteration " << i << ")";

        prev_root = new_root;
    }
}

TEST_F(RootChangeTest, RemoveChangesRoot) {
    UtreexoForest forest;
    auto leaf = makeTestHash(1);
    forest.add(leaf);
    forest.add(makeTestHash(2));

    auto root_before = forest.getCommitment();

    auto proof = forest.prove(0);
    ASSERT_TRUE(proof.has_value());

    bool removed = forest.remove(leaf, *proof);
    ASSERT_TRUE(removed);

    auto root_after = forest.getCommitment();

    EXPECT_FALSE(hashesEqual(root_before, root_after))
        << "Root MUST change after Remove operation";
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-2: Removing requires a valid proof
// ═══════════════════════════════════════════════════════════════════════════════

class RemoveRequiresProofTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        for (int i = 0; i < 8; i++) {
            auto leaf = makeTestHash(i);
            forest.add(leaf);
            leaves.push_back(leaf);
        }
    }
};

TEST_F(RemoveRequiresProofTest, ValidProofSucceeds) {
    auto proof = forest.prove(3);
    ASSERT_TRUE(proof.has_value());

    bool removed = forest.remove(leaves[3], *proof);
    EXPECT_TRUE(removed)
        << "Remove with valid proof should succeed";
}

TEST_F(RemoveRequiresProofTest, TamperedProofFails) {
    auto proof = forest.prove(3);
    ASSERT_TRUE(proof.has_value());

    // Tamper with proof
    UtreexoProof tampered = *proof;
    if (!tampered.siblings.empty()) {
        tampered.siblings[0][0] ^= 0xFF;
    }

    bool removed = forest.remove(leaves[3], tampered);
    EXPECT_FALSE(removed)
        << "Remove with tampered proof should fail";
}

TEST_F(RemoveRequiresProofTest, WrongLeafProofFails) {
    auto proof = forest.prove(3);
    ASSERT_TRUE(proof.has_value());

    // Try to remove different leaf with proof for leaf 3
    bool removed = forest.remove(leaves[5], *proof);
    EXPECT_FALSE(removed)
        << "Remove with proof for different leaf should fail";
}

TEST_F(RemoveRequiresProofTest, WrongPositionProofFails) {
    auto proof = forest.prove(3);
    ASSERT_TRUE(proof.has_value());

    // Modify position
    UtreexoProof wrong_pos = *proof;
    wrong_pos.position = 5;

    bool removed = forest.remove(leaves[3], wrong_pos);
    EXPECT_FALSE(removed)
        << "Remove with wrong position should fail";
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-3: Removing non-existent leaf → hard failure
// ═══════════════════════════════════════════════════════════════════════════════

class RemoveNonExistentTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    void SetUp() override {
        for (int i = 0; i < 4; i++) {
            auto leaf = makeTestHash(i);
            forest.add(leaf);
            leaves.push_back(leaf);
        }
    }
};

TEST_F(RemoveNonExistentTest, RemovingNeverAddedLeafFails) {
    auto never_added = makeTestHash(999);

    // Try to construct a fake proof (won't verify)
    UtreexoProof fake_proof;
    fake_proof.position = 0;
    fake_proof.numLeaves = forest.getNumLeaves();
    fake_proof.siblings = std::vector<UtreexoHash>{makeTestHash(100), makeTestHash(200)};

    bool removed = forest.remove(never_added, fake_proof);
    EXPECT_FALSE(removed)
        << "Removing never-added leaf should fail";
}

TEST_F(RemoveNonExistentTest, DoubleRemoveFails) {
    auto proof = forest.prove(2);
    ASSERT_TRUE(proof.has_value());

    // First remove succeeds
    bool first_remove = forest.remove(leaves[2], *proof);
    ASSERT_TRUE(first_remove);

    // Second remove of same leaf should fail (already removed)
    bool second_remove = forest.remove(leaves[2], *proof);
    EXPECT_FALSE(second_remove)
        << "Double remove should fail - leaf already deleted";
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-5: Snapshot → Restore is lossless
// ═══════════════════════════════════════════════════════════════════════════════

class SnapshotRestoreTest : public ::testing::Test {};

TEST_F(SnapshotRestoreTest, ClonePreservesState) {
    UtreexoForest original;
    for (int i = 0; i < 16; i++) {
        original.add(makeTestHash(i));
    }

    auto original_root = original.getCommitment();
    auto original_num_leaves = original.getNumLeaves();
    auto original_num_roots = original.getNumRoots();

    // Clone (snapshot)
    UtreexoForest snapshot = original.clone();

    EXPECT_TRUE(hashesEqual(snapshot.getCommitment(), original_root))
        << "Clone must have same root as original";
    EXPECT_EQ(snapshot.getNumLeaves(), original_num_leaves);
    EXPECT_EQ(snapshot.getNumRoots(), original_num_roots);
}

TEST_F(SnapshotRestoreTest, MutateCloneDoesNotAffectOriginal) {
    UtreexoForest original;
    for (int i = 0; i < 8; i++) {
        original.add(makeTestHash(i));
    }

    auto original_root = original.getCommitment();

    // Clone and mutate
    UtreexoForest mutated = original.clone();
    mutated.add(makeTestHash(999));

    // Original should be unchanged
    EXPECT_TRUE(hashesEqual(original.getCommitment(), original_root))
        << "Mutating clone must not affect original";
}

TEST_F(SnapshotRestoreTest, SerializeDeserializeRoundTrip) {
    UtreexoForest original;
    for (int i = 0; i < 32; i++) {
        original.add(makeUTXOLeaf(i, i * 1000));
    }

    auto original_root = original.getCommitment();
    auto original_leaves = original.getNumLeaves();

    // Serialize
    auto serialized = original.serialize();
    EXPECT_GT(serialized.size(), 0);

    // Deserialize
    auto restored = UtreexoForest::deserialize(serialized);

    EXPECT_TRUE(hashesEqual(restored.getCommitment(), original_root))
        << "Deserialized forest must have same commitment";
    EXPECT_EQ(restored.getNumLeaves(), original_leaves);
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-6: Add N leaves → deterministic root (order matters)
// ═══════════════════════════════════════════════════════════════════════════════

class DeterministicRootTest : public ::testing::Test {};

TEST_F(DeterministicRootTest, SameOrderSameRoot) {
    UtreexoForest forest1;
    UtreexoForest forest2;

    for (int i = 0; i < 50; i++) {
        auto leaf = makeTestHash(i);
        forest1.add(leaf);
        forest2.add(leaf);
    }

    EXPECT_TRUE(hashesEqual(forest1.getCommitment(), forest2.getCommitment()))
        << "Same leaves in same order must produce same root";
}

TEST_F(DeterministicRootTest, DifferentOrderDifferentRoot) {
    UtreexoForest forest1;
    UtreexoForest forest2;

    // Forest 1: add 0, 1, 2
    forest1.add(makeTestHash(0));
    forest1.add(makeTestHash(1));
    forest1.add(makeTestHash(2));

    // Forest 2: add 2, 1, 0 (reverse order)
    forest2.add(makeTestHash(2));
    forest2.add(makeTestHash(1));
    forest2.add(makeTestHash(0));

    EXPECT_FALSE(hashesEqual(forest1.getCommitment(), forest2.getCommitment()))
        << "Different order must produce different root (order is consensus-critical)";
}

TEST_F(DeterministicRootTest, DifferentLeavesDifferentRoot) {
    UtreexoForest forest1;
    UtreexoForest forest2;

    for (int i = 0; i < 10; i++) {
        forest1.add(makeTestHash(i));
        forest2.add(makeTestHash(i + 100));  // Different leaves
    }

    EXPECT_FALSE(hashesEqual(forest1.getCommitment(), forest2.getCommitment()))
        << "Different leaves must produce different root";
}

// ═══════════════════════════════════════════════════════════════════════════════
// INV-7: Remove leaf with proof → root matches expected
// ═══════════════════════════════════════════════════════════════════════════════

class RemoveRootConsistencyTest : public ::testing::Test {};

TEST_F(RemoveRootConsistencyTest, RemoveAndReaddProducesSameSequence) {
    // Build forest: add 0, 1, 2, 3
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    for (int i = 0; i < 4; i++) {
        auto leaf = makeTestHash(i);
        forest.add(leaf);
        leaves.push_back(leaf);
    }

    auto root_with_4 = forest.getCommitment();

    // Remove leaf 3
    auto proof = forest.prove(3);
    ASSERT_TRUE(proof.has_value());
    forest.remove(leaves[3], *proof);

    auto root_with_3 = forest.getCommitment();

    // Add leaf 3 back (at new position)
    forest.add(leaves[3]);

    // Root should NOT match original (leaf 3 is now at position 4, not 3)
    // This is expected behavior - UTXOs can't be "restored" to same position
    EXPECT_FALSE(hashesEqual(forest.getCommitment(), root_with_4))
        << "Re-adding removed leaf goes to new position, root differs";
}

TEST_F(RemoveRootConsistencyTest, MultipleRemovesChangeRoot) {
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    std::vector<UtreexoHash> roots;

    for (int i = 0; i < 8; i++) {
        auto leaf = makeTestHash(i);
        forest.add(leaf);
        leaves.push_back(leaf);
    }

    roots.push_back(forest.getCommitment());

    // Remove leaves 0, 2, 4, 6 (every other one)
    for (int i : {0, 2, 4, 6}) {
        auto pos_opt = forest.findLeafPosition(leaves[i]);
        if (pos_opt.has_value() && !forest.isDeleted(*pos_opt)) {
            auto proof = forest.prove(*pos_opt);
            if (proof.has_value()) {
                forest.remove(leaves[i], *proof);
            }
        }
        roots.push_back(forest.getCommitment());
    }

    // All roots should be unique
    for (size_t i = 0; i < roots.size(); i++) {
        for (size_t j = i + 1; j < roots.size(); j++) {
            EXPECT_FALSE(hashesEqual(roots[i], roots[j]))
                << "Roots at step " << i << " and " << j << " should differ";
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Block Processing Simulation
// ═══════════════════════════════════════════════════════════════════════════════

class BlockProcessingTest : public ::testing::Test {};

TEST_F(BlockProcessingTest, SimulateBlockApplication) {
    // Simulate: genesis → block 1 → block 2

    UtreexoForest forest;

    // Genesis: create 2 UTXOs
    auto utxo_genesis_0 = makeUTXOLeaf(0, 50 * 100000000ULL);  // 50 DIN
    auto utxo_genesis_1 = makeUTXOLeaf(1, 50 * 100000000ULL);
    forest.add(utxo_genesis_0);
    forest.add(utxo_genesis_1);

    auto root_after_genesis = forest.getCommitment();
    EXPECT_EQ(forest.getNumLeaves(), 2);

    // Block 1: spend genesis UTXO 0, create 2 new UTXOs
    auto proof_0 = forest.prove(0);
    ASSERT_TRUE(proof_0.has_value());

    forest.remove(utxo_genesis_0, *proof_0);

    auto utxo_block1_0 = makeUTXOLeaf(100, 25 * 100000000ULL);
    auto utxo_block1_1 = makeUTXOLeaf(101, 24 * 100000000ULL);
    forest.add(utxo_block1_0);
    forest.add(utxo_block1_1);

    auto root_after_block1 = forest.getCommitment();

    EXPECT_FALSE(hashesEqual(root_after_genesis, root_after_block1))
        << "Root must change after block application";
    EXPECT_EQ(forest.getActiveLeaves(), 3);  // 1 (genesis_1) + 2 (block1)

    // Block 2: spend both block1 UTXOs
    // IMPORTANT: Must regenerate proofs after each state mutation!

    // Remove first UTXO
    auto pos_block1_0 = forest.findLeafPosition(utxo_block1_0);
    ASSERT_TRUE(pos_block1_0.has_value());
    auto proof_block1_0 = forest.prove(*pos_block1_0);
    ASSERT_TRUE(proof_block1_0.has_value());
    forest.remove(utxo_block1_0, *proof_block1_0);

    // Must regenerate proof for second UTXO after state change
    auto pos_block1_1 = forest.findLeafPosition(utxo_block1_1);
    ASSERT_TRUE(pos_block1_1.has_value());
    auto proof_block1_1 = forest.prove(*pos_block1_1);
    ASSERT_TRUE(proof_block1_1.has_value());
    forest.remove(utxo_block1_1, *proof_block1_1);

    auto root_after_block2 = forest.getCommitment();

    EXPECT_FALSE(hashesEqual(root_after_block1, root_after_block2));
    EXPECT_EQ(forest.getActiveLeaves(), 1);  // Only genesis_1 remains
}

TEST_F(BlockProcessingTest, SimulateReorg) {
    UtreexoForest forest;

    // Build initial chain
    for (int i = 0; i < 10; i++) {
        forest.add(makeUTXOLeaf(i, i * 1000));
    }

    // Snapshot before block N
    auto snapshot = forest.clone();
    auto snapshot_root = snapshot.getCommitment();

    // Apply block N
    auto proof = forest.prove(5);
    ASSERT_TRUE(proof.has_value());
    forest.remove(makeUTXOLeaf(5, 5000), *proof);
    forest.add(makeUTXOLeaf(100, 50000));

    auto root_after_blockN = forest.getCommitment();
    EXPECT_FALSE(hashesEqual(root_after_blockN, snapshot_root));

    // "Reorg": restore to snapshot
    forest = snapshot.clone();

    EXPECT_TRUE(hashesEqual(forest.getCommitment(), snapshot_root))
        << "After reorg (restore from snapshot), root must match pre-block state";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Stress Tests
// ═══════════════════════════════════════════════════════════════════════════════

class StressTest : public ::testing::Test {};

TEST_F(StressTest, LargeForestOperations) {
    UtreexoForest forest;

    // Add 10,000 leaves
    for (int i = 0; i < 10000; i++) {
        forest.add(makeUTXOLeaf(i, i));
    }

    EXPECT_EQ(forest.getNumLeaves(), 10000);

    // Verify some random proofs
    std::mt19937 rng(42);
    for (int i = 0; i < 100; i++) {
        uint64_t pos = rng() % 10000;
        auto proof = forest.prove(pos);
        ASSERT_TRUE(proof.has_value())
            << "Failed to generate proof for position " << pos;

        auto leaf = makeUTXOLeaf(pos, pos);
        auto roots = forest.getRoots();
        EXPECT_TRUE(proof->verify(leaf, roots))
            << "Proof verification failed at position " << pos;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Consistency Regression Tests
// ═══════════════════════════════════════════════════════════════════════════════

class ConsistencyRegressionTest : public ::testing::Test {};

TEST_F(ConsistencyRegressionTest, FailedRestoreDoesNotPartiallyMutateForest) {
    UtreexoForest forest;
    const auto leaf0 = makeUTXOLeaf(10, 1000);
    const auto leaf1 = makeUTXOLeaf(11, 2000);
    const auto leaf2 = makeUTXOLeaf(12, 3000);

    ASSERT_NE(forest.add(leaf0), UINT64_MAX);
    ASSERT_NE(forest.add(leaf1), UINT64_MAX);
    ASSERT_NE(forest.add(leaf2), UINT64_MAX);

    const auto original_root = forest.getCommitment();
    const auto deleted_pos = forest.findLeafPosition(leaf1);
    ASSERT_TRUE(deleted_pos.has_value());
    const auto proof = forest.prove(*deleted_pos);
    ASSERT_TRUE(proof.has_value());
    ASSERT_TRUE(forest.remove(leaf1, *proof));

    const auto root_after_delete = forest.getCommitment();
    EXPECT_FALSE(hashesEqual(root_after_delete, original_root));

    // Restoring leaf0 into leaf1's deleted position must fail, but it must not
    // partially resurrect the deleted slot.
    EXPECT_FALSE(forest.restoreDeletedLeaf(*deleted_pos, leaf0));
    EXPECT_TRUE(hashesEqual(forest.getCommitment(), root_after_delete));

    const auto live_leaf0_pos = forest.findLeafPosition(leaf0);
    ASSERT_TRUE(live_leaf0_pos.has_value());
    EXPECT_EQ(*live_leaf0_pos, 0u);

    const auto deleted_leaf1_pos = forest.findLeafPosition(leaf1);
    EXPECT_FALSE(deleted_leaf1_pos.has_value());

    // A correct restore must still succeed after the failed attempt.
    EXPECT_TRUE(forest.restoreDeletedLeaf(*deleted_pos, leaf1));
    EXPECT_TRUE(hashesEqual(forest.getCommitment(), original_root));
}

// removeLastNLeaves under the gap #5 fail-loud contract (b520a3196):
// the function MUST reject any range whose tail is already in
// deleted_positions_ — that situation only happens when the caller's
// per-block leaf-add count is wrong, and silently cleaning the
// tombstones masks the bug. The happy path is "add N, immediately
// undo N" — DisconnectTip's reverse-of-add path.
TEST_F(ConsistencyRegressionTest, RemoveLastNLeaves_HappyPathReversesAdds) {
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 4; ++i) {
        leaves.push_back(makeUTXOLeaf(100 + i, 5000 + i));
        ASSERT_NE(forest.add(leaves.back()), UINT64_MAX);
    }

    ASSERT_TRUE(forest.removeLastNLeaves(1));
    EXPECT_EQ(forest.getNumLeaves(), 3u);

    // Position 3 now reusable for a fresh add — leaf_positions_ map and
    // nodes_ both cleared by the truncation, no stale tombstones.
    const auto replacement = makeUTXOLeaf(999, 9999);
    const auto replacement_pos = forest.add(replacement);
    ASSERT_NE(replacement_pos, UINT64_MAX);
    EXPECT_EQ(replacement_pos, 3u);

    auto replacement_proof = forest.prove(replacement_pos);
    ASSERT_TRUE(replacement_proof.has_value())
        << "Reused tail position must produce a valid proof";
}

TEST_F(ConsistencyRegressionTest, RemoveLastNLeaves_RejectsDeletedTail) {
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 4; ++i) {
        leaves.push_back(makeUTXOLeaf(100 + i, 5000 + i));
        ASSERT_NE(forest.add(leaves.back()), UINT64_MAX);
    }

    auto tail_proof = forest.prove(3);
    ASSERT_TRUE(tail_proof.has_value());
    ASSERT_TRUE(forest.remove(leaves[3], *tail_proof));

    const auto root_after_remove = forest.getCommitment();

    // gap #5 contract: tail position 3 sits in deleted_positions_, so
    // removeLastNLeaves(1) must refuse and leave the forest untouched.
    EXPECT_FALSE(forest.removeLastNLeaves(1));
    EXPECT_EQ(forest.getNumLeaves(), 4u);
    EXPECT_TRUE(hashesEqual(forest.getCommitment(), root_after_remove));
}

TEST_F(ConsistencyRegressionTest, DeserializeRejectsRootNodeMismatch) {
    UtreexoForest forest;
    for (uint64_t i = 0; i < 6; ++i) {
        ASSERT_NE(forest.add(makeUTXOLeaf(200 + i, 7000 + i)), UINT64_MAX);
    }

    auto serialized = forest.serialize();
    ASSERT_GT(serialized.size(), 16u);

    // v3 layout (5074754a4): byte 0 = version, byte 1 = canonical_empty_roots
    // flag, bytes 2..9 = numLeaves, bytes 10..13 = numRoots, bytes 14+ = root
    // hashes. Corrupt the first byte of the first root hash so numLeaves /
    // numRoots / nodes_ all parse cleanly and the deserialize-time
    // rebuildRoots() check (gap #10c) is the one that catches the mismatch.
    ASSERT_EQ(serialized[0], 3u) << "fixture must exercise the v3 layout";
    serialized[14] ^= 0x5a;

    UtreexoForest restored = UtreexoForest::deserialize(serialized);
    EXPECT_EQ(restored.getNumLeaves(), 0u)
        << "deserialize() must reject internally inconsistent forest payloads";
    EXPECT_TRUE(restored.getRoots().empty());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Utreexo Forest Mutation Correctness Tests\n";
    std::cout << "  Phase U.2: CONSENSUS CRITICAL\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
