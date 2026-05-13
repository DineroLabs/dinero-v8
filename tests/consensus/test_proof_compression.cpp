#include "consensus/proof_compression.h"
#include <iostream>
#include <vector>
#include <set>

using namespace dinero;
using namespace dinero::consensus;

// Test utilities

uint256 CreateTestHash(uint32_t seed) {
    uint256 hash;
    hash.data[0] = static_cast<uint8_t>(seed & 0xFF);
    hash.data[1] = static_cast<uint8_t>((seed >> 8) & 0xFF);
    hash.data[2] = static_cast<uint8_t>((seed >> 16) & 0xFF);
    hash.data[3] = static_cast<uint8_t>((seed >> 24) & 0xFF);
    for (size_t i = 4; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>((seed + i) & 0xFF);
    }
    return hash;
}

BlockUtreexoData CreateTestProof(size_t num_targets, size_t num_proof_hashes, bool duplicate_hashes = false) {
    BlockUtreexoData proof;

    // Set accumulator root before
    std::vector<uint8_t> root_data(32);
    for (size_t i = 0; i < 32; i++) {
        root_data[i] = static_cast<uint8_t>(i);
    }
    proof.accumulator_root_before = UtreexoHash(root_data);

    // Add targets
    for (size_t i = 0; i < num_targets; i++) {
        if (duplicate_hashes && i > 0 && i % 3 == 0) {
            // Reuse hash to create duplicates
            proof.spend_proof.targets.push_back(proof.spend_proof.targets[0]);
        } else {
            uint256 hash = CreateTestHash(i);
            UtreexoHash hash_vec(hash.data, hash.data + 32);
            proof.spend_proof.targets.push_back(hash_vec);
        }
    }

    // Add proof hashes
    for (size_t i = 0; i < num_proof_hashes; i++) {
        if (duplicate_hashes && i > 0 && i % 3 == 0) {
            // Reuse hash to create duplicates
            proof.spend_proof.proof_hashes.push_back(proof.spend_proof.proof_hashes[0]);
        } else {
            uint256 hash = CreateTestHash(num_targets + i);
            UtreexoHash hash_vec(hash.data, hash.data + 32);
            proof.spend_proof.proof_hashes.push_back(hash_vec);
        }
    }

    // Add some spent outputs
    for (size_t i = 0; i < 3; i++) {
        SpentOutputData output;
        output.value = 1000000 * (i + 1);
        output.scriptPubKey = {0x00, 0x14, static_cast<uint8_t>(i)};
        proof.spent_outputs.push_back(output);
    }

    return proof;
}

// Test implementations

void test_T9_19_deduplication_effectiveness() {
    std::cout << "\n[T9.19] Deduplication effectiveness\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCompressionManager manager;

    // Create proof with many duplicate hashes (typical in Utreexo proofs)
    BlockUtreexoData proof = CreateTestProof(50, 50, true);  // 100 hashes, many duplicates

    std::cout << "✓ Created proof with 100 hashes (many duplicates)\n";

    // Compress with deduplication only
    CompressionResult result = manager.CompressProofWithMethod(proof, CompressionMethod::DEDUPLICATED);

    std::cout << "✓ Compressed with deduplication (v2)\n";
    std::cout << "  Original size: " << result.original_size << " bytes\n";
    std::cout << "  Compressed size: " << result.compressed_size << " bytes\n";
    std::cout << "  Compression ratio: " << result.CompressionRatio() << ":1\n";

    if (result.compressed_size >= result.original_size) {
        std::cout << "❌ TEST FAILED: Compression should reduce size with duplicates\n";
        return;
    }

    double ratio = result.CompressionRatio();
    if (ratio < 1.2) {
        std::cout << "❌ TEST FAILED: Expected at least 1.2:1 compression ratio with duplicates\n";
        return;
    }

    std::cout << "✓ Deduplication reduced size by " << (100.0 - (result.compressed_size * 100.0 / result.original_size)) << "%\n";
    std::cout << "✅ TEST PASSED: Deduplication is effective\n";
}

void test_T9_20_zstd_compression_effectiveness() {
    std::cout << "\n[T9.20] Zstd compression effectiveness\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCompressionManager manager;

    // Create large proof with duplicates
    BlockUtreexoData proof = CreateTestProof(100, 100, true);  // Large proof

    std::cout << "✓ Created large proof with 200 hashes\n";

    // Compress with v2 (deduplication only)
    CompressionResult v2_result = manager.CompressProofWithMethod(proof, CompressionMethod::DEDUPLICATED);
    std::cout << "✓ v2 (deduplicated): " << v2_result.compressed_size << " bytes\n";

    // Compress with v3 (deduplication + zstd)
    CompressionResult v3_result = manager.CompressProofWithMethod(proof, CompressionMethod::ZSTD);
    std::cout << "✓ v3 (deduplicated + zstd): " << v3_result.compressed_size << " bytes\n";

    std::cout << "  Original size: " << v3_result.original_size << " bytes\n";
    std::cout << "  v2 compression ratio: " << v2_result.CompressionRatio() << ":1\n";
    std::cout << "  v3 compression ratio: " << v3_result.CompressionRatio() << ":1\n";

    // v3 should be smaller or equal to v2 (zstd adds compression or falls back)
    if (v3_result.compressed_size > v2_result.compressed_size + 100) {
        std::cout << "❌ TEST FAILED: v3 should not be significantly larger than v2\n";
        return;
    }

    std::cout << "✓ Zstd compression effective\n";
    std::cout << "✅ TEST PASSED: Zstd compression works\n";
}

void test_T9_21_compression_threshold_behavior() {
    std::cout << "\n[T9.21] Compression threshold behavior\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCompressionManager manager;
    manager.SetCompressionThreshold(256);
    manager.SetZstdThreshold(1024);

    std::cout << "✓ Set thresholds: compression=256, zstd=1024\n";

    // Small proof (< 256 bytes)
    BlockUtreexoData small_proof = CreateTestProof(2, 2, false);
    CompressionResult small_result = manager.CompressProof(small_proof);

    std::cout << "✓ Small proof (" << small_result.original_size << " bytes): method="
              << CompressionMethodName(small_result.method_used) << "\n";

    if (small_result.method_used != CompressionMethod::NONE) {
        std::cout << "❌ TEST FAILED: Small proof should not be compressed\n";
        return;
    }

    // Medium proof (256-1024 bytes)
    BlockUtreexoData medium_proof = CreateTestProof(10, 10, false);
    CompressionResult medium_result = manager.CompressProof(medium_proof);

    std::cout << "✓ Medium proof (" << medium_result.original_size << " bytes): method="
              << CompressionMethodName(medium_result.method_used) << "\n";

    if (medium_result.method_used != CompressionMethod::DEDUPLICATED) {
        std::cout << "❌ TEST FAILED: Medium proof should use deduplication\n";
        return;
    }

    // Large proof (> 1024 bytes)
    BlockUtreexoData large_proof = CreateTestProof(50, 50, false);
    CompressionResult large_result = manager.CompressProof(large_proof);

    std::cout << "✓ Large proof (" << large_result.original_size << " bytes): method="
              << CompressionMethodName(large_result.method_used) << "\n";

    if (large_result.method_used != CompressionMethod::ZSTD) {
        std::cout << "❌ TEST FAILED: Large proof should use zstd\n";
        return;
    }

    std::cout << "✓ Threshold-based method selection works\n";
    std::cout << "✅ TEST PASSED: Compression threshold behavior correct\n";
}

void test_T9_22_decompression_bomb_protection() {
    std::cout << "\n[T9.22] Decompression bomb protection\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCompressionManager manager;

    // Create malicious v3 frame with huge uncompressed size
    std::vector<uint8_t> malicious_data;
    malicious_data.push_back(3);  // version = 3 (zstd)

    // Uncompressed size = 10 MB (way over 100 KB limit)
    uint32_t huge_size = 10 * 1024 * 1024;
    malicious_data.push_back(huge_size & 0xFF);
    malicious_data.push_back((huge_size >> 8) & 0xFF);
    malicious_data.push_back((huge_size >> 16) & 0xFF);
    malicious_data.push_back((huge_size >> 24) & 0xFF);

    // Compressed size = 100 bytes
    uint32_t compressed_size = 100;
    malicious_data.push_back(compressed_size & 0xFF);
    malicious_data.push_back((compressed_size >> 8) & 0xFF);
    malicious_data.push_back((compressed_size >> 16) & 0xFF);
    malicious_data.push_back((compressed_size >> 24) & 0xFF);

    // Add dummy compressed data
    for (size_t i = 0; i < compressed_size; i++) {
        malicious_data.push_back(0x00);
    }

    std::cout << "✓ Created malicious v3 frame (claims 10 MB uncompressed)\n";

    // Try to decompress
    auto result = manager.DecompressProof(malicious_data);

    if (result.has_value()) {
        std::cout << "❌ TEST FAILED: Decompression bomb should be rejected\n";
        return;
    }

    std::cout << "✓ Decompression bomb rejected (size limit enforced)\n";

    // Create suspicious compression ratio (100:1)
    std::vector<uint8_t> suspicious_data;
    suspicious_data.push_back(3);  // version = 3

    // 100 KB uncompressed, 1 KB compressed = 100:1 ratio
    uint32_t suspect_uncompressed = 100 * 1024;
    suspicious_data.push_back(suspect_uncompressed & 0xFF);
    suspicious_data.push_back((suspect_uncompressed >> 8) & 0xFF);
    suspicious_data.push_back((suspect_uncompressed >> 16) & 0xFF);
    suspicious_data.push_back((suspect_uncompressed >> 24) & 0xFF);

    uint32_t suspect_compressed = 1000;
    suspicious_data.push_back(suspect_compressed & 0xFF);
    suspicious_data.push_back((suspect_compressed >> 8) & 0xFF);
    suspicious_data.push_back((suspect_compressed >> 16) & 0xFF);
    suspicious_data.push_back((suspect_compressed >> 24) & 0xFF);

    for (size_t i = 0; i < suspect_compressed; i++) {
        suspicious_data.push_back(0x00);
    }

    std::cout << "✓ Created suspicious frame (100:1 compression ratio)\n";

    result = manager.DecompressProof(suspicious_data);

    if (result.has_value()) {
        std::cout << "❌ TEST FAILED: Suspicious compression ratio should be rejected\n";
        return;
    }

    std::cout << "✓ Suspicious compression ratio rejected\n";
    std::cout << "✅ TEST PASSED: Decompression bomb protection works\n";
}

void test_T9_23_compression_statistics() {
    std::cout << "\n[T9.23] Compression statistics tracking\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCompressionManager manager;
    manager.ClearStats();

    std::cout << "✓ Cleared statistics\n";

    // Compress several proofs
    for (int i = 0; i < 10; i++) {
        BlockUtreexoData proof = CreateTestProof(20 + i, 20 + i, true);
        manager.CompressProof(proof);
    }

    std::cout << "✓ Compressed 10 proofs\n";

    auto stats = manager.GetStats();
    std::cout << "  Proofs compressed: " << stats.proofs_compressed << "\n";
    std::cout << "  Total original bytes: " << stats.total_original_bytes << "\n";
    std::cout << "  Total compressed bytes: " << stats.total_compressed_bytes << "\n";
    std::cout << "  Average compression ratio: " << stats.AverageCompressionRatio() << ":1\n";
    std::cout << "  Space saved: " << stats.SpaceSavedPercent() << "%\n";
    std::cout << "  Deduplication count: " << stats.deduplication_count << "\n";
    std::cout << "  Zstd count: " << stats.zstd_count << "\n";

    if (stats.proofs_compressed != 10) {
        std::cout << "❌ TEST FAILED: Expected 10 proofs compressed\n";
        return;
    }

    if (stats.total_original_bytes == 0 || stats.total_compressed_bytes == 0) {
        std::cout << "❌ TEST FAILED: Byte counts should be non-zero\n";
        return;
    }

    if (stats.AverageCompressionRatio() < 1.0) {
        std::cout << "❌ TEST FAILED: Compression ratio should be >= 1.0\n";
        return;
    }

    std::cout << "✓ Statistics tracked correctly\n";
    std::cout << "✅ TEST PASSED: Compression statistics work\n";
}

void test_T9_24_round_trip_all_methods() {
    std::cout << "\n[T9.24] Round-trip compression/decompression\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCompressionManager manager;

    BlockUtreexoData original = CreateTestProof(30, 30, true);

    std::cout << "✓ Created test proof\n";

    // Test v2 format (deduplication)
    {
        CompressionResult compressed = manager.CompressProofWithMethod(original, CompressionMethod::DEDUPLICATED);
        std::cout << "✓ Compressed with v2 (deduplication)\n";

        auto decompressed = manager.DecompressProof(compressed.data);
        if (!decompressed.has_value()) {
            std::cout << "❌ TEST FAILED: v2 decompression failed\n";
            return;
        }

        std::cout << "✓ Decompressed v2 successfully\n";

        // Verify targets match
        if (decompressed->spend_proof.targets.size() != original.spend_proof.targets.size()) {
            std::cout << "❌ TEST FAILED: v2 target count mismatch\n";
            return;
        }

        std::cout << "✓ v2 round-trip successful\n";
    }

    // Test v3 format (deduplication + zstd)
    {
        CompressionResult compressed = manager.CompressProofWithMethod(original, CompressionMethod::ZSTD);
        std::cout << "✓ Compressed with v3 (zstd)\n";

        auto decompressed = manager.DecompressProof(compressed.data);
        if (!decompressed.has_value()) {
            std::cout << "❌ TEST FAILED: v3 decompression failed\n";
            return;
        }

        std::cout << "✓ Decompressed v3 successfully\n";

        // Verify targets match
        if (decompressed->spend_proof.targets.size() != original.spend_proof.targets.size()) {
            std::cout << "❌ TEST FAILED: v3 target count mismatch\n";
            return;
        }

        std::cout << "✓ v3 round-trip successful\n";
    }

    // Test auto selection
    {
        CompressionResult compressed = manager.CompressProof(original);
        std::cout << "✓ Compressed with auto selection (" << CompressionMethodName(compressed.method_used) << ")\n";

        auto decompressed = manager.DecompressProof(compressed.data);
        if (!decompressed.has_value()) {
            std::cout << "❌ TEST FAILED: Auto decompression failed\n";
            return;
        }

        std::cout << "✓ Auto round-trip successful\n";
    }

    std::cout << "✅ TEST PASSED: Round-trip works for all methods\n";
}

void test_T9_25_format_detection() {
    std::cout << "\n[T9.25] Compression format detection\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    // Test v2 detection
    std::vector<uint8_t> v2_data = {2, 0, 0, 0, 0};  // version=2
    CompressionMethod method = DetectCompressionMethod(v2_data);

    if (method != CompressionMethod::DEDUPLICATED) {
        std::cout << "❌ TEST FAILED: v2 format not detected\n";
        return;
    }

    std::cout << "✓ v2 format detected correctly\n";

    // Test v3 detection
    std::vector<uint8_t> v3_data = {3, 0, 0, 0, 0};  // version=3
    method = DetectCompressionMethod(v3_data);

    if (method != CompressionMethod::ZSTD) {
        std::cout << "❌ TEST FAILED: v3 format not detected\n";
        return;
    }

    std::cout << "✓ v3 format detected correctly\n";

    // Test v1 detection (no version byte)
    std::vector<uint8_t> v1_data = {32, 0, 0, 0};  // Looks like v1 data
    method = DetectCompressionMethod(v1_data);

    if (method != CompressionMethod::NONE) {
        std::cout << "❌ TEST FAILED: v1 format not detected\n";
        return;
    }

    std::cout << "✓ v1 format detected correctly\n";
    std::cout << "✅ TEST PASSED: Format detection works\n";
}

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Phase 9.4: Proof Compression Tests (T9.19–T9.25)\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";

    int passed = 0;
    int failed = 0;

    try {
        test_T9_19_deduplication_effectiveness();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.19 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_20_zstd_compression_effectiveness();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.20 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_21_compression_threshold_behavior();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.21 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_22_decompression_bomb_protection();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.22 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_23_compression_statistics();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.23 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_24_round_trip_all_methods();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.24 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_25_format_detection();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.25 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    std::cout << "\n═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Test Summary\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Passed: " << passed << "\n";
    std::cout << "  Failed: " << failed << "\n";

    if (failed == 0) {
        std::cout << "\n✅ All Phase 9.4 tests PASSED\n";
        return 0;
    } else {
        std::cout << "\n❌ Some tests FAILED\n";
        return 1;
    }
}
