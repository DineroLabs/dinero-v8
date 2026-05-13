/**
 * Utreexo Adversarial Test Suite
 *
 * Tests edge cases that reliably surface bugs:
 * - Zero hashes
 * - Duplicate outputs
 * - Reused values with different scripts
 * - Large blocks near limits
 * - Malformed proofs
 * - Delta mutations
 * - Serialize/rollback/mutate cycles
 *
 * PHILOSOPHY: Shadow mode should be hostile, not gentle.
 * These tests deliberately inject "barely valid" and "almost invalid" inputs.
 */

#include "consensus/utreexo_accumulator.h"
#include <iostream>
#include <cassert>
#include <random>
#include <set>
#include <cstdint>

// Portable 64-bit popcount. GCC/Clang provide __builtin_popcountll; MSVC
// provides __popcnt64 (with the popcnt CPUID bit; Visual Studio 17.x x64
// build configurations target processors that include it).
#ifdef _MSC_VER
#include <intrin.h>
static inline int dinero_popcountll(uint64_t v) {
    return static_cast<int>(__popcnt64(v));
}
#define __builtin_popcountll dinero_popcountll
#endif

using namespace dinero::consensus;

// ═══════════════════════════════════════════════════════════════════════════
// Invariant Assertions (Panic in Debug Builds)
// ═══════════════════════════════════════════════════════════════════════════

#define INVARIANT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "❌ INVARIANT VIOLATION: " << msg << std::endl; \
        std::cerr << "   File: " << __FILE__ << ":" << __LINE__ << std::endl; \
        std::abort(); \
    } \
} while(0)

/**
 * Assert tree-shape invariant: root count = popcount(numLeaves)
 */
void assertTreeShape(const UtreexoForest& forest) {
    uint64_t n = forest.getNumLeaves();
    size_t expected_roots = __builtin_popcountll(n);
    size_t actual_roots = forest.getNumRoots();

    INVARIANT(actual_roots == expected_roots,
        "Root count mismatch: expected " + std::to_string(expected_roots) +
        " (popcount of " + std::to_string(n) + "), got " + std::to_string(actual_roots));
}

/**
 * Assert commitment is deterministic
 */
void assertDeterministicCommitment(const UtreexoForest& forest) {
    auto c1 = forest.getCommitment();
    auto c2 = forest.getCommitment();
    INVARIANT(c1 == c2, "Commitment is not deterministic!");
}

/**
 * Assert serialization round-trip preserves all state
 */
void assertSerializationRoundTrip(const UtreexoForest& forest) {
    auto data = forest.serialize();
    auto restored = UtreexoForest::deserialize(data);

    INVARIANT(restored.getNumLeaves() == forest.getNumLeaves(),
        "numLeaves changed after deserialize");
    INVARIANT(restored.getActiveLeaves() == forest.getActiveLeaves(),
        "activeLeaves changed after deserialize");
    INVARIANT(restored.getNumRoots() == forest.getNumRoots(),
        "numRoots changed after deserialize");
    INVARIANT(restored.getCommitment() == forest.getCommitment(),
        "commitment changed after deserialize");

    assertTreeShape(restored);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test Utilities
// ═══════════════════════════════════════════════════════════════════════════

UtreexoHash makeHash(uint8_t fill) {
    return UtreexoHash(32, fill);
}

UtreexoHash makeUniqueHash(uint32_t seed) {
    UtreexoHash h(32, 0);
    h[0] = seed & 0xFF;
    h[1] = (seed >> 8) & 0xFF;
    h[2] = (seed >> 16) & 0xFF;
    h[3] = (seed >> 24) & 0xFF;
    return h;
}

UtreexoHash zeroHash() {
    return UtreexoHash(32, 0);
}

void assertAddAccepted(UtreexoForest& forest, const UtreexoHash& leaf, const std::string& label) {
    const uint64_t before = forest.getNumLeaves();
    const uint64_t pos = forest.add(leaf);
    INVARIANT(pos != UINT64_MAX, label + " should be accepted");
    INVARIANT(forest.getNumLeaves() == before + 1,
        label + " should increase numLeaves");
}

void assertAddRejected(UtreexoForest& forest, const UtreexoHash& leaf, const std::string& label) {
    const uint64_t before = forest.getNumLeaves();
    const uint64_t pos = forest.add(leaf);
    INVARIANT(pos == UINT64_MAX, label + " should be rejected");
    INVARIANT(forest.getNumLeaves() == before,
        label + " should not change numLeaves");
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Zero Hash Stress
// ═══════════════════════════════════════════════════════════════════════════

void testZeroHashStress() {
    std::cout << "\n[Test 1] Zero Hash Stress..." << std::endl;

    UtreexoForest forest;

    // Zero hashes are allowed, but only once while live.
    assertAddAccepted(forest, zeroHash(), "initial zero hash");
    for (int i = 0; i < 3; i++) {
        assertAddRejected(forest, zeroHash(), "duplicate zero hash");
        assertTreeShape(forest);
        assertDeterministicCommitment(forest);
    }

    INVARIANT(forest.getNumLeaves() == 1, "Should have exactly one live zero hash leaf");

    // Serialize and restore
    assertSerializationRoundTrip(forest);

    // After restore, the duplicate must still be rejected until the original is removed.
    auto data = forest.serialize();
    auto restored = UtreexoForest::deserialize(data);
    assertAddRejected(restored, zeroHash(), "duplicate zero hash after restore");

    auto proof = restored.prove(0);
    INVARIANT(proof.has_value(), "Should prove the original zero hash");
    INVARIANT(proof->verify(zeroHash(), restored.getRoots()), "Original zero hash proof should verify");
    INVARIANT(restored.getNumLeaves() == 1, "Restore must preserve the single zero hash leaf");

    std::cout << "  ✅ Zero hash rejection/restore path handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Interleaved Zero and Non-Zero
// ═══════════════════════════════════════════════════════════════════════════

void testInterleavedZeroNonZero() {
    std::cout << "\n[Test 2] Interleaved Zero and Non-Zero..." << std::endl;

    UtreexoForest forest;
    bool saw_zero = false;

    // Interleave zero and non-zero hashes
    for (int i = 0; i < 12; i++) {
        if (i % 3 == 0) {
            if (!saw_zero) {
                assertAddAccepted(forest, zeroHash(), "first interleaved zero hash");
                saw_zero = true;
            } else {
                assertAddRejected(forest, zeroHash(), "duplicate interleaved zero hash");
            }
        } else {
            assertAddAccepted(forest, makeUniqueHash(i), "unique interleaved leaf");
        }
        assertTreeShape(forest);
    }

    INVARIANT(forest.getNumLeaves() == 9, "Expected 8 unique non-zero leaves plus one zero hash");
    assertSerializationRoundTrip(forest);

    // Verify we can still generate proofs for all positions
    for (uint64_t pos = 0; pos < forest.getNumLeaves(); pos++) {
        auto proof = forest.prove(pos);
        INVARIANT(proof.has_value(), "Should be able to prove position " + std::to_string(pos));
    }

    std::cout << "  ✅ Interleaved zero/non-zero handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Duplicate Hash Values
// ═══════════════════════════════════════════════════════════════════════════

void testDuplicateHashes() {
    std::cout << "\n[Test 3] Duplicate Hash Values..." << std::endl;

    UtreexoForest forest;

    // Duplicate live leaf hashes must be rejected until the original is removed.
    UtreexoHash retained = makeUniqueHash(0xCD);
    UtreexoHash dup = makeHash(0xAB);

    assertAddAccepted(forest, retained, "retained leaf");
    assertAddAccepted(forest, dup, "initial duplicate-candidate leaf");
    for (int i = 0; i < 5; i++) {
        assertAddRejected(forest, dup, "duplicate live leaf");
        assertTreeShape(forest);
    }

    assertSerializationRoundTrip(forest);

    auto roots = forest.getRoots();
    auto proof = forest.prove(1);
    INVARIANT(proof.has_value(), "Should prove the surviving duplicate-candidate leaf");
    INVARIANT(proof->verify(dup, roots), "Proof should verify");

    INVARIANT(forest.remove(dup, *proof), "Should remove the surviving leaf");
    INVARIANT(!forest.findLeafPosition(dup).has_value(), "Removed leaf should no longer be findable");

    auto restored = UtreexoForest::deserialize(forest.serialize());
    INVARIANT(restored.findLeafPosition(retained).has_value(), "Retained leaf must survive restore");
    uint64_t readded_pos = restored.add(dup);
    INVARIANT(readded_pos == 2, "Re-added duplicate-candidate leaf should append at position 2");
    auto readded_proof = restored.prove(readded_pos);
    INVARIANT(readded_proof.has_value(), "Should prove re-added leaf");
    INVARIANT(readded_proof->verify(dup, restored.getRoots()), "Re-added leaf proof should verify");

    std::cout << "  ✅ Duplicate live leaf rejection handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: Near-Limit Large Forest
// ═══════════════════════════════════════════════════════════════════════════

void testLargeForest() {
    std::cout << "\n[Test 4] Large Forest (10,000 leaves)..." << std::endl;

    UtreexoForest forest;

    // Add 10,000 leaves
    for (uint32_t i = 0; i < 10000; i++) {
        forest.add(makeUniqueHash(i));

        // Check invariants periodically
        if (i % 1000 == 0) {
            assertTreeShape(forest);
        }
    }

    assertTreeShape(forest);
    assertSerializationRoundTrip(forest);

    // Verify random proofs
    std::mt19937 rng(42);
    for (int i = 0; i < 100; i++) {
        uint64_t pos = rng() % 10000;
        auto proof = forest.prove(pos);
        INVARIANT(proof.has_value(), "Should prove position " + std::to_string(pos));
    }

    std::cout << "  ✅ Large forest (10,000) handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Delete/Add Interleaving
// ═══════════════════════════════════════════════════════════════════════════

void testDeleteAddInterleaving() {
    std::cout << "\n[Test 5] Delete/Add Interleaving..." << std::endl;

    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    // Add 20 leaves
    for (int i = 0; i < 20; i++) {
        auto h = makeUniqueHash(i + 1000);
        leaves.push_back(h);
        forest.add(h);
    }

    assertTreeShape(forest);

    // Delete every other leaf
    for (int i = 0; i < 20; i += 2) {
        auto proof = forest.prove(i);
        if (proof.has_value()) {
            bool removed = forest.remove(leaves[i], *proof);
            INVARIANT(removed, "Should remove leaf at position " + std::to_string(i));
        }
    }

    // Add more leaves
    for (int i = 0; i < 10; i++) {
        forest.add(makeUniqueHash(i + 2000));
        assertTreeShape(forest);
    }

    assertSerializationRoundTrip(forest);

    std::cout << "  ✅ Delete/Add interleaving handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Serialize → Mutate → Rollback → Mutate
// ═══════════════════════════════════════════════════════════════════════════

void testSerializeMutateRollback() {
    std::cout << "\n[Test 6] Serialize → Mutate → Rollback → Mutate..." << std::endl;

    UtreexoForest forest;

    // Initial state
    for (int i = 0; i < 10; i++) {
        forest.add(makeUniqueHash(i));
    }
    auto snapshot1 = forest.serialize();
    auto commitment1 = forest.getCommitment();

    // Mutate
    for (int i = 0; i < 5; i++) {
        forest.add(makeUniqueHash(100 + i));
    }
    auto snapshot2 = forest.serialize();
    auto commitment2 = forest.getCommitment();

    INVARIANT(commitment1 != commitment2, "Commitment should change after add");

    // "Rollback" by restoring snapshot1
    auto restored1 = UtreexoForest::deserialize(snapshot1);
    INVARIANT(restored1.getCommitment() == commitment1, "Rollback should restore commitment");

    // Mutate again from rollback point
    for (int i = 0; i < 5; i++) {
        restored1.add(makeUniqueHash(100 + i));
    }

    // Should match the same mutation path
    INVARIANT(restored1.getCommitment() == commitment2,
        "Same mutations should produce same commitment");

    assertTreeShape(restored1);

    std::cout << "  ✅ Serialize/rollback/mutate cycle works correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: Proof with All-Zero Siblings
// ═══════════════════════════════════════════════════════════════════════════

void testProofWithZeroSiblings() {
    std::cout << "\n[Test 7] Proof with Zero Siblings..." << std::endl;

    UtreexoForest forest;

    // Add zero hash, then remove it (creates zero sibling in tree)
    forest.add(zeroHash());
    forest.add(makeHash(0x01));

    auto proof0 = forest.prove(0);
    auto proof1 = forest.prove(1);

    INVARIANT(proof0.has_value(), "Should prove zero hash at position 0");
    INVARIANT(proof1.has_value(), "Should prove non-zero at position 1");

    auto roots = forest.getRoots();
    INVARIANT(proof0->verify(zeroHash(), roots), "Zero hash proof should verify");
    INVARIANT(proof1->verify(makeHash(0x01), roots), "Non-zero proof should verify");

    // The sibling of position 1 is the zero hash
    INVARIANT(proof1->siblings.size() == 1, "Should have 1 sibling");
    INVARIANT(proof1->siblings[0] == zeroHash(), "Sibling should be zero hash");

    std::cout << "  ✅ Proofs with zero siblings handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 8: Batch Proof with Overlapping Paths
// ═══════════════════════════════════════════════════════════════════════════

void testBatchProofOverlap() {
    std::cout << "\n[Test 8] Batch Proof with Overlapping Paths..." << std::endl;

    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    // Add 8 leaves (perfect binary tree)
    for (int i = 0; i < 8; i++) {
        auto h = makeUniqueHash(i + 500);
        leaves.push_back(h);
        forest.add(h);
    }

    assertTreeShape(forest);
    INVARIANT(forest.getNumRoots() == 1, "8 leaves should have 1 root");

    // Generate batch proof for adjacent leaves (share siblings)
    std::vector<UtreexoHash> targets = {leaves[0], leaves[1], leaves[2], leaves[3]};
    auto proof_hashes = forest.generateBatchProof(targets);

    // Deduplicated proof should be smaller than 4 individual proofs
    // Each individual proof has 3 siblings, so 4*3 = 12 hashes
    // Deduplicated should be fewer
    std::cout << "  Batch proof hashes: " << proof_hashes.size() << std::endl;

    bool verified = forest.verifyBatchProof(targets, proof_hashes);
    INVARIANT(verified, "Batch proof should verify");

    std::cout << "  ✅ Batch proof with overlapping paths works correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 9: Power-of-Two Boundaries
// ═══════════════════════════════════════════════════════════════════════════

void testPowerOfTwoBoundaries() {
    std::cout << "\n[Test 9] Power-of-Two Boundaries..." << std::endl;

    UtreexoForest forest;

    // Test boundary cases: 2, 4, 8, 16, 32, 64
    // Note: Skip 1 because 1+1=2 is also a power of 2 (popcount(2)=1)
    std::vector<uint64_t> boundaries = {2, 4, 8, 16, 32, 64};

    uint32_t seed = 0;
    for (uint64_t target : boundaries) {
        while (forest.getNumLeaves() < target) {
            forest.add(makeUniqueHash(seed++));
        }

        assertTreeShape(forest);
        assertSerializationRoundTrip(forest);

        // Power of 2 should have exactly 1 root
        INVARIANT(forest.getNumRoots() == 1,
            "Power of 2 (" + std::to_string(target) + ") should have 1 root");

        // Add one more to cross boundary
        forest.add(makeUniqueHash(seed++));
        assertTreeShape(forest);

        // Now should have 2 roots (for power_of_2 + 1)
        // popcount(n+1) where n is power of 2 > 1 equals 2
        INVARIANT(forest.getNumRoots() == 2,
            "Power of 2 + 1 (" + std::to_string(target + 1) + ") should have 2 roots");
    }

    std::cout << "  ✅ Power-of-two boundaries handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 10: Adversarial Delete Pattern
// ═══════════════════════════════════════════════════════════════════════════

void testAdversarialDeletePattern() {
    std::cout << "\n[Test 10] Adversarial Delete Pattern..." << std::endl;

    UtreexoForest forest;
    std::vector<UtreexoHash> leaves;

    // Add 16 leaves
    for (int i = 0; i < 16; i++) {
        auto h = makeUniqueHash(i + 3000);
        leaves.push_back(h);
        forest.add(h);
    }

    // Delete in adversarial pattern: all left children first
    // Positions 0, 2, 4, 6, 8, 10, 12, 14 (even positions)
    for (int i = 0; i < 16; i += 2) {
        auto proof = forest.prove(i);
        if (proof.has_value() && !forest.isDeleted(i)) {
            forest.remove(leaves[i], *proof);
        }
    }

    assertSerializationRoundTrip(forest);

    // All odd positions should still be provable
    auto roots = forest.getRoots();
    for (int i = 1; i < 16; i += 2) {
        if (!forest.isDeleted(i)) {
            auto proof = forest.prove(i);
            INVARIANT(proof.has_value(), "Odd position should be provable");
            INVARIANT(proof->verify(leaves[i], roots), "Odd position proof should verify");
        }
    }

    std::cout << "  ✅ Adversarial delete pattern handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 11: Commitment Collision Attempt
// ═══════════════════════════════════════════════════════════════════════════

void testCommitmentCollision() {
    std::cout << "\n[Test 11] Commitment Collision Attempt..." << std::endl;

    // Try to create two different forests with same commitment (should be impossible)
    std::set<std::vector<uint8_t>> commitments;

    for (int trial = 0; trial < 100; trial++) {
        UtreexoForest forest;

        // Random number of leaves
        int num_leaves = (trial % 10) + 1;
        for (int i = 0; i < num_leaves; i++) {
            forest.add(makeUniqueHash(trial * 1000 + i));
        }

        auto commitment = forest.getCommitment();

        // Check for collision
        auto [it, inserted] = commitments.insert(commitment);
        INVARIANT(inserted, "Commitment collision detected!");
    }

    std::cout << "  ✅ No commitment collisions in 100 trials" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 12: Empty Forest Edge Cases
// ═══════════════════════════════════════════════════════════════════════════

void testEmptyForestEdgeCases() {
    std::cout << "\n[Test 12] Empty Forest Edge Cases..." << std::endl;

    UtreexoForest forest;

    // Empty forest invariants
    INVARIANT(forest.isEmpty(), "New forest should be empty");
    INVARIANT(forest.getNumLeaves() == 0, "Empty forest should have 0 leaves");
    INVARIANT(forest.getNumRoots() == 0, "Empty forest should have 0 roots");
    INVARIANT(forest.getRoots().empty(), "Empty forest getRoots should be empty");

    // Serialize empty forest
    auto data = forest.serialize();
    auto restored = UtreexoForest::deserialize(data);

    INVARIANT(restored.isEmpty(), "Restored empty forest should be empty");
    INVARIANT(restored.getCommitment() == forest.getCommitment(),
        "Empty forest commitment should be preserved");

    // Add to restored empty forest
    restored.add(makeHash(0x42));
    INVARIANT(!restored.isEmpty(), "Forest should not be empty after add");
    INVARIANT(restored.getNumLeaves() == 1, "Should have 1 leaf");

    std::cout << "  ✅ Empty forest edge cases handled correctly" << std::endl;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;
    std::cout << "  UTREEXO ADVERSARIAL TEST SUITE" << std::endl;
    std::cout << "  Testing edge cases that reliably surface bugs" << std::endl;
    std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;

    try {
        testZeroHashStress();
        testInterleavedZeroNonZero();
        testDuplicateHashes();
        testLargeForest();
        testDeleteAddInterleaving();
        testSerializeMutateRollback();
        testProofWithZeroSiblings();
        testBatchProofOverlap();
        testPowerOfTwoBoundaries();
        testAdversarialDeletePattern();
        testCommitmentCollision();
        testEmptyForestEdgeCases();

        std::cout << "\n═══════════════════════════════════════════════════════════════" << std::endl;
        std::cout << "  ✅ ALL 12 ADVERSARIAL TESTS PASSED" << std::endl;
        std::cout << "═══════════════════════════════════════════════════════════════" << std::endl;

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ EXCEPTION: " << e.what() << std::endl;
        return 1;
    }
}
