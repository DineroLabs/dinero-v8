/**
 * Test Suite: Stateless Batch Proof Verification
 *
 * Tests the new position-based stateless Utreexo batch proof verification.
 * This validates the bulletproofed implementation that allows verification
 * WITHOUT requiring local UTXO database or leaf_positions_.
 *
 * Test Coverage:
 * - T_SB_1: Empty proof verification (coinbase-only)
 * - T_SB_2: Single target with position verifies correctly
 * - T_SB_3: Multiple targets with positions verify correctly
 * - T_SB_4: Position count mismatch causes failure
 * - T_SB_5: Invalid position causes failure
 * - T_SB_6: Tampered proof_hashes causes failure
 * - T_SB_7: generateBlockProof produces valid proof with positions
 * - T_SB_8: Round-trip: generate -> serialize -> deserialize -> verify
 * - T_SB_9: Cross-validate stateful vs stateless verification
 */

#include "consensus/utreexo_accumulator.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <random>

using namespace dinero::consensus;

// ═══════════════════════════════════════════════════════════════════════════
// Test Helpers
// ═══════════════════════════════════════════════════════════════════════════

// Create a deterministic test hash
UtreexoHash makeTestHash(uint8_t seed) {
    UtreexoHash hash(32);
    for (int i = 0; i < 32; i++) {
        hash[i] = static_cast<uint8_t>((seed * 7 + i * 13) % 256);
    }
    return hash;
}

// Create a forest with N leaves
UtreexoForest createForestWithLeaves(int numLeaves, std::vector<UtreexoHash>& leaves) {
    UtreexoForest forest;
    leaves.clear();

    for (int i = 0; i < numLeaves; i++) {
        UtreexoHash leaf = makeTestHash(static_cast<uint8_t>(i + 1));
        leaves.push_back(leaf);
        forest.add(leaf);
    }

    return forest;
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_1: Empty Proof Verification
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_1_empty_proof() {
    std::cout << "\n[T_SB_1] Empty proof verification (coinbase-only blocks)\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    // Empty vectors for coinbase-only block
    std::vector<UtreexoHash> targets;
    std::vector<uint64_t> positions;
    std::vector<UtreexoHash> proof_hashes;
    std::vector<UtreexoHash> expectedRoots;

    bool result = forest.verifyBatchProofStateless(
        targets, positions, proof_hashes, 0, expectedRoots);

    if (result) {
        std::cout << "✅ TEST PASSED: Empty proof accepted\n";
    } else {
        std::cerr << "❌ TEST FAILED: Empty proof should be accepted\n";
        throw std::runtime_error("T_SB_1 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_2: Single Target Verification
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_2_single_target() {
    std::cout << "\n[T_SB_2] Single target with position verifies correctly\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(4, leaves);

    std::cout << "✓ Created forest with 4 leaves\n";
    std::cout << "  Roots: " << forest.getNumRoots() << "\n";

    // Target the first leaf
    std::vector<UtreexoHash> targets = { leaves[0] };

    // Generate complete block proof with positions
    BlockUtreexoProof block_proof = forest.generateBlockProof(targets);

    std::cout << "✓ Generated proof:\n";
    std::cout << "  Targets: " << block_proof.targets.size() << "\n";
    std::cout << "  Positions: " << block_proof.positions.size() << "\n";
    std::cout << "  Proof hashes: " << block_proof.proof_hashes.size() << "\n";
    std::cout << "  numLeaves: " << block_proof.numLeaves << "\n";

    // Verify
    bool result = forest.verifyBatchProofStateless(
        block_proof.targets,
        block_proof.positions,
        block_proof.proof_hashes,
        block_proof.numLeaves,
        forest.getRoots());

    if (result) {
        std::cout << "✅ TEST PASSED: Single target verified\n";
    } else {
        std::cerr << "❌ TEST FAILED: Single target verification failed\n";
        throw std::runtime_error("T_SB_2 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_3: Multiple Targets Verification
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_3_multiple_targets() {
    std::cout << "\n[T_SB_3] Multiple targets with positions verify correctly\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(8, leaves);

    std::cout << "✓ Created forest with 8 leaves\n";

    // Target multiple leaves
    std::vector<UtreexoHash> targets = { leaves[0], leaves[3], leaves[5] };

    BlockUtreexoProof block_proof = forest.generateBlockProof(targets);

    std::cout << "✓ Generated proof for 3 targets\n";
    std::cout << "  Proof hashes: " << block_proof.proof_hashes.size() << " (deduplicated)\n";

    bool result = forest.verifyBatchProofStateless(
        block_proof.targets,
        block_proof.positions,
        block_proof.proof_hashes,
        block_proof.numLeaves,
        forest.getRoots());

    if (result) {
        std::cout << "✅ TEST PASSED: Multiple targets verified\n";
    } else {
        std::cerr << "❌ TEST FAILED: Multiple targets verification failed\n";
        throw std::runtime_error("T_SB_3 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_4: Position Count Mismatch
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_4_position_count_mismatch() {
    std::cout << "\n[T_SB_4] Position count mismatch causes failure\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(4, leaves);

    // Create mismatched targets and positions
    std::vector<UtreexoHash> targets = { leaves[0], leaves[1] };
    std::vector<uint64_t> positions = { 0 };  // Only 1 position for 2 targets!
    std::vector<UtreexoHash> proof_hashes;

    bool result = forest.verifyBatchProofStateless(
        targets, positions, proof_hashes, 4, forest.getRoots());

    if (!result) {
        std::cout << "✅ TEST PASSED: Position mismatch correctly rejected\n";
    } else {
        std::cerr << "❌ TEST FAILED: Position mismatch should fail\n";
        throw std::runtime_error("T_SB_4 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_5: Invalid Position
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_5_invalid_position() {
    std::cout << "\n[T_SB_5] Invalid position causes failure\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(4, leaves);

    std::vector<UtreexoHash> targets = { leaves[0] };
    std::vector<uint64_t> positions = { 100 };  // Invalid: >= numLeaves
    std::vector<UtreexoHash> proof_hashes;

    bool result = forest.verifyBatchProofStateless(
        targets, positions, proof_hashes, 4, forest.getRoots());

    if (!result) {
        std::cout << "✅ TEST PASSED: Invalid position correctly rejected\n";
    } else {
        std::cerr << "❌ TEST FAILED: Invalid position should fail\n";
        throw std::runtime_error("T_SB_5 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_6: Tampered Proof Hashes
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_6_tampered_proof_hashes() {
    std::cout << "\n[T_SB_6] Tampered proof_hashes causes failure\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(4, leaves);

    std::vector<UtreexoHash> targets = { leaves[0] };
    BlockUtreexoProof block_proof = forest.generateBlockProof(targets);

    // Tamper with proof_hashes
    if (!block_proof.proof_hashes.empty()) {
        block_proof.proof_hashes[0][0] ^= 0xFF;  // Flip bits
    }

    bool result = forest.verifyBatchProofStateless(
        block_proof.targets,
        block_proof.positions,
        block_proof.proof_hashes,
        block_proof.numLeaves,
        forest.getRoots());

    if (!result) {
        std::cout << "✅ TEST PASSED: Tampered proof correctly rejected\n";
    } else {
        std::cerr << "❌ TEST FAILED: Tampered proof should fail\n";
        throw std::runtime_error("T_SB_6 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_7: generateBlockProof Produces Valid Proof
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_7_generate_block_proof() {
    std::cout << "\n[T_SB_7] generateBlockProof produces valid proof with positions\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(16, leaves);

    std::cout << "✓ Created forest with 16 leaves\n";

    // Test with various target sets
    std::vector<UtreexoHash> targets = { leaves[2], leaves[7], leaves[11], leaves[14] };

    BlockUtreexoProof proof = forest.generateBlockProof(targets);

    // Validate proof structure
    assert(proof.targets.size() == targets.size());
    assert(proof.positions.size() == targets.size());
    assert(proof.numLeaves == 16);
    assert(proof.isValid());  // positions.size() == targets.size()

    std::cout << "✓ Proof structure validated:\n";
    std::cout << "  targets.size() = positions.size() = " << proof.targets.size() << "\n";
    std::cout << "  numLeaves = " << proof.numLeaves << "\n";
    std::cout << "  proof_hashes = " << proof.proof_hashes.size() << "\n";

    std::cout << "✅ TEST PASSED: generateBlockProof produces valid proof\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_8: Round-Trip Serialization
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_8_round_trip_serialization() {
    std::cout << "\n[T_SB_8] Round-trip: generate -> serialize -> deserialize -> verify\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(8, leaves);

    std::vector<UtreexoHash> targets = { leaves[1], leaves[5] };

    // Generate proof
    BlockUtreexoProof original = forest.generateBlockProof(targets);

    // Serialize
    std::vector<uint8_t> serialized = original.serialize();
    std::cout << "✓ Serialized proof: " << serialized.size() << " bytes\n";

    // Deserialize
    BlockUtreexoProof restored = BlockUtreexoProof::deserialize(serialized);

    // Validate restoration
    assert(restored.targets.size() == original.targets.size());
    assert(restored.positions.size() == original.positions.size());
    assert(restored.proof_hashes.size() == original.proof_hashes.size());
    assert(restored.numLeaves == original.numLeaves);

    // Verify positions match
    for (size_t i = 0; i < original.positions.size(); i++) {
        assert(restored.positions[i] == original.positions[i]);
    }

    std::cout << "✓ Deserialized proof matches original\n";

    // Verify restored proof
    bool result = forest.verifyBatchProofStateless(
        restored.targets,
        restored.positions,
        restored.proof_hashes,
        restored.numLeaves,
        forest.getRoots());

    if (result) {
        std::cout << "✅ TEST PASSED: Round-trip serialization works\n";
    } else {
        std::cerr << "❌ TEST FAILED: Restored proof failed verification\n";
        throw std::runtime_error("T_SB_8 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// T_SB_9: Cross-Validate Stateful vs Stateless
// ═══════════════════════════════════════════════════════════════════════════

void test_SB_9_cross_validate() {
    std::cout << "\n[T_SB_9] Cross-validate stateful vs stateless verification\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    std::vector<UtreexoHash> leaves;
    UtreexoForest forest = createForestWithLeaves(8, leaves);

    std::vector<UtreexoHash> targets = { leaves[0], leaves[4] };

    // Generate old-style proof (just proof_hashes)
    std::vector<UtreexoHash> old_proof = forest.generateBatchProof(targets);

    // Generate new-style proof (with positions)
    BlockUtreexoProof new_proof = forest.generateBlockProof(targets);

    // Verify both
    bool stateful_result = forest.verifyBatchProof(targets, old_proof);

    bool stateless_result = forest.verifyBatchProofStateless(
        new_proof.targets,
        new_proof.positions,
        new_proof.proof_hashes,
        new_proof.numLeaves,
        forest.getRoots());

    std::cout << "  Stateful verification: " << (stateful_result ? "PASS" : "FAIL") << "\n";
    std::cout << "  Stateless verification: " << (stateless_result ? "PASS" : "FAIL") << "\n";

    if (stateful_result && stateless_result) {
        std::cout << "✅ TEST PASSED: Both verification modes agree\n";
    } else {
        std::cerr << "❌ TEST FAILED: Verification modes disagree\n";
        throw std::runtime_error("T_SB_9 failed");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Main Test Runner
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << "  Stateless Batch Proof Verification Tests\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n";

    int passed = 0;
    int failed = 0;

    auto runTest = [&](auto testFn, const char* name) {
        try {
            testFn();
            passed++;
        } catch (const std::exception& e) {
            std::cerr << name << " FAILED: " << e.what() << "\n";
            failed++;
        }
    };

    runTest(test_SB_1_empty_proof, "T_SB_1");
    runTest(test_SB_2_single_target, "T_SB_2");
    runTest(test_SB_3_multiple_targets, "T_SB_3");
    runTest(test_SB_4_position_count_mismatch, "T_SB_4");
    runTest(test_SB_5_invalid_position, "T_SB_5");
    runTest(test_SB_6_tampered_proof_hashes, "T_SB_6");
    runTest(test_SB_7_generate_block_proof, "T_SB_7");
    runTest(test_SB_8_round_trip_serialization, "T_SB_8");
    runTest(test_SB_9_cross_validate, "T_SB_9");

    std::cout << "\n═══════════════════════════════════════════════════════════════════\n";
    std::cout << "  Test Summary\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n";
    std::cout << "  Passed: " << passed << "\n";
    std::cout << "  Failed: " << failed << "\n";

    if (failed == 0) {
        std::cout << "\n✅ All stateless batch proof tests PASSED\n";
        return 0;
    } else {
        std::cerr << "\n❌ Some tests FAILED\n";
        return 1;
    }
}
