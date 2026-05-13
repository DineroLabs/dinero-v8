/**
 * Phase 34.1: Utreexo Accumulator Unit Tests
 *
 * Tests for core Utreexo functionality:
 * - Hash functions (HashNode, HashUTXO)
 * - Forest operations (add, remove, prove)
 * - Proof verification
 * - Commitment computation
 * - Serialization/deserialization
 */

#include "consensus/utreexo_accumulator.h"
#include <iostream>
#include <sstream>
#include <cassert>
#include <iomanip>

using namespace dinero::consensus;

// Helper: Print hash as hex string
std::string hashToHex(const Hash256& hash) {
    std::ostringstream oss;
    for (uint8_t byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

// Helper: Create dummy UTXO hash
Hash256 createDummyUTXO(int index) {
    // Create txid as uint256 (Phase M.0: uint256 is canonical identity type)
    std::ostringstream oss;
    oss << std::hex << std::setw(64) << std::setfill('0') << index;
    std::string txid_hex = oss.str();
    // Ensure exactly 64 characters (truncate if longer, pad if shorter)
    if (txid_hex.length() > 64) {
        txid_hex = txid_hex.substr(txid_hex.length() - 64);
    }
    uint256 txid = uint256::FromHexUnsafe(txid_hex);

    std::vector<uint8_t> scriptPubKey = {0x76, 0xa9, 0x14}; // OP_DUP OP_HASH160 OP_PUSH20
    for (int i = 0; i < 20; i++) {
        scriptPubKey.push_back(static_cast<uint8_t>(index + i));
    }
    scriptPubKey.push_back(0x88); // OP_EQUALVERIFY
    scriptPubKey.push_back(0xac); // OP_CHECKSIG

    return HashUTXO(txid, 0, 10000000000ULL, scriptPubKey);  // 100 DIN
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Hash Functions
// ═══════════════════════════════════════════════════════════════════════════

void test_hash_functions() {
    std::cout << "\n[Test 1] Hash Functions\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // Test HashUTXO produces 32-byte hash
    Hash256 utxo1 = createDummyUTXO(1);
    assert(utxo1.size() == 32);
    std::cout << "✓ HashUTXO produces 32-byte hash\n";
    std::cout << "  Hash: " << hashToHex(utxo1).substr(0, 16) << "...\n";

    // Test HashUTXO is deterministic
    Hash256 utxo1_again = createDummyUTXO(1);
    assert(utxo1 == utxo1_again);
    std::cout << "✓ HashUTXO is deterministic\n";

    // Test different UTXOs produce different hashes
    Hash256 utxo2 = createDummyUTXO(2);
    assert(utxo1 != utxo2);
    std::cout << "✓ Different UTXOs produce different hashes\n";

    // Test HashNode
    Hash256 parent = HashNode(utxo1, utxo2);
    assert(parent.size() == 32);
    std::cout << "✓ HashNode produces 32-byte hash\n";
    std::cout << "  Parent: " << hashToHex(parent).substr(0, 16) << "...\n";

    // Test HashNode is deterministic
    Hash256 parent_again = HashNode(utxo1, utxo2);
    assert(parent == parent_again);
    std::cout << "✓ HashNode is deterministic\n";

    // Test HashNode(A,B) != HashNode(B,A) (not commutative)
    Hash256 parent_reversed = HashNode(utxo2, utxo1);
    assert(parent != parent_reversed);
    std::cout << "✓ HashNode is not commutative (A,B) != (B,A)\n";

    std::cout << "✅ All hash function tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Empty Forest
// ═══════════════════════════════════════════════════════════════════════════

void test_empty_forest() {
    std::cout << "\n[Test 2] Empty Forest\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    assert(forest.isEmpty());
    std::cout << "✓ New forest is empty\n";

    assert(forest.getNumLeaves() == 0);
    std::cout << "✓ numLeaves = 0\n";

    assert(forest.getNumRoots() == 0);
    std::cout << "✓ numRoots = 0\n";

    Hash256 commitment = forest.getCommitment();
    assert(commitment.size() == 32);
    std::cout << "✓ Empty forest has 32-byte commitment\n";
    std::cout << "  Commitment: " << hashToHex(commitment).substr(0, 16) << "...\n";

    std::cout << "✅ All empty forest tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Add Single UTXO
// ═══════════════════════════════════════════════════════════════════════════

void test_add_single() {
    std::cout << "\n[Test 3] Add Single UTXO\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;
    Hash256 utxo1 = createDummyUTXO(1);

    uint64_t pos = forest.add(utxo1);

    assert(pos == 0);
    std::cout << "✓ First UTXO added at position 0\n";

    assert(!forest.isEmpty());
    std::cout << "✓ Forest is no longer empty\n";

    assert(forest.getNumLeaves() == 1);
    std::cout << "✓ numLeaves = 1\n";

    assert(forest.getNumRoots() == 1);
    std::cout << "✓ numRoots = 1 (binary: 1 = 0b1)\n";

    std::vector<Hash256> roots = forest.getRoots();
    assert(roots.size() == 1);
    assert(roots[0] == utxo1);
    std::cout << "✓ Root matches added UTXO hash\n";

    std::cout << "✅ All single UTXO tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: Add Multiple UTXOs (Binary Decomposition)
// ═══════════════════════════════════════════════════════════════════════════

void test_add_multiple() {
    std::cout << "\n[Test 4] Add Multiple UTXOs\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    // Add 2 UTXOs: 2 = 0b10 → 1 root (tree of 2 leaves)
    Hash256 utxo1 = createDummyUTXO(1);
    Hash256 utxo2 = createDummyUTXO(2);
    forest.add(utxo1);
    forest.add(utxo2);

    assert(forest.getNumLeaves() == 2);
    assert(forest.getNumRoots() == 1);
    std::cout << "✓ 2 leaves → 1 root (binary: 2 = 0b10)\n";

    // Add 3rd UTXO: 3 = 0b11 → 2 roots
    Hash256 utxo3 = createDummyUTXO(3);
    forest.add(utxo3);

    assert(forest.getNumLeaves() == 3);
    assert(forest.getNumRoots() == 2);
    std::cout << "✓ 3 leaves → 2 roots (binary: 3 = 0b11)\n";

    // Add 4th UTXO: 4 = 0b100 → 1 root
    Hash256 utxo4 = createDummyUTXO(4);
    forest.add(utxo4);

    assert(forest.getNumLeaves() == 4);
    assert(forest.getNumRoots() == 1);
    std::cout << "✓ 4 leaves → 1 root (binary: 4 = 0b100)\n";

    // Add 5th UTXO: 5 = 0b101 → 2 roots
    Hash256 utxo5 = createDummyUTXO(5);
    forest.add(utxo5);

    assert(forest.getNumLeaves() == 5);
    assert(forest.getNumRoots() == 2);
    std::cout << "✓ 5 leaves → 2 roots (binary: 5 = 0b101)\n";

    // Test commitment changes
    Hash256 commitment5 = forest.getCommitment();

    Hash256 utxo6 = createDummyUTXO(6);
    forest.add(utxo6);
    Hash256 commitment6 = forest.getCommitment();

    assert(commitment5 != commitment6);
    std::cout << "✓ Commitment changes when UTXO added\n";

    std::cout << "✅ All multiple UTXO tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Proof Generation and Verification
// ═══════════════════════════════════════════════════════════════════════════

void test_proofs() {
    std::cout << "\n[Test 5] Proof Generation and Verification\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    // Add 8 UTXOs (perfect binary tree)
    std::vector<Hash256> utxos;
    for (int i = 0; i < 8; i++) {
        Hash256 utxo = createDummyUTXO(i);
        utxos.push_back(utxo);
        forest.add(utxo);
    }

    assert(forest.getNumLeaves() == 8);
    assert(forest.getNumRoots() == 1);
    std::cout << "✓ Created forest with 8 leaves (1 root)\n";

    // Generate proof for first UTXO
    auto proof_opt = forest.prove(0);
    assert(proof_opt.has_value());
    std::cout << "✓ Generated proof for position 0\n";

    UtreexoProof proof = proof_opt.value();
    assert(proof.position == 0);
    assert(proof.numLeaves == 8);
    std::cout << "✓ Proof has correct metadata\n";

    // Proof size should be log₂(8) = 3 hashes
    assert(proof.siblings.size() == 3);
    std::cout << "✓ Proof size = log₂(n) = " << proof.siblings.size() << " hashes\n";

    // Verify proof
    std::vector<Hash256> roots = forest.getRoots();
    bool valid = proof.verify(utxos[0], roots);
    assert(valid);
    std::cout << "✓ Proof verifies correctly\n";

    // Test invalid proof (wrong leaf hash)
    Hash256 wrong_utxo = createDummyUTXO(999);
    bool invalid = proof.verify(wrong_utxo, roots);
    assert(!invalid);
    std::cout << "✓ Proof fails for wrong UTXO\n";

    // Test proof for middle position
    auto proof_mid = forest.prove(4);
    assert(proof_mid.has_value());
    assert(proof_mid->siblings.size() == 3);
    assert(proof_mid->verify(utxos[4], roots));
    std::cout << "✓ Proof for middle position works\n";

    // Test proof for last position
    auto proof_last = forest.prove(7);
    assert(proof_last.has_value());
    assert(proof_last->verify(utxos[7], roots));
    std::cout << "✓ Proof for last position works\n";

    std::cout << "✅ All proof tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Proof Serialization
// ═══════════════════════════════════════════════════════════════════════════

void test_proof_serialization() {
    std::cout << "\n[Test 6] Proof Serialization\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    // Add 4 UTXOs
    std::vector<Hash256> utxos;
    for (int i = 0; i < 4; i++) {
        utxos.push_back(createDummyUTXO(i));
        forest.add(utxos.back());
    }

    // Generate proof
    auto proof_opt = forest.prove(1);
    assert(proof_opt.has_value());
    UtreexoProof original_proof = proof_opt.value();

    std::cout << "✓ Generated proof for position 1\n";

    // Serialize proof
    std::vector<uint8_t> serialized = original_proof.serialize();
    size_t expected_size = 8 + 8 + 8 + (original_proof.siblings.size() * 32);
    assert(serialized.size() == expected_size);
    std::cout << "✓ Serialized proof (" << serialized.size() << " bytes)\n";

    // Deserialize proof
    UtreexoProof deserialized = UtreexoProof::deserialize(serialized);

    assert(deserialized.position == original_proof.position);
    assert(deserialized.numLeaves == original_proof.numLeaves);
    assert(deserialized.siblings.size() == original_proof.siblings.size());
    std::cout << "✓ Deserialized proof matches original\n";

    // Verify deserialized proof works
    std::vector<Hash256> roots = forest.getRoots();
    bool valid = deserialized.verify(utxos[1], roots);
    assert(valid);
    std::cout << "✓ Deserialized proof verifies correctly\n";

    std::cout << "✅ All serialization tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: Forest Serialization
// ═══════════════════════════════════════════════════════════════════════════

void test_forest_serialization() {
    std::cout << "\n[Test 7] Forest Serialization\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest original_forest;

    // Add 5 UTXOs (5 = 0b101 → 2 roots)
    for (int i = 0; i < 5; i++) {
        original_forest.add(createDummyUTXO(i));
    }

    Hash256 original_commitment = original_forest.getCommitment();
    std::cout << "✓ Created forest with 5 leaves\n";
    std::cout << "  Original commitment: " << hashToHex(original_commitment).substr(0, 16) << "...\n";

    // Serialize forest
    std::vector<uint8_t> serialized = original_forest.serialize();
    std::cout << "✓ Serialized forest (" << serialized.size() << " bytes)\n";

    // Deserialize forest
    UtreexoForest restored_forest = UtreexoForest::deserialize(serialized);

    assert(restored_forest.getNumLeaves() == 5);
    assert(restored_forest.getNumRoots() == 2);
    std::cout << "✓ Deserialized forest has correct structure\n";

    // Verify commitment matches
    Hash256 restored_commitment = restored_forest.getCommitment();
    assert(restored_commitment == original_commitment);
    std::cout << "✓ Commitment matches after deserialization\n";

    // Verify roots match
    std::vector<Hash256> original_roots = original_forest.getRoots();
    std::vector<Hash256> restored_roots = restored_forest.getRoots();
    assert(original_roots == restored_roots);
    std::cout << "✓ All roots match after deserialization\n";

    std::cout << "✅ All forest serialization tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 8: Remove UTXO
// ═══════════════════════════════════════════════════════════════════════════

void test_remove() {
    std::cout << "\n[Test 8] Remove UTXO\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    // Add 4 UTXOs
    std::vector<Hash256> utxos;
    for (int i = 0; i < 4; i++) {
        utxos.push_back(createDummyUTXO(i));
        forest.add(utxos.back());
    }

    Hash256 commitment_before = forest.getCommitment();
    std::cout << "✓ Created forest with 4 leaves\n";

    // Generate proof for UTXO at position 1
    auto proof_opt = forest.prove(1);
    assert(proof_opt.has_value());
    UtreexoProof proof = proof_opt.value();

    // Remove UTXO
    bool removed = forest.remove(utxos[1], proof);
    assert(removed);
    std::cout << "✓ Removed UTXO at position 1\n";

    assert(forest.getNumLeaves() == 3);
    std::cout << "✓ numLeaves decreased: 4 → 3\n";

    // Commitment should change
    Hash256 commitment_after = forest.getCommitment();
    assert(commitment_before != commitment_after);
    std::cout << "✓ Commitment changed after removal\n";

    // Try to remove with invalid proof (should fail)
    Hash256 wrong_utxo = createDummyUTXO(999);
    bool failed = forest.remove(wrong_utxo, proof);
    assert(!failed);
    std::cout << "✓ Removal with invalid proof fails\n";

    std::cout << "✅ All remove tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 9: Batch Update
// ═══════════════════════════════════════════════════════════════════════════

void test_batch_update() {
    std::cout << "\n[Test 9] Batch Update\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    // Initial state: 4 UTXOs
    std::vector<Hash256> old_utxos;
    for (int i = 0; i < 4; i++) {
        old_utxos.push_back(createDummyUTXO(i));
        forest.add(old_utxos.back());
    }
    std::cout << "✓ Initial forest: 4 leaves\n";

    // Create batch update:
    // - Remove 2 old UTXOs (positions 1 and 2)
    // - Add 3 new UTXOs
    UtreexoBatchUpdate batch;

    // Adds
    batch.adds.push_back(createDummyUTXO(100));
    batch.adds.push_back(createDummyUTXO(101));
    batch.adds.push_back(createDummyUTXO(102));

    // Removes (need proofs)
    auto proof1 = forest.prove(1);
    auto proof2 = forest.prove(2);
    assert(proof1.has_value() && proof2.has_value());

    batch.removes.push_back({old_utxos[1], proof1.value()});
    batch.removes.push_back({old_utxos[2], proof2.value()});

    // Apply batch
    bool success = batch.apply(forest);
    assert(success);
    std::cout << "✓ Applied batch update (removed 2, added 3)\n";

    // Final state: 4 - 2 + 3 = 5 leaves
    assert(forest.getNumLeaves() == 5);
    std::cout << "✓ Final forest: 5 leaves (4 - 2 + 3)\n";

    std::cout << "✅ All batch update tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 10: Performance and Statistics
// ═══════════════════════════════════════════════════════════════════════════

void test_performance() {
    std::cout << "\n[Test 10] Performance and Statistics\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    UtreexoForest forest;

    // Add 1000 UTXOs
    const int NUM_UTXOS = 1000;
    for (int i = 0; i < NUM_UTXOS; i++) {
        forest.add(createDummyUTXO(i));
    }

    assert(forest.getNumLeaves() == NUM_UTXOS);
    std::cout << "✓ Added " << NUM_UTXOS << " UTXOs\n";

    // Get statistics
    auto stats = forest.getStats();
    std::cout << "\nForest Statistics:\n";
    std::cout << "  Leaves:         " << stats.numLeaves << "\n";
    std::cout << "  Roots:          " << stats.numRoots << "\n";
    std::cout << "  Total size:     " << stats.totalSize << " bytes\n";
    std::cout << "  Avg proof size: " << stats.avgProofSize << " bytes\n";

    // Verify proof size is logarithmic
    // log₂(1000) ≈ 10, so proof should be ~10 hashes = ~320 bytes
    assert(stats.avgProofSize < 400);
    std::cout << "✓ Proof size is logarithmic: " << stats.avgProofSize << " bytes for " << NUM_UTXOS << " UTXOs\n";

    // Verify commitment is compact (32 bytes regardless of UTXO count)
    Hash256 commitment = forest.getCommitment();
    assert(commitment.size() == 32);
    std::cout << "✓ Commitment is constant 32 bytes for any UTXO count\n";

    // Calculate compression ratio
    // Traditional UTXO set: ~200 bytes per UTXO
    size_t traditional_size = NUM_UTXOS * 200;
    double compression = static_cast<double>(traditional_size) / stats.totalSize;
    std::cout << "\nCompression:\n";
    std::cout << "  Traditional UTXO set: ~" << traditional_size / 1024 << " KB\n";
    std::cout << "  Utreexo accumulator:  ~" << stats.totalSize / 1024 << " KB\n";
    std::cout << "  Compression ratio:    " << std::fixed << std::setprecision(1) << compression << "x\n";

    std::cout << "\n✅ All performance tests passed\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Main Test Runner
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  UTREEXO ACCUMULATOR UNIT TESTS\n";
    std::cout << "  Phase 34.1: Core Accumulator Implementation\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";

    try {
        test_hash_functions();
        test_empty_forest();
        test_add_single();
        test_add_multiple();
        test_proofs();
        test_proof_serialization();
        test_forest_serialization();
        test_remove();
        test_batch_update();
        test_performance();

        std::cout << "\n═══════════════════════════════════════════════════════════════\n";
        std::cout << "  ✅ ALL TESTS PASSED (" << 10 << " test suites)\n";
        std::cout << "═══════════════════════════════════════════════════════════════\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << "\n";
        return 1;
    }
}
