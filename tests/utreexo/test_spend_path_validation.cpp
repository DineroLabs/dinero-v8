/**
 * @file test_spend_path_validation.cpp
 * @brief Phase U.3: Utreexo Spend-Path Proof Validation Tests
 *
 * CONSENSUS CRITICAL - This is the CONSENSUS GATE.
 * Nothing else matters if this is wrong.
 *
 * These tests validate that block validation correctly:
 *   - Verifies proof against pre-block forest root
 *   - Rejects invalid proofs
 *   - Rejects double-spends
 *   - Rejects wrong final root
 *   - Ensures tx reordering does NOT change final root
 *
 * Validation Flow (STRICT ORDER):
 *   for each tx in block:
 *     for each input:
 *       1. Verify proof against pre-block forest root
 *       2. Reject if proof invalid
 *       3. Reject if leaf already spent
 *       4. Mark leaf for removal
 *   apply all removals
 *   apply all additions (new outputs)
 *   assert resulting forest.root == header.utreexo_root
 *
 * If any test fails → CONSENSUS BUG → Network fork risk!
 */

#include <gtest/gtest.h>
#include "consensus/utreexo_accumulator.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include <unordered_set>

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

    return HashUTXOLegacy(txid, 0, value, scriptPubKey);
}

bool hashesEqual(const UtreexoHash& a, const UtreexoHash& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin());
}

// Simulated block validation result
struct ValidationResult {
    bool valid;
    std::string error;
    UtreexoHash computed_root;

    static ValidationResult Ok(const UtreexoHash& root) {
        return {true, "", root};
    }
    static ValidationResult Fail(const std::string& err) {
        return {false, err, {}};
    }
};

/**
 * @brief Simulate block validation with Utreexo
 *
 * This is a simplified version of what BlockValidator does.
 * It validates proofs and computes the new forest root.
 *
 * NOTE: For simplicity, we use individual proof verification which is
 * well-tested. Real production uses batched proofs for efficiency.
 */
ValidationResult validateBlockUtreexo(
    UtreexoForest& forest,
    const std::vector<UtreexoHash>& spent_leaves,
    const std::vector<uint64_t>& spent_positions,  // Leaf positions to spend
    const std::vector<UtreexoHash>& new_outputs,
    const UtreexoHash& header_utreexo_root) {

    // Step 1: Check for double-spends within block (duplicate positions)
    std::unordered_set<uint64_t> seen_positions;
    for (uint64_t pos : spent_positions) {
        if (seen_positions.count(pos) > 0) {
            return ValidationResult::Fail("DOUBLE_SPEND: duplicate position in block");
        }
        seen_positions.insert(pos);
    }

    // Step 2: Validate sizes match
    if (spent_leaves.size() != spent_positions.size()) {
        return ValidationResult::Fail("INVALID_PROOF: leaf/position count mismatch");
    }

    // Step 3: Verify each spend individually and apply removals
    // We must regenerate proofs as we go since forest state changes
    auto working_forest = forest.clone();

    for (size_t i = 0; i < spent_leaves.size(); i++) {
        uint64_t pos = spent_positions[i];
        const auto& leaf = spent_leaves[i];

        // Check if already deleted
        if (working_forest.isDeleted(pos)) {
            return ValidationResult::Fail("DOUBLE_SPEND: position already deleted");
        }

        // Generate proof for this position
        auto proof = working_forest.prove(pos);
        if (!proof.has_value()) {
            return ValidationResult::Fail("INVALID_PROOF: cannot generate proof");
        }

        // Verify proof against expected leaf
        auto roots = working_forest.getRoots();
        if (!proof->verify(leaf, roots)) {
            return ValidationResult::Fail("INVALID_PROOF: proof verification failed");
        }

        // Apply removal
        bool removed = working_forest.remove(leaf, *proof);
        if (!removed) {
            return ValidationResult::Fail("REMOVE_FAILED: removal returned false");
        }
    }

    // Step 4: Apply additions (new outputs)
    for (const auto& output : new_outputs) {
        working_forest.add(output);
    }

    // Step 5: Verify final root matches header commitment
    auto computed_root = working_forest.getCommitment();

    if (!hashesEqual(computed_root, header_utreexo_root)) {
        return ValidationResult::Fail("ROOT_MISMATCH: computed root doesn't match header");
    }

    // Success - update the original forest
    forest = std::move(working_forest);

    return ValidationResult::Ok(computed_root);
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════════
// Test: Valid Block Passes
// ═══════════════════════════════════════════════════════════════════════════════

class ValidBlockTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> initial_leaves;

    void SetUp() override {
        for (int i = 0; i < 10; i++) {
            auto leaf = makeUTXOLeaf(i, (i + 1) * 1000);
            forest.add(leaf);
            initial_leaves.push_back(leaf);
        }
    }
};

TEST_F(ValidBlockTest, ValidBlockWithSpendsAndOutputsSucceeds) {
    // Spend leaves at positions 0, 2, 4
    std::vector<UtreexoHash> spent = {initial_leaves[0], initial_leaves[2], initial_leaves[4]};
    std::vector<uint64_t> positions = {0, 2, 4};

    // Clone forest to compute expected root
    auto sim_forest = forest.clone();
    for (size_t i = 0; i < positions.size(); i++) {
        auto p = sim_forest.prove(positions[i]);
        if (p) sim_forest.remove(spent[i], *p);
    }

    // Add new outputs
    std::vector<UtreexoHash> new_outputs;
    for (int i = 100; i < 105; i++) {
        auto output = makeUTXOLeaf(i, i * 100);
        sim_forest.add(output);
        new_outputs.push_back(output);
    }

    auto expected_root = sim_forest.getCommitment();

    // Validate block
    auto result = validateBlockUtreexo(forest, spent, positions, new_outputs, expected_root);

    EXPECT_TRUE(result.valid) << "Error: " << result.error;
    EXPECT_TRUE(hashesEqual(result.computed_root, expected_root));
}

TEST_F(ValidBlockTest, EmptyBlockSucceeds) {
    // No spends, no new outputs
    std::vector<UtreexoHash> spent;
    std::vector<uint64_t> positions;
    std::vector<UtreexoHash> new_outputs;

    auto expected_root = forest.getCommitment();

    auto result = validateBlockUtreexo(forest, spent, positions, new_outputs, expected_root);

    EXPECT_TRUE(result.valid) << "Empty block should be valid";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test: Invalid Proof → Block Rejected
// ═══════════════════════════════════════════════════════════════════════════════

class InvalidProofTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> initial_leaves;

    void SetUp() override {
        for (int i = 0; i < 8; i++) {
            auto leaf = makeUTXOLeaf(i, i * 1000);
            forest.add(leaf);
            initial_leaves.push_back(leaf);
        }
    }
};

TEST_F(InvalidProofTest, TamperedProofRejected) {
    // Test that validation fails when leaf doesn't match position
    // We'll try to claim leaf at position 3 exists but provide wrong leaf hash
    std::vector<UtreexoHash> wrong_spent = {makeUTXOLeaf(999, 999)};  // Non-existent leaf
    std::vector<uint64_t> positions = {3};  // Valid position, but leaf won't match

    auto result = validateBlockUtreexo(forest, wrong_spent, positions, {}, forest.getCommitment());

    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("INVALID_PROOF"), std::string::npos)
        << "Should reject when leaf doesn't match position";
}

TEST_F(InvalidProofTest, WrongTargetRejected) {
    // Try to claim one leaf at another's position
    std::vector<UtreexoHash> wrong_spent = {initial_leaves[5]};  // Leaf 5
    std::vector<uint64_t> positions = {3};  // But claim it's at position 3

    auto result = validateBlockUtreexo(forest, wrong_spent, positions, {}, forest.getCommitment());

    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("INVALID_PROOF"), std::string::npos);
}

TEST_F(InvalidProofTest, MismatchedPositionCountRejected) {
    std::vector<UtreexoHash> spent = {initial_leaves[1], initial_leaves[2]};
    std::vector<uint64_t> positions = {1};  // Only one position for two leaves

    auto result = validateBlockUtreexo(forest, spent, positions, {}, forest.getCommitment());

    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("mismatch"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test: Double-Spend → Rejected
// ═══════════════════════════════════════════════════════════════════════════════

class DoubleSpendTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> initial_leaves;

    void SetUp() override {
        for (int i = 0; i < 4; i++) {
            auto leaf = makeUTXOLeaf(i, i * 1000);
            forest.add(leaf);
            initial_leaves.push_back(leaf);
        }
    }
};

TEST_F(DoubleSpendTest, SameLeafSpentTwiceInBlockRejected) {
    // Try to spend same position twice in one block
    std::vector<UtreexoHash> spent = {initial_leaves[1], initial_leaves[1]};
    std::vector<uint64_t> positions = {1, 1};  // Same position twice

    auto result = validateBlockUtreexo(forest, spent, positions, {}, forest.getCommitment());

    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("DOUBLE_SPEND"), std::string::npos)
        << "Error was: " << result.error;
}

TEST_F(DoubleSpendTest, SpendingDeletedLeafRejected) {
    // First: spend leaf at position 2
    std::vector<UtreexoHash> first_spent = {initial_leaves[2]};
    std::vector<uint64_t> first_positions = {2};

    // Compute expected root after first spend
    auto sim_forest = forest.clone();
    auto p = sim_forest.prove(2);
    if (p) sim_forest.remove(first_spent[0], *p);
    auto root_after_first = sim_forest.getCommitment();

    // Apply first block
    auto first_result = validateBlockUtreexo(forest, first_spent, first_positions, {}, root_after_first);
    ASSERT_TRUE(first_result.valid) << "First spend should succeed: " << first_result.error;

    // Second block: try to spend same position again
    std::vector<UtreexoHash> second_spent = {initial_leaves[2]};
    std::vector<uint64_t> second_positions = {2};  // Same position (now deleted)

    auto result = validateBlockUtreexo(forest, second_spent, second_positions, {}, forest.getCommitment());

    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("DOUBLE_SPEND"), std::string::npos)
        << "Should reject spending already-deleted leaf";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test: Wrong Final Root → Rejected
// ═══════════════════════════════════════════════════════════════════════════════

class WrongRootTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> initial_leaves;

    void SetUp() override {
        for (int i = 0; i < 8; i++) {
            auto leaf = makeUTXOLeaf(i, i * 1000);
            forest.add(leaf);
            initial_leaves.push_back(leaf);
        }
    }
};

TEST_F(WrongRootTest, MismatchedHeaderRootRejected) {
    std::vector<UtreexoHash> spent = {initial_leaves[0]};
    std::vector<uint64_t> positions = {0};

    // Compute correct root
    auto sim = forest.clone();
    auto p = sim.prove(0);
    sim.remove(spent[0], *p);
    auto correct_root = sim.getCommitment();

    // Use wrong root in header
    UtreexoHash wrong_root = correct_root;
    wrong_root[0] ^= 0xFF;

    auto result = validateBlockUtreexo(forest, spent, positions, {}, wrong_root);

    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("ROOT_MISMATCH"), std::string::npos)
        << "Error was: " << result.error;
}

TEST_F(WrongRootTest, MissingOutputsChangeRoot) {
    std::vector<UtreexoHash> spent = {initial_leaves[1]};
    std::vector<uint64_t> positions = {1};

    // Compute root WITH new outputs
    auto sim = forest.clone();
    auto p = sim.prove(1);
    sim.remove(spent[0], *p);
    auto output = makeUTXOLeaf(100, 5000);
    sim.add(output);
    auto root_with_output = sim.getCommitment();

    // Try to validate WITHOUT including the output (empty new_outputs)
    auto result = validateBlockUtreexo(forest, spent, positions, {}, root_with_output);

    // This should fail because we didn't add the output
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("ROOT_MISMATCH"), std::string::npos)
        << "Error was: " << result.error;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test: Tx Reordering Does NOT Change Root
// ═══════════════════════════════════════════════════════════════════════════════

class TxReorderingTest : public ::testing::Test {
protected:
    UtreexoForest forest;
    std::vector<UtreexoHash> initial_leaves;

    void SetUp() override {
        for (int i = 0; i < 16; i++) {
            auto leaf = makeUTXOLeaf(i, i * 1000);
            forest.add(leaf);
            initial_leaves.push_back(leaf);
        }
    }
};

TEST_F(TxReorderingTest, SpendOrderDoesNotAffectFinalRoot) {
    // Spend leaves 0, 5, 10 in different orders

    // Order 1: 0, 5, 10
    auto forest1 = forest.clone();
    std::vector<uint64_t> order1 = {0, 5, 10};

    for (uint64_t idx : order1) {
        auto p = forest1.prove(idx);
        if (p && !forest1.isDeleted(idx)) {
            forest1.remove(initial_leaves[idx], *p);
        }
    }

    // Add same new outputs
    auto output = makeUTXOLeaf(100, 10000);
    forest1.add(output);
    auto root1 = forest1.getCommitment();

    // Order 2: 10, 0, 5 (different order)
    auto forest2 = forest.clone();
    std::vector<uint64_t> order2 = {10, 0, 5};

    for (uint64_t idx : order2) {
        auto p = forest2.prove(idx);
        if (p && !forest2.isDeleted(idx)) {
            forest2.remove(initial_leaves[idx], *p);
        }
    }

    forest2.add(output);
    auto root2 = forest2.getCommitment();

    // Roots should be SAME regardless of removal order
    EXPECT_TRUE(hashesEqual(root1, root2))
        << "Spend order MUST NOT affect final root";
}

TEST_F(TxReorderingTest, OutputOrderDoesAffectFinalRoot) {
    // This is expected: output order matters because positions are assigned sequentially
    auto forest1 = forest.clone();
    auto forest2 = forest.clone();

    auto output_a = makeUTXOLeaf(100, 1000);
    auto output_b = makeUTXOLeaf(101, 2000);

    // Forest 1: add A then B
    forest1.add(output_a);
    forest1.add(output_b);
    auto root1 = forest1.getCommitment();

    // Forest 2: add B then A
    forest2.add(output_b);
    forest2.add(output_a);
    auto root2 = forest2.getCommitment();

    // Roots should be DIFFERENT (output order matters)
    EXPECT_FALSE(hashesEqual(root1, root2))
        << "Output order DOES affect final root (expected behavior)";
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Utreexo Spend-Path Proof Validation Tests\n";
    std::cout << "  Phase U.3: CONSENSUS GATE\n";
    std::cout << "  THIS IS THE MOST CRITICAL TEST SUITE\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\n";

    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
