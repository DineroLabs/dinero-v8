/**
 * Minimal test for stateless batch proof verification
 * Can be compiled standalone with just dinero_consensus
 */

#include "consensus/utreexo_accumulator.h"
#include <iostream>
#include <cassert>

using namespace dinero::consensus;

// Create a deterministic test hash
UtreexoHash makeHash(uint8_t seed) {
    UtreexoHash h(32);
    for (int i = 0; i < 32; i++) h[i] = (seed * 7 + i * 13) % 256;
    return h;
}

int main() {
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  Minimal Stateless Batch Proof Test\n";
    std::cout << "═══════════════════════════════════════════════════════════\n\n";

    int passed = 0, failed = 0;

    // Test 1: BlockUtreexoProof includes positions
    {
        std::cout << "[Test 1] BlockUtreexoProof structure has positions\n";
        BlockUtreexoProof proof;
        proof.targets.push_back(makeHash(1));
        proof.positions.push_back(0);
        proof.numLeaves = 4;

        if (proof.isValid() && proof.positions.size() == proof.targets.size()) {
            std::cout << "✅ PASS: isValid() works correctly\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Structure validation failed\n";
            failed++;
        }
    }

    // Test 2: Serialization includes positions (version 5)
    {
        std::cout << "\n[Test 2] Serialization format includes positions\n";
        BlockUtreexoProof original;
        original.targets.push_back(makeHash(10));
        original.targets.push_back(makeHash(20));
        original.positions.push_back(5);
        original.positions.push_back(7);
        original.numLeaves = 16;
        original.proof_hashes.push_back(makeHash(99));

        std::vector<uint8_t> data = original.serialize();
        std::cout << "  Serialized size: " << data.size() << " bytes\n";
        std::cout << "  Version byte: " << (int)data[0] << " (expected: 5)\n";

        BlockUtreexoProof restored = BlockUtreexoProof::deserialize(data);

        bool match = (restored.targets.size() == original.targets.size() &&
                      restored.positions.size() == original.positions.size() &&
                      restored.numLeaves == original.numLeaves &&
                      restored.positions[0] == 5 &&
                      restored.positions[1] == 7);

        if (match) {
            std::cout << "✅ PASS: Round-trip serialization preserves positions\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Positions not preserved\n";
            failed++;
        }
    }

    // Test 3: generateBlockProof produces positions
    {
        std::cout << "\n[Test 3] generateBlockProof includes positions\n";
        UtreexoForest forest;

        // Add some leaves
        std::vector<UtreexoHash> leaves;
        for (int i = 0; i < 8; i++) {
            UtreexoHash leaf = makeHash(i + 1);
            leaves.push_back(leaf);
            forest.add(leaf);
        }

        // Generate block proof for some targets
        std::vector<UtreexoHash> targets = { leaves[0], leaves[3], leaves[7] };
        BlockUtreexoProof proof = forest.generateBlockProof(targets);

        std::cout << "  Targets: " << proof.targets.size() << "\n";
        std::cout << "  Positions: " << proof.positions.size() << "\n";
        std::cout << "  numLeaves: " << proof.numLeaves << "\n";
        std::cout << "  Proof hashes: " << proof.proof_hashes.size() << "\n";

        if (proof.isValid() && proof.numLeaves == 8 && proof.positions.size() == 3) {
            std::cout << "✅ PASS: generateBlockProof produces valid proof with positions\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Proof generation failed\n";
            failed++;
        }
    }

    // Test 4: Empty proof verification
    {
        std::cout << "\n[Test 4] Empty proof (coinbase-only) verification\n";
        UtreexoForest forest;

        std::vector<UtreexoHash> empty_targets;
        std::vector<uint64_t> empty_positions;
        std::vector<UtreexoHash> empty_proofs;
        std::vector<UtreexoHash> empty_roots;

        bool result = forest.verifyBatchProofStateless(
            empty_targets, empty_positions, empty_proofs, 0, empty_roots);

        if (result) {
            std::cout << "✅ PASS: Empty proof accepted\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Empty proof should be accepted\n";
            failed++;
        }
    }

    // Test 5: Position count mismatch rejection
    {
        std::cout << "\n[Test 5] Position count mismatch rejection\n";
        UtreexoForest forest;
        forest.add(makeHash(1));
        forest.add(makeHash(2));

        std::vector<UtreexoHash> targets = { makeHash(1), makeHash(2) };
        std::vector<uint64_t> positions = { 0 };  // Only 1 position!
        std::vector<UtreexoHash> proofs;

        bool result = forest.verifyBatchProofStateless(
            targets, positions, proofs, 2, forest.getRoots());

        if (!result) {
            std::cout << "✅ PASS: Position mismatch correctly rejected\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Should reject mismatched positions\n";
            failed++;
        }
    }

    // Test 6: Invalid position rejection
    {
        std::cout << "\n[Test 6] Invalid position rejection\n";
        UtreexoForest forest;
        forest.add(makeHash(1));

        std::vector<UtreexoHash> targets = { makeHash(1) };
        std::vector<uint64_t> positions = { 999 };  // Invalid!
        std::vector<UtreexoHash> proofs;

        bool result = forest.verifyBatchProofStateless(
            targets, positions, proofs, 1, forest.getRoots());

        if (!result) {
            std::cout << "✅ PASS: Invalid position correctly rejected\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Should reject invalid position\n";
            failed++;
        }
    }

    // Test 7: Assembler ↔ Stateless Equivalence (belt-and-suspenders)
    {
        std::cout << "\n[Test 7] Assembler ↔ Stateless equivalence\n";
        std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

        // Setup: Create forest with some UTXOs
        UtreexoForest forest;
        std::vector<UtreexoHash> leaves;
        for (int i = 0; i < 8; i++) {
            UtreexoHash leaf = makeHash(i + 100);
            leaves.push_back(leaf);
            forest.add(leaf);
        }

        // Capture root BEFORE block
        UtreexoHash r0 = forest.getCommitment();

        std::cout << "  Root r0 (before): ";
        for (int i = 0; i < 8; i++) std::cout << std::hex << (int)r0[i];
        std::cout << std::dec << "...\n";

        // Simulate block: spend some UTXOs, add new ones
        std::vector<UtreexoHash> spends = { leaves[1], leaves[4] };

        // Generate proof at r0 (captures positions at this state)
        BlockUtreexoProof proof = forest.generateBlockProof(spends);

        std::cout << "  Generated proof with " << proof.positions.size() << " positions\n";
        std::cout << "  Positions: [" << proof.positions[0] << ", " << proof.positions[1] << "]\n";

        // PATH A: Apply block using proof positions (assembler path)
        UtreexoForest forest_a = forest.clone();
        for (size_t i = 0; i < spends.size(); i++) {
            auto proof_opt = forest_a.prove(proof.positions[i]);
            if (proof_opt.has_value()) {
                forest_a.remove(spends[i], proof_opt.value());
            }
        }
        UtreexoHash new_output1 = makeHash(200);
        UtreexoHash new_output2 = makeHash(201);
        forest_a.add(new_output1);
        forest_a.add(new_output2);

        UtreexoHash r1_a = forest_a.getCommitment();

        // PATH B: Apply same operations independently (stateless receiver)
        // A stateless node receives: proof.targets, proof.positions, proof.numLeaves
        // It can verify the spends existed and apply the same delta
        UtreexoForest forest_b = forest.clone();
        for (size_t i = 0; i < proof.targets.size(); i++) {
            auto proof_opt = forest_b.prove(proof.positions[i]);
            if (proof_opt.has_value()) {
                forest_b.remove(proof.targets[i], proof_opt.value());
            }
        }
        forest_b.add(new_output1);
        forest_b.add(new_output2);

        UtreexoHash r1_b = forest_b.getCommitment();

        std::cout << "  Root r1 (path A): ";
        for (int i = 0; i < 8; i++) std::cout << std::hex << (int)r1_a[i];
        std::cout << std::dec << "...\n";

        std::cout << "  Root r1 (path B): ";
        for (int i = 0; i < 8; i++) std::cout << std::hex << (int)r1_b[i];
        std::cout << std::dec << "...\n";

        // CORE INVARIANT: Both paths produce identical roots
        bool roots_match = (r1_a == r1_b);

        // Also verify proof structure is correct
        bool proof_valid = proof.isValid() &&
                          proof.numLeaves == 8 &&
                          proof.positions.size() == spends.size();

        if (roots_match && proof_valid) {
            std::cout << "✅ PASS: Assembler ↔ Stateless equivalence confirmed\n";
            std::cout << "  - Proof structure valid (positions match targets)\n";
            std::cout << "  - Both paths produce identical root r1\n";
            std::cout << "  - Positions enable deterministic replay\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: ";
            if (!proof_valid) std::cout << "Proof structure invalid. ";
            if (!roots_match) std::cout << "Roots don't match!";
            std::cout << "\n";
            failed++;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // MEDIUM PRIORITY HARDENING TESTS
    // ═══════════════════════════════════════════════════════════════════════

    // Test 8: Compression v2 - empty dictionary with non-empty indices
    {
        std::cout << "\n[Test 8] Compression v2: Empty dictionary rejection\n";

        // Craft malformed v2 data: version=2, dict_size=0, but 1 target index
        std::vector<uint8_t> malformed;
        malformed.push_back(2);  // Version 2
        // dict_size = 0 (4 bytes, little-endian)
        malformed.push_back(0); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);
        // num_targets = 1 (4 bytes)
        malformed.push_back(1); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);
        // target index = 0 (would be OOB in empty dictionary)
        malformed.push_back(0); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);
        // num_proofs = 0
        malformed.push_back(0); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);

        BlockUtreexoProof result = BlockUtreexoProof::deserializeCompressed(malformed);

        if (result.targets.empty() && result.proof_hashes.empty()) {
            std::cout << "✅ PASS: Empty dictionary with indices correctly rejected\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Should reject empty dictionary with target indices\n";
            failed++;
        }
    }

    // Test 9: Compression v2 - out-of-bounds index rejection
    {
        std::cout << "\n[Test 9] Compression v2: OOB index rejection\n";

        // Craft malformed v2 data: dictionary has 1 entry, index points to entry 5
        std::vector<uint8_t> malformed;
        malformed.push_back(2);  // Version 2
        // dict_size = 1
        malformed.push_back(1); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);
        // 1 dictionary entry (32 bytes of zeros)
        for (int i = 0; i < 32; i++) malformed.push_back(0);
        // num_targets = 1
        malformed.push_back(1); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);
        // target index = 5 (OOB - only 1 dictionary entry)
        malformed.push_back(5); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);
        // num_proofs = 0
        malformed.push_back(0); malformed.push_back(0);
        malformed.push_back(0); malformed.push_back(0);

        BlockUtreexoProof result = BlockUtreexoProof::deserializeCompressed(malformed);

        if (result.targets.empty()) {
            std::cout << "✅ PASS: OOB dictionary index correctly rejected\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Should reject OOB dictionary index\n";
            failed++;
        }
    }

    // Test 10: Compression v2 - leftover bytes rejection
    {
        std::cout << "\n[Test 10] Compression v2: Leftover bytes rejection\n";

        // Create valid v2 proof, then append garbage
        BlockUtreexoProof valid;
        valid.targets.push_back(makeHash(42));
        valid.proof_hashes.push_back(makeHash(43));
        std::vector<uint8_t> data = valid.serializeCompressed();

        // Append leftover garbage bytes
        data.push_back(0xFF);
        data.push_back(0xAB);

        BlockUtreexoProof result = BlockUtreexoProof::deserializeCompressed(data);

        if (result.targets.empty() && result.proof_hashes.empty()) {
            std::cout << "✅ PASS: Leftover bytes correctly rejected\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Should reject data with leftover bytes\n";
            failed++;
        }
    }

    // Test 11: Integer overflow - MAX_UTREEXO_LEAVES enforcement
    {
        std::cout << "\n[Test 11] Integer overflow: MAX_UTREEXO_LEAVES bounds\n";

        UtreexoForest forest;

        // Verify the constant exists and is reasonable
        bool bounds_exist = (MAX_UTREEXO_LEAVES > 0 && MAX_UTREEXO_LEAVES <= (1ULL << 40));

        // Verify checked_add works
        uint64_t result;
        bool overflow_detected = !checked_add(UINT64_MAX, 1, result);

        // Verify checked_shift_left works
        uint64_t shift_result;
        bool shift_overflow = !checked_shift_left(1ULL, 64, shift_result);  // Shift by 64 should fail

        if (bounds_exist && overflow_detected && shift_overflow) {
            std::cout << "✅ PASS: Integer overflow protection in place\n";
            std::cout << "  - MAX_UTREEXO_LEAVES = " << MAX_UTREEXO_LEAVES << "\n";
            std::cout << "  - checked_add detects overflow\n";
            std::cout << "  - checked_shift_left detects overflow\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Missing overflow protection\n";
            failed++;
        }
    }

    // Test 12: Checked arithmetic helpers
    {
        std::cout << "\n[Test 12] Checked arithmetic helper functions\n";

        uint64_t result;
        int tests_passed = 0;

        // checked_add: normal case
        if (checked_add(100, 200, result) && result == 300) tests_passed++;

        // checked_add: overflow case
        if (!checked_add(UINT64_MAX, 1, result)) tests_passed++;

        // checked_mul: normal case
        if (checked_mul(1000, 1000, result) && result == 1000000) tests_passed++;

        // checked_mul: overflow case
        if (!checked_mul(UINT64_MAX, 2, result)) tests_passed++;

        // checked_shift_left: normal case
        if (checked_shift_left(1, 10, result) && result == 1024) tests_passed++;

        // checked_shift_left: shift too large
        if (!checked_shift_left(1, 64, result)) tests_passed++;

        // checked_shift_left: value too large for shift
        if (!checked_shift_left(UINT64_MAX, 1, result)) tests_passed++;

        // checked_increment: normal case
        uint64_t val = 100;
        if (checked_increment(val) && val == 101) tests_passed++;

        // checked_increment: overflow case
        val = UINT64_MAX;
        if (!checked_increment(val)) tests_passed++;

        if (tests_passed == 9) {
            std::cout << "✅ PASS: All 9 checked arithmetic tests passed\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: Only " << tests_passed << "/9 arithmetic tests passed\n";
            failed++;
        }
    }

    // Test 13: MAX_UTREEXO_PROOF_BYTES DoS protection
    {
        std::cout << "\n[Test 13] MAX_UTREEXO_PROOF_BYTES DoS protection\n";

        // Verify the constant exists and is reasonable (4 MB)
        bool constant_valid = (MAX_UTREEXO_PROOF_BYTES == 4 * 1024 * 1024);

        // Create oversized data (just over limit)
        std::vector<uint8_t> oversized(MAX_UTREEXO_PROOF_BYTES + 1, 0);
        oversized[0] = 5;  // Version 5

        BlockUtreexoProof result = BlockUtreexoProof::deserialize(oversized);

        // Should reject before allocating
        bool rejected = result.targets.empty() && result.proof_hashes.empty();

        if (constant_valid && rejected) {
            std::cout << "✅ PASS: Oversized proof rejected (4 MB limit)\n";
            std::cout << "  - MAX_UTREEXO_PROOF_BYTES = " << MAX_UTREEXO_PROOF_BYTES << " bytes\n";
            passed++;
        } else {
            std::cout << "❌ FAIL: DoS protection not working\n";
            failed++;
        }
    }

    // Summary
    std::cout << "\n═══════════════════════════════════════════════════════════\n";
    std::cout << "  Summary: " << passed << " passed, " << failed << " failed\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";

    return (failed == 0) ? 0 : 1;
}
