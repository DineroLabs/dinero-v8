/**
 * Utreexo Stump Deletion Test Suite
 *
 * Cross-validates stump deletion (modify) against the forest.
 * For each test: build forest, extract stump, apply same deletions
 * to both, verify commitments match.
 */

#include "consensus/utreexo_accumulator.h"
#include "consensus/utreexo_stump.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>

using namespace dinero;
using namespace dinero::consensus;

// ============================================================================
// Helpers
// ============================================================================

static UtreexoHash makeLeaf(uint64_t id, uint64_t value) {
    uint256 txid;
    std::memset(txid.data, 0, 32);
    std::memcpy(txid.data, &id, sizeof(id));
    std::vector<uint8_t> spk = {0x51, 0x20};
    spk.resize(34, 0x00);
    return HashUTXOLegacy(txid, 0, value, spk);
}

static int tests_passed = 0;
static int tests_total = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_total++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        assert(false); \
    } else { \
        tests_passed++; \
    } \
} while(0)

/**
 * Core invariant helper: build forest+stump, delete given positions from both,
 * verify commitments match.
 *
 * Returns true if commitments match after deletions.
 */
static bool testDeletionInvariant(
    uint64_t numLeaves,
    const std::vector<uint64_t>& deletePositions
) {
    // Build forest with numLeaves leaves
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < numLeaves; ++i) {
        UtreexoHash leaf = makeLeaf(i + 1000, 100 + i);
        leaves.push_back(leaf);
        forest.add(leaf);
    }

    // Extract stump from forest
    UtreexoStump stump = UtreexoStump::fromForest(forest);

    // Prove all targets from the ORIGINAL forest (before any removals)
    std::vector<UtreexoHash> targets;
    std::vector<uint64_t> positions;
    std::vector<UtreexoHash> all_proof_hashes;

    for (uint64_t pos : deletePositions) {
        if (pos >= numLeaves) continue;
        auto leaf_pos = forest.findLeafPosition(leaves[pos]);
        if (!leaf_pos.has_value()) return false;

        auto proof = forest.prove(leaf_pos.value());
        if (!proof.has_value()) return false;

        targets.push_back(leaves[pos]);
        positions.push_back(leaf_pos.value());
        all_proof_hashes.insert(all_proof_hashes.end(),
            proof.value().siblings.begin(),
            proof.value().siblings.end());
    }

    // Apply deletions to forest
    for (size_t i = 0; i < targets.size(); ++i) {
        auto pos = forest.findLeafPosition(targets[i]);
        if (!pos.has_value()) continue;
        auto proof = forest.prove(pos.value());
        if (!proof.has_value()) continue;
        forest.remove(targets[i], proof.value());
    }

    // Apply deletions to stump via modify (no additions)
    BlockUtreexoProof bp;
    bp.targets = targets;
    bp.positions = positions;
    bp.proof_hashes = all_proof_hashes;
    bp.numLeaves = numLeaves;

    bool ok = stump.modify(bp, {});
    if (!ok) return false;

    // Compare commitments
    return stump.getCommitment() == forest.getCommitment();
}

// ============================================================================
// Test 1: Single deletion from various tree sizes
// ============================================================================

void test_single_deletion_various_sizes() {
    std::cout << "Test 1: Single deletion from various tree sizes..." << std::endl;

    std::vector<uint64_t> sizes = {1, 2, 3, 4, 5, 7, 8, 13, 16, 17};

    for (uint64_t n : sizes) {
        // Delete position 0 (leftmost)
        TEST_ASSERT(testDeletionInvariant(n, {0}),
            "Delete pos 0 from " + std::to_string(n) + " leaves");

        // Delete last position
        TEST_ASSERT(testDeletionInvariant(n, {n - 1}),
            "Delete last pos from " + std::to_string(n) + " leaves");

        // Delete middle position (if > 2 leaves)
        if (n > 2) {
            TEST_ASSERT(testDeletionInvariant(n, {n / 2}),
                "Delete middle pos from " + std::to_string(n) + " leaves");
        }
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 2: Multiple deletions from same tree
// ============================================================================

void test_multiple_deletions_same_tree() {
    std::cout << "Test 2: Multiple deletions from same tree..." << std::endl;

    // 8 leaves = single tree of height 3. Delete scattered positions.
    TEST_ASSERT(testDeletionInvariant(8, {1, 3, 5}),
        "Scattered deletions in 8-leaf tree");

    // Sibling pair: positions 4 and 5
    TEST_ASSERT(testDeletionInvariant(8, {4, 5}),
        "Sibling pair deletion in 8-leaf tree");

    // Sibling pair: positions 0 and 1
    TEST_ASSERT(testDeletionInvariant(8, {0, 1}),
        "First sibling pair deletion in 8-leaf tree");

    // Multiple sibling pairs: 0,1 and 6,7
    TEST_ASSERT(testDeletionInvariant(8, {0, 1, 6, 7}),
        "Two sibling pairs in 8-leaf tree");

    // 4 leaves, delete 3
    TEST_ASSERT(testDeletionInvariant(4, {0, 1, 3}),
        "Delete 3 of 4 leaves");

    // 16 leaves, delete positions spanning the tree
    TEST_ASSERT(testDeletionInvariant(16, {0, 4, 8, 12}),
        "Every 4th leaf in 16-leaf tree");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 3: Multiple deletions from different trees
// ============================================================================

void test_multiple_deletions_different_trees() {
    std::cout << "Test 3: Multiple deletions from different trees..." << std::endl;

    // 5 leaves (0b101): tree h2[0-3], tree h0[4]
    // Delete from both trees
    TEST_ASSERT(testDeletionInvariant(5, {2, 4}),
        "Delete from both trees (5 leaves)");

    // 13 leaves (0b1101): tree h3[0-7], tree h2[8-11], tree h0[12]
    // Delete one from each tree
    TEST_ASSERT(testDeletionInvariant(13, {3, 10, 12}),
        "Delete one from each tree (13 leaves)");

    // 7 leaves (0b111): tree h2[0-3], tree h1[4-5], tree h0[6]
    TEST_ASSERT(testDeletionInvariant(7, {1, 5, 6}),
        "Delete one from each tree (7 leaves)");

    // 11 leaves (0b1011): tree h3[0-7], tree h1[8-9], tree h0[10]
    TEST_ASSERT(testDeletionInvariant(11, {0, 7, 8, 10}),
        "Delete from 3 of 3 trees (11 leaves)");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 4: Delete entire tree (root → nullopt)
// ============================================================================

void test_delete_entire_tree() {
    std::cout << "Test 4: Delete entire tree..." << std::endl;

    // 5 leaves: delete singleton at position 4
    TEST_ASSERT(testDeletionInvariant(5, {4}),
        "Delete singleton tree (5 leaves)");

    // 3 leaves (0b11): tree h1[0-1], tree h0[2]
    // Delete the singleton
    TEST_ASSERT(testDeletionInvariant(3, {2}),
        "Delete singleton tree (3 leaves)");

    // 3 leaves: delete the entire h1 tree (positions 0, 1)
    TEST_ASSERT(testDeletionInvariant(3, {0, 1}),
        "Delete entire h1 tree (3 leaves)");

    // 2 leaves: delete both (entire forest)
    TEST_ASSERT(testDeletionInvariant(2, {0, 1}),
        "Delete all leaves (2 leaves)");

    // 4 leaves: delete all
    TEST_ASSERT(testDeletionInvariant(4, {0, 1, 2, 3}),
        "Delete all leaves (4 leaves)");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 5: Deletion followed by addition (full modify path)
// ============================================================================

void test_deletion_then_addition() {
    std::cout << "Test 5: Deletion followed by addition..." << std::endl;

    // Build forest with 5 leaves, delete position 1, add 2 new leaves
    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 5; ++i) {
        UtreexoHash leaf = makeLeaf(i + 500, 200 + i);
        leaves.push_back(leaf);
        forest.add(leaf);
    }

    UtreexoStump stump = UtreexoStump::fromForest(forest);

    // Prove target from original forest
    auto pos = forest.findLeafPosition(leaves[1]);
    TEST_ASSERT(pos.has_value(), "Leaf 1 should exist");
    auto proof = forest.prove(pos.value());
    TEST_ASSERT(proof.has_value(), "Should prove leaf 1");

    // Delete from forest
    forest.remove(leaves[1], proof.value());

    // Add new leaves to forest
    std::vector<UtreexoHash> additions;
    for (uint64_t i = 0; i < 2; ++i) {
        UtreexoHash leaf = makeLeaf(900 + i, 1000 + i);
        additions.push_back(leaf);
        forest.add(leaf);
    }

    // Apply same via stump.modify()
    BlockUtreexoProof bp;
    bp.targets = {leaves[1]};
    bp.positions = {pos.value()};
    bp.proof_hashes = proof.value().siblings;
    bp.numLeaves = 5;

    bool ok = stump.modify(bp, additions);
    TEST_ASSERT(ok, "modify() should succeed");
    TEST_ASSERT(stump.getCommitment() == forest.getCommitment(),
        "Commitments match after delete+add");
    TEST_ASSERT(stump.getNumLeaves() == forest.getNumLeaves(),
        "numLeaves match after delete+add");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 6: Sequential modify() calls
// ============================================================================

void test_sequential_modify() {
    std::cout << "Test 6: Sequential modify() calls..." << std::endl;

    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 10; ++i) {
        UtreexoHash leaf = makeLeaf(i + 2000, 300 + i);
        leaves.push_back(leaf);
        forest.add(leaf);
    }

    UtreexoStump stump = UtreexoStump::fromForest(forest);

    // Round 1: delete position 3, add 1 leaf
    {
        auto pos = forest.findLeafPosition(leaves[3]);
        auto proof = forest.prove(pos.value());
        forest.remove(leaves[3], proof.value());

        UtreexoHash newLeaf = makeLeaf(3000, 500);
        forest.add(newLeaf);

        BlockUtreexoProof bp;
        bp.targets = {leaves[3]};
        bp.positions = {pos.value()};
        bp.proof_hashes = proof.value().siblings;
        bp.numLeaves = stump.getNumLeaves();

        TEST_ASSERT(stump.modify(bp, {newLeaf}), "Round 1 modify succeeds");
        TEST_ASSERT(stump.getCommitment() == forest.getCommitment(),
            "Round 1 commitments match");
    }

    // Round 2: delete position 7, add 2 leaves
    {
        auto pos = forest.findLeafPosition(leaves[7]);
        auto proof = forest.prove(pos.value());
        forest.remove(leaves[7], proof.value());

        std::vector<UtreexoHash> adds;
        for (int i = 0; i < 2; ++i) {
            UtreexoHash leaf = makeLeaf(4000 + i, 600 + i);
            adds.push_back(leaf);
            forest.add(leaf);
        }

        BlockUtreexoProof bp;
        bp.targets = {leaves[7]};
        bp.positions = {pos.value()};
        bp.proof_hashes = proof.value().siblings;
        bp.numLeaves = stump.getNumLeaves();

        TEST_ASSERT(stump.modify(bp, adds), "Round 2 modify succeeds");
        TEST_ASSERT(stump.getCommitment() == forest.getCommitment(),
            "Round 2 commitments match");
    }

    // Round 3: delete position 0, no additions
    {
        auto pos = forest.findLeafPosition(leaves[0]);
        auto proof = forest.prove(pos.value());
        forest.remove(leaves[0], proof.value());

        BlockUtreexoProof bp;
        bp.targets = {leaves[0]};
        bp.positions = {pos.value()};
        bp.proof_hashes = proof.value().siblings;
        bp.numLeaves = stump.getNumLeaves();

        TEST_ASSERT(stump.modify(bp, {}), "Round 3 modify succeeds");
        TEST_ASSERT(stump.getCommitment() == forest.getCommitment(),
            "Round 3 commitments match");
    }

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 7: Full block simulation (multiple deletes + multiple adds)
// ============================================================================

void test_full_block_simulation() {
    std::cout << "Test 7: Full block simulation..." << std::endl;

    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;
    for (uint64_t i = 0; i < 20; ++i) {
        UtreexoHash leaf = makeLeaf(i + 5000, 400 + i);
        leaves.push_back(leaf);
        forest.add(leaf);
    }

    UtreexoStump stump = UtreexoStump::fromForest(forest);

    // Block: spend leaves 2, 5, 11, 17; create 6 new outputs
    std::vector<size_t> spend_indices = {2, 5, 11, 17};

    // Prove all targets from original forest
    std::vector<UtreexoHash> targets;
    std::vector<uint64_t> positions;
    std::vector<UtreexoHash> proof_hashes;

    for (size_t idx : spend_indices) {
        auto pos = forest.findLeafPosition(leaves[idx]);
        TEST_ASSERT(pos.has_value(), "Spent leaf should exist");
        auto proof = forest.prove(pos.value());
        TEST_ASSERT(proof.has_value(), "Should prove spent leaf");

        targets.push_back(leaves[idx]);
        positions.push_back(pos.value());
        proof_hashes.insert(proof_hashes.end(),
            proof.value().siblings.begin(),
            proof.value().siblings.end());
    }

    // Apply removals to forest
    for (size_t i = 0; i < targets.size(); ++i) {
        auto pos = forest.findLeafPosition(targets[i]);
        auto proof = forest.prove(pos.value());
        forest.remove(targets[i], proof.value());
    }

    // Create new outputs
    std::vector<UtreexoHash> additions;
    for (uint64_t i = 0; i < 6; ++i) {
        UtreexoHash leaf = makeLeaf(8000 + i, 700 + i);
        additions.push_back(leaf);
        forest.add(leaf);
    }

    // Apply via stump
    BlockUtreexoProof bp;
    bp.targets = targets;
    bp.positions = positions;
    bp.proof_hashes = proof_hashes;
    bp.numLeaves = 20;

    TEST_ASSERT(stump.modify(bp, additions), "Block modify succeeds");
    TEST_ASSERT(stump.getCommitment() == forest.getCommitment(),
        "Block commitments match");
    TEST_ASSERT(stump.getNumLeaves() == forest.getNumLeaves(),
        "Block numLeaves match");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Test 8: Edge cases
// ============================================================================

void test_edge_cases() {
    std::cout << "Test 8: Edge cases..." << std::endl;

    // Empty targets: modify with no deletions, no additions
    {
        UtreexoForest forest;
        forest.add(makeLeaf(1, 1));
        UtreexoStump stump = UtreexoStump::fromForest(forest);

        BlockUtreexoProof empty;
        TEST_ASSERT(stump.modify(empty, {}), "Empty modify succeeds");
        TEST_ASSERT(stump.getCommitment() == forest.getCommitment(),
            "Empty modify preserves commitment");
    }

    // Additions only (no deletions)
    {
        UtreexoForest forest;
        forest.add(makeLeaf(1, 1));
        UtreexoStump stump = UtreexoStump::fromForest(forest);

        UtreexoHash newLeaf = makeLeaf(2, 2);
        forest.add(newLeaf);

        BlockUtreexoProof empty;
        TEST_ASSERT(stump.modify(empty, {newLeaf}), "Addition-only modify succeeds");
        TEST_ASSERT(stump.getCommitment() == forest.getCommitment(),
            "Addition-only commitment matches");
    }

    // Single leaf forest: delete the only leaf
    TEST_ASSERT(testDeletionInvariant(1, {0}),
        "Delete only leaf in single-leaf forest");

    std::cout << "  PASSED" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "=========================================" << std::endl;
    std::cout << "  Utreexo Stump Deletion Test Suite" << std::endl;
    std::cout << "=========================================" << std::endl;

    test_single_deletion_various_sizes();
    test_multiple_deletions_same_tree();
    test_multiple_deletions_different_trees();
    test_delete_entire_tree();
    test_deletion_then_addition();
    test_sequential_modify();
    test_full_block_simulation();
    test_edge_cases();

    std::cout << std::endl;
    std::cout << "=========================================" << std::endl;
    std::cout << "  All " << tests_passed << "/" << tests_total << " assertions passed!" << std::endl;
    std::cout << "=========================================" << std::endl;

    return 0;
}
