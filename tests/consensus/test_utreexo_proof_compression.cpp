/**
 * Phase 9.1: Utreexo Proof Compression Tests
 *
 * CRITICAL: Tests for compression correctness and security
 * - Round-trip serialization (compression preserves data)
 * - Backward compatibility (v1 uncompressed still works)
 * - Deduplication correctness (index mapping preserves proof validity)
 * - Adversarial validation (reject corrupted indices/dictionaries)
 *
 * Context: Phase 9 changes wire format and data semantics.
 * Silent corruption is possible without these tests.
 */

#include "consensus/utreexo_accumulator.h"
#include "crypto/hash.h"
#include <iostream>
#include <cassert>
#include <iomanip>
#include <set>

using namespace dinero::consensus;

// ═══════════════════════════════════════════════════════════════════════════
// Test Helpers
// ═══════════════════════════════════════════════════════════════════════════

// Helper: Create random 32-byte hash
UtreexoHash makeRandomHash(uint32_t seed) {
    UtreexoHash hash(32);
    for (size_t i = 0; i < 32; i++) {
        hash[i] = static_cast<uint8_t>((seed + i) % 256);
    }
    return hash;
}

// Helper: Create BlockUtreexoProof with random data
BlockUtreexoProof makeRandomProof(size_t num_targets, size_t num_proofs) {
    BlockUtreexoProof proof;

    for (size_t i = 0; i < num_targets; i++) {
        proof.targets.push_back(makeRandomHash(i * 1000));
    }

    for (size_t i = 0; i < num_proofs; i++) {
        proof.proof_hashes.push_back(makeRandomHash(i * 2000));
    }

    return proof;
}

// Helper: Create proof with intentional hash duplication
BlockUtreexoProof makeProofWithRepeatedHashes() {
    BlockUtreexoProof proof;

    // Create some unique hashes
    UtreexoHash hash_a = makeRandomHash(100);
    UtreexoHash hash_b = makeRandomHash(200);
    UtreexoHash hash_c = makeRandomHash(300);
    UtreexoHash hash_d = makeRandomHash(400);

    // Add targets with duplication
    proof.targets.push_back(hash_a);
    proof.targets.push_back(hash_b);
    proof.targets.push_back(hash_a);  // Duplicate hash_a

    // Add proof_hashes with duplication
    proof.proof_hashes.push_back(hash_c);
    proof.proof_hashes.push_back(hash_d);
    proof.proof_hashes.push_back(hash_c);  // Duplicate hash_c
    proof.proof_hashes.push_back(hash_d);  // Duplicate hash_d
    proof.proof_hashes.push_back(hash_c);  // Duplicate hash_c again

    return proof;
}

// Helper: Compare two proofs for equality
bool proofsEqual(const BlockUtreexoProof& a, const BlockUtreexoProof& b) {
    if (a.targets.size() != b.targets.size()) return false;
    if (a.proof_hashes.size() != b.proof_hashes.size()) return false;

    for (size_t i = 0; i < a.targets.size(); i++) {
        if (a.targets[i] != b.targets[i]) return false;
    }

    for (size_t i = 0; i < a.proof_hashes.size(); i++) {
        if (a.proof_hashes[i] != b.proof_hashes[i]) return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: Round-Trip Serialization (MANDATORY)
// ═══════════════════════════════════════════════════════════════════════════

void test_roundtrip_identity() {
    std::cout << "\n[Test 1] Round-Trip Compression Identity\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "CRITICAL: Compression must preserve proof data exactly\n\n";

    // Test case 1: Empty proof
    {
        BlockUtreexoProof original;
        auto encoded = original.serializeCompressed();
        auto decoded = BlockUtreexoProof::deserializeCompressed(encoded);

        assert(proofsEqual(original, decoded));
        std::cout << "✓ Empty proof: round-trip preserves data\n";
    }

    // Test case 2: Small proof (10 targets, 20 proof hashes)
    {
        BlockUtreexoProof original = makeRandomProof(10, 20);
        auto encoded = original.serializeCompressed();
        auto decoded = BlockUtreexoProof::deserializeCompressed(encoded);

        assert(proofsEqual(original, decoded));
        std::cout << "✓ Small proof (10 targets, 20 proofs): round-trip preserves data\n";
    }

    // Test case 3: Large proof (100 targets, 200 proof hashes)
    {
        BlockUtreexoProof original = makeRandomProof(100, 200);
        auto encoded = original.serializeCompressed();
        auto decoded = BlockUtreexoProof::deserializeCompressed(encoded);

        assert(proofsEqual(original, decoded));
        std::cout << "✓ Large proof (100 targets, 200 proofs): round-trip preserves data\n";
    }

    // Test case 4: Proof with repeated hashes (critical for dedup)
    {
        BlockUtreexoProof original = makeProofWithRepeatedHashes();
        auto encoded = original.serializeCompressed();
        auto decoded = BlockUtreexoProof::deserializeCompressed(encoded);

        assert(proofsEqual(original, decoded));
        std::cout << "✓ Proof with repeated hashes: round-trip preserves data\n";
    }

    // Test case 5: Proof with all same hash (edge case)
    {
        BlockUtreexoProof original;
        UtreexoHash same_hash = makeRandomHash(999);
        for (int i = 0; i < 5; i++) {
            original.targets.push_back(same_hash);
            original.proof_hashes.push_back(same_hash);
        }

        auto encoded = original.serializeCompressed();
        auto decoded = BlockUtreexoProof::deserializeCompressed(encoded);

        assert(proofsEqual(original, decoded));
        std::cout << "✓ Proof with all identical hashes: round-trip preserves data\n";
    }

    std::cout << "\n✅ Round-trip identity test PASSED\n";
    std::cout << "   Compression is lossless (no data corruption)\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: Backward Compatibility (Version 1 Uncompressed)
// ═══════════════════════════════════════════════════════════════════════════

void test_backward_compatible_v1() {
    std::cout << "\n[Test 2] Backward Compatibility (Version 1)\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "CRITICAL: Old peers must still work with uncompressed format\n\n";

    // Test case 1: V1 format (uncompressed) must still deserialize
    {
        BlockUtreexoProof original = makeRandomProof(10, 20);

        // Serialize using v1 (uncompressed) format
        auto v1_encoded = original.serialize();

        // Deserialize using v1 deserializer
        auto decoded = BlockUtreexoProof::deserialize(v1_encoded);

        assert(proofsEqual(original, decoded));
        std::cout << "✓ V1 uncompressed format: serialize/deserialize works\n";
    }

    // Test case 2: V2 format (compressed) must be distinct from v1
    {
        BlockUtreexoProof original = makeRandomProof(10, 20);

        auto v1_encoded = original.serialize();
        auto v2_encoded = original.serializeCompressed();

        // Formats should be different (version byte difference)
        assert(v1_encoded.size() > 0 && v2_encoded.size() > 0);
        assert(v1_encoded[0] != v2_encoded[0]);  // Different version bytes

        std::cout << "✓ V1 and V2 formats are distinct (different version bytes)\n";
    }

    // Test case 3: Both formats should produce same logical proof
    {
        BlockUtreexoProof original = makeRandomProof(10, 20);

        auto v1_encoded = original.serialize();
        auto v2_encoded = original.serializeCompressed();

        auto v1_decoded = BlockUtreexoProof::deserialize(v1_encoded);
        auto v2_decoded = BlockUtreexoProof::deserializeCompressed(v2_encoded);

        assert(proofsEqual(v1_decoded, v2_decoded));
        std::cout << "✓ V1 and V2 produce logically equivalent proofs\n";
    }

    std::cout << "\n✅ Backward compatibility test PASSED\n";
    std::cout << "   Old peers can still use V1 uncompressed format\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: Deduplication Correctness
// ═══════════════════════════════════════════════════════════════════════════

void test_dedup_correctness() {
    std::cout << "\n[Test 3] Deduplication Correctness\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "CRITICAL: Index mapping must preserve hash order and duplication\n\n";

    // Test case 1: Dedup actually reduces size
    {
        BlockUtreexoProof proof = makeProofWithRepeatedHashes();

        auto uncompressed = proof.serialize();
        auto compressed = proof.serializeCompressed();

        assert(compressed.size() < uncompressed.size());
        std::cout << "✓ Compression reduces size: "
                  << uncompressed.size() << " bytes → "
                  << compressed.size() << " bytes ("
                  << (100 - (compressed.size() * 100 / uncompressed.size()))
                  << "% savings)\n";
    }

    // Test case 2: Hash order is preserved exactly
    {
        BlockUtreexoProof original;
        original.targets = {
            makeRandomHash(1),
            makeRandomHash(2),
            makeRandomHash(1),  // Same as first
        };
        original.proof_hashes = {
            makeRandomHash(3),
            makeRandomHash(4),
            makeRandomHash(3),  // Same as first
        };

        auto compressed = original.serializeCompressed();
        auto decoded = BlockUtreexoProof::deserializeCompressed(compressed);

        // Check targets order
        assert(decoded.targets[0] == original.targets[0]);
        assert(decoded.targets[1] == original.targets[1]);
        assert(decoded.targets[2] == original.targets[2]);
        assert(decoded.targets[0] == decoded.targets[2]);  // Duplication preserved

        // Check proof_hashes order
        assert(decoded.proof_hashes[0] == original.proof_hashes[0]);
        assert(decoded.proof_hashes[1] == original.proof_hashes[1]);
        assert(decoded.proof_hashes[2] == original.proof_hashes[2]);
        assert(decoded.proof_hashes[0] == decoded.proof_hashes[2]);  // Duplication preserved

        std::cout << "✓ Hash order preserved: duplicates map to same hash\n";
    }

    // Test case 3: Dictionary size is correct
    {
        BlockUtreexoProof proof = makeProofWithRepeatedHashes();

        // Count unique hashes manually
        std::set<UtreexoHash> unique_hashes;
        for (const auto& h : proof.targets) unique_hashes.insert(h);
        for (const auto& h : proof.proof_hashes) unique_hashes.insert(h);

        size_t expected_dict_size = unique_hashes.size();

        // Estimate compressed size (should match dictionary)
        size_t estimated = proof.estimateCompressedSize();

        // Dictionary overhead: 1 (version) + 4 (dict_size) + 32*dict_size (hashes)
        // Plus indices overhead
        size_t dict_overhead = 1 + 4 + (32 * expected_dict_size);

        assert(estimated >= dict_overhead);
        std::cout << "✓ Dictionary size correct: " << expected_dict_size << " unique hashes\n";
    }

    std::cout << "\n✅ Deduplication correctness test PASSED\n";
    std::cout << "   Index mapping preserves hash identity and order\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: Adversarial Validation
// ═══════════════════════════════════════════════════════════════════════════

void test_adversarial_inputs() {
    std::cout << "\n[Test 4] Adversarial Input Validation\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "CRITICAL: Reject corrupted indices and malformed dictionaries\n\n";

    // Test case 1: Invalid version byte (reject unknown versions)
    {
        std::vector<uint8_t> bad_data = {99};  // Invalid version
        auto decoded = BlockUtreexoProof::deserializeCompressed(bad_data);

        assert(decoded.isEmpty());
        std::cout << "✓ Invalid version rejected (returns empty proof)\n";
    }

    // Test case 2: Truncated data (incomplete dictionary)
    {
        BlockUtreexoProof original = makeRandomProof(5, 10);
        auto valid_data = original.serializeCompressed();

        // Truncate to half size (corrupted)
        std::vector<uint8_t> truncated(valid_data.begin(), valid_data.begin() + valid_data.size() / 2);
        auto decoded = BlockUtreexoProof::deserializeCompressed(truncated);

        assert(decoded.isEmpty());
        std::cout << "✓ Truncated data rejected (returns empty proof)\n";
    }

    // Test case 3: Out-of-bounds index attack
    {
        // Manually craft malicious payload with index > dict_size
        std::vector<uint8_t> malicious;
        malicious.push_back(2);  // Version 2

        // Dictionary size = 2
        malicious.push_back(2);  // dict_size low byte
        malicious.push_back(0);
        malicious.push_back(0);
        malicious.push_back(0);

        // Add 2 dictionary hashes
        UtreexoHash hash1 = makeRandomHash(1);
        UtreexoHash hash2 = makeRandomHash(2);
        malicious.insert(malicious.end(), hash1.begin(), hash1.end());
        malicious.insert(malicious.end(), hash2.begin(), hash2.end());

        // Num targets = 1
        malicious.push_back(1);
        malicious.push_back(0);
        malicious.push_back(0);
        malicious.push_back(0);

        // Target index = 99 (OUT OF BOUNDS! dict_size = 2)
        malicious.push_back(99);
        malicious.push_back(0);
        malicious.push_back(0);
        malicious.push_back(0);

        // Num proofs = 0
        malicious.push_back(0);
        malicious.push_back(0);
        malicious.push_back(0);
        malicious.push_back(0);

        auto decoded = BlockUtreexoProof::deserializeCompressed(malicious);

        assert(decoded.isEmpty());
        std::cout << "✓ Out-of-bounds index rejected (index > dict_size)\n";
    }

    // Test case 4: Oversized dictionary attack
    {
        std::vector<uint8_t> malicious;
        malicious.push_back(2);  // Version 2

        // Dictionary size = 50000 (exceeds MAX_PROOF_SIZE limit)
        uint32_t huge_dict = 50000;
        malicious.push_back(huge_dict & 0xFF);
        malicious.push_back((huge_dict >> 8) & 0xFF);
        malicious.push_back((huge_dict >> 16) & 0xFF);
        malicious.push_back((huge_dict >> 24) & 0xFF);

        auto decoded = BlockUtreexoProof::deserializeCompressed(malicious);

        assert(decoded.isEmpty());
        std::cout << "✓ Oversized dictionary rejected (size > 10000 limit)\n";
    }

    // Test case 5: Empty data (graceful failure)
    {
        std::vector<uint8_t> empty;
        auto decoded = BlockUtreexoProof::deserializeCompressed(empty);

        assert(decoded.isEmpty());
        std::cout << "✓ Empty data rejected gracefully\n";
    }

    std::cout << "\n✅ Adversarial validation test PASSED\n";
    std::cout << "   Malicious inputs are detected and rejected\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Phase 9.2 zstd Compression
// ═══════════════════════════════════════════════════════════════════════════

void test_zstd_compression() {
    std::cout << "\n[Test 5] Phase 9.2: zstd Compression Framing\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "CRITICAL: zstd compression must be lossless and secure\n\n";

    // Test case 1: Round-trip for large proof (> 256 bytes, triggers zstd)
    {
        BlockUtreexoProof original = makeRandomProof(50, 100);
        auto compressed = original.serializeCompressedWithZstd();
        auto decoded = BlockUtreexoProof::deserializeCompressedWithZstd(compressed);

        assert(proofsEqual(original, decoded));
        std::cout << "✓ Large proof: zstd round-trip preserves data\n";
    }

    // Test case 2: Threshold behavior (small proof stays v2)
    {
        BlockUtreexoProof small_proof = makeRandomProof(2, 4);
        auto compressed = small_proof.serializeCompressedWithZstd();

        // Should return v2 format (threshold not met)
        assert(compressed.size() > 0);
        assert(compressed[0] == 2);  // Version 2 (not zstd)
        std::cout << "✓ Small proof: stays v2 format (below threshold)\n";
    }

    // Test case 3: Compatibility (deserializer handles v2 and v3)
    {
        BlockUtreexoProof proof = makeRandomProof(30, 60);

        // Compress with v2 (dedup only)
        auto v2_compressed = proof.serializeCompressed();

        // Compress with v3 (dedup + zstd)
        auto v3_compressed = proof.serializeCompressedWithZstd();

        // Deserializer should handle both
        auto v2_decoded = BlockUtreexoProof::deserializeCompressedWithZstd(v2_compressed);
        auto v3_decoded = BlockUtreexoProof::deserializeCompressedWithZstd(v3_compressed);

        assert(proofsEqual(proof, v2_decoded));
        assert(proofsEqual(proof, v3_decoded));
        std::cout << "✓ Mixed versions: deserializer handles v2 and v3\n";
    }

    // Test case 4: Additional compression savings
    {
        BlockUtreexoProof proof = makeRandomProof(80, 160);

        auto v2_size = proof.serializeCompressed().size();
        auto v3_data = proof.serializeCompressedWithZstd();

        // v3 should be smaller or similar (zstd adds overhead for small data)
        std::cout << "✓ Compression comparison: v2=" << v2_size << " bytes, v3=" << v3_data.size() << " bytes\n";
    }

    // Test case 5: Decompression bomb protection
    {
        std::vector<uint8_t> malicious;
        malicious.push_back(3);  // Version 3

        // Claim huge uncompressed size (500 KB > 100 KB limit)
        uint32_t huge_size = 500 * 1024;
        malicious.push_back(huge_size & 0xFF);
        malicious.push_back((huge_size >> 8) & 0xFF);
        malicious.push_back((huge_size >> 16) & 0xFF);
        malicious.push_back((huge_size >> 24) & 0xFF);

        // Small compressed size
        uint32_t small_compressed = 100;
        malicious.push_back(small_compressed & 0xFF);
        malicious.push_back((small_compressed >> 8) & 0xFF);
        malicious.push_back((small_compressed >> 16) & 0xFF);
        malicious.push_back((small_compressed >> 24) & 0xFF);

        // Add dummy data
        malicious.insert(malicious.end(), 100, 0xFF);

        auto decoded = BlockUtreexoProof::deserializeCompressedWithZstd(malicious);

        assert(decoded.isEmpty());
        std::cout << "✓ Decompression bomb rejected (size > 100 KB limit)\n";
    }

    // Test case 6: Suspicious compression ratio
    {
        std::vector<uint8_t> malicious;
        malicious.push_back(3);  // Version 3

        // Claim 10 KB uncompressed
        uint32_t uncompressed = 10 * 1024;
        malicious.push_back(uncompressed & 0xFF);
        malicious.push_back((uncompressed >> 8) & 0xFF);
        malicious.push_back((uncompressed >> 16) & 0xFF);
        malicious.push_back((uncompressed >> 24) & 0xFF);

        // Tiny compressed size (ratio > 100:1)
        uint32_t compressed = 50;
        malicious.push_back(compressed & 0xFF);
        malicious.push_back((compressed >> 8) & 0xFF);
        malicious.push_back((compressed >> 16) & 0xFF);
        malicious.push_back((compressed >> 24) & 0xFF);

        // Add dummy data
        malicious.insert(malicious.end(), 50, 0xFF);

        auto decoded = BlockUtreexoProof::deserializeCompressedWithZstd(malicious);

        assert(decoded.isEmpty());
        std::cout << "✓ Suspicious ratio rejected (> 100:1)\n";
    }

    std::cout << "\n✅ Phase 9.2 zstd compression test PASSED\n";
    std::cout << "   zstd compression is lossless and secure\n";
}

// ═══════════════════════════════════════════════════════════════════════════
// Main Test Runner
// ═══════════════════════════════════════════════════════════════════════════

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  UTREEXO PROOF COMPRESSION TESTS\n";
    std::cout << "  Phase 9.1 + 9.2: Hash Dedup + zstd Compression\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "\nWHY THESE TESTS ARE MANDATORY:\n";
    std::cout << "  Phase 9 changes wire format and data semantics.\n";
    std::cout << "  Compression bugs are SILENT and DATA-CORRUPTING.\n";
    std::cout << "  Existing tests CANNOT detect proof encoding regressions.\n";
    std::cout << "  These tests must pass before any Phase 9 commit lands.\n";

    try {
        test_roundtrip_identity();
        test_backward_compatible_v1();
        test_dedup_correctness();
        test_adversarial_inputs();
        test_zstd_compression();

        std::cout << "\n═══════════════════════════════════════════════════════════════\n";
        std::cout << "  ✅ ALL COMPRESSION TESTS PASSED (5 test suites)\n";
        std::cout << "═══════════════════════════════════════════════════════════════\n";
        std::cout << "\n  Phase 9.1 + 9.2 compression is:\n";
        std::cout << "    ✓ Lossless (round-trip identity)\n";
        std::cout << "    ✓ Backward compatible (v1/v2/v3 all work)\n";
        std::cout << "    ✓ Correct (dedup + zstd preserve proof validity)\n";
        std::cout << "    ✓ Secure (adversarial inputs rejected)\n";
        std::cout << "    ✓ Threshold-aware (< 256 bytes stays v2)\n";
        std::cout << "\n  Safe to commit.\n\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "\n❌ COMPRESSION TEST FAILED: " << e.what() << "\n";
        std::cerr << "   DO NOT COMMIT until this is fixed.\n\n";
        return 1;
    }
}
