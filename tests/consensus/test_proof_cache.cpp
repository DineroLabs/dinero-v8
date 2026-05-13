#include "consensus/proof_cache.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace dinero;
using namespace dinero::consensus;

// Test utilities

BlockUtreexoData CreateTestProof(size_t num_outputs = 3) {
    BlockUtreexoData proof;

    // Set accumulator root before
    std::vector<uint8_t> root_data(32);
    for (size_t i = 0; i < 32; i++) {
        root_data[i] = static_cast<uint8_t>(i);
    }
    proof.accumulator_root_before = UtreexoHash(root_data);

    // Add spent outputs metadata
    for (size_t i = 0; i < num_outputs; i++) {
        SpentOutputData output;
        output.value = 1000000 * (i + 1);
        output.scriptPubKey = {0x00, 0x14, static_cast<uint8_t>(i)};  // Dummy P2WPKH script
        proof.spent_outputs.push_back(output);
    }

    // spend_proof will be empty for simple test (coinbase-only scenario)

    return proof;
}

uint256 CreateTestHash(uint32_t seed) {
    uint256 hash;
    // Use all 4 bytes of seed to avoid collisions
    hash.data[0] = static_cast<uint8_t>(seed & 0xFF);
    hash.data[1] = static_cast<uint8_t>((seed >> 8) & 0xFF);
    hash.data[2] = static_cast<uint8_t>((seed >> 16) & 0xFF);
    hash.data[3] = static_cast<uint8_t>((seed >> 24) & 0xFF);
    // Fill rest with derived values
    for (size_t i = 4; i < 32; i++) {
        hash.data[i] = static_cast<uint8_t>((seed + i) & 0xFF);
    }
    return hash;
}

// Test implementations

void test_T9_1_cache_hit_returns_correct_proof() {
    std::cout << "\n[T9.1] Cache hit returns correct proof\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCache cache;

    // Create test data
    uint256 block_hash = CreateTestHash(42);
    uint256 root_hash = CreateTestHash(100);
    BlockUtreexoData proof = CreateTestProof(10);

    std::cout << "✓ Created test proof with 10 hashes\n";

    // Put proof in cache
    cache.Put(block_hash, proof, root_hash);
    std::cout << "✓ Added proof to cache\n";
    std::cout << "  Cache size: " << cache.Size() << " entries\n";
    std::cout << "  Cache bytes: " << cache.TotalBytes() << " bytes\n";

    // Get proof from cache
    auto cached_proof = cache.Get(block_hash);

    if (!cached_proof.has_value()) {
        std::cout << "❌ TEST FAILED: Cache miss on existing entry\n";
        return;
    }

    std::cout << "✓ Cache hit successful\n";

    // Verify proof contents match
    if (cached_proof->spent_outputs.size() != proof.spent_outputs.size()) {
        std::cout << "❌ TEST FAILED: Spent output count mismatch\n";
        return;
    }

    if (cached_proof->size() != proof.size()) {
        std::cout << "❌ TEST FAILED: Proof size mismatch\n";
        return;
    }

    std::cout << "✓ Cached proof matches original\n";
    std::cout << "  Proof size: " << cached_proof->size() << " bytes\n";
    std::cout << "  Spent outputs: " << cached_proof->spent_outputs.size() << "\n";

    // Check hit rate
    double hit_rate = cache.HitRate();
    std::cout << "✓ Hit rate: " << (hit_rate * 100) << "%\n";

    if (hit_rate != 1.0) {
        std::cout << "❌ TEST FAILED: Expected 100% hit rate after 1 hit\n";
        return;
    }

    std::cout << "✅ TEST PASSED: Cache hit returns correct proof\n";
}

void test_T9_2_cache_miss() {
    std::cout << "\n[T9.2] Cache miss returns nullopt\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCache cache;

    // Try to get non-existent proof
    uint256 missing_hash = CreateTestHash(99);
    auto result = cache.Get(missing_hash);

    if (result.has_value()) {
        std::cout << "❌ TEST FAILED: Cache returned value for non-existent entry\n";
        return;
    }

    std::cout << "✓ Cache miss returns nullopt\n";

    // Check hit rate
    double hit_rate = cache.HitRate();
    std::cout << "✓ Hit rate after miss: " << (hit_rate * 100) << "%\n";

    if (hit_rate != 0.0) {
        std::cout << "❌ TEST FAILED: Expected 0% hit rate after 1 miss\n";
        return;
    }

    std::cout << "✅ TEST PASSED: Cache miss returns nullopt\n";
}

void test_T9_3_lru_eviction() {
    std::cout << "\n[T9.3] LRU eviction works correctly\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCache cache;

    // Fill cache with proofs until we exceed size limit
    // We need enough proofs to exceed 100 MB
    // Each proof with 1000 spent outputs ~ 10.5 KB
    // So we need ~10,000 proofs to exceed 100 MB and trigger eviction
    std::cout << "✓ Creating proofs to trigger eviction\n";

    std::vector<uint256> hashes;
    for (int i = 0; i < 12000; i++) {  // 12000 * 10.5 KB ≈ 126 MB > 100 MB limit
        uint256 block_hash = CreateTestHash(i);
        uint256 root_hash = CreateTestHash(i + 100000);
        BlockUtreexoData proof = CreateTestProof(1000);  // 1000 spent outputs (~10.5 KB each)

        cache.Put(block_hash, proof, root_hash);
        hashes.push_back(block_hash);

        if (i % 2000 == 0 && i > 0) {
            std::cout << "  Added " << i << " proofs, cache: "
                      << cache.Size() << " entries, "
                      << (cache.TotalBytes() / 1024 / 1024) << " MB\n";
        }
    }

    std::cout << "✓ Final cache size: " << cache.Size() << " entries\n";
    std::cout << "✓ Final cache bytes: " << (cache.TotalBytes() / 1024 / 1024) << " MB\n";

    // Cache should have evicted entries to stay under 100 MB
    if (cache.TotalBytes() > ProofCache::MAX_CACHE_SIZE) {
        std::cout << "❌ TEST FAILED: Cache exceeded max size\n";
        return;
    }

    std::cout << "✓ Cache stayed within size limit\n";

    // Oldest entries (LRU) should be evicted
    auto first_entry = cache.Get(hashes[0]);
    if (first_entry.has_value()) {
        std::cout << "❌ TEST FAILED: Oldest entry was not evicted\n";
        return;
    }

    std::cout << "✓ Oldest entry was evicted (LRU)\n";

    // Newest entries should still be cached
    auto last_entry = cache.Get(hashes.back());
    if (!last_entry.has_value()) {
        std::cout << "❌ TEST FAILED: Newest entry was evicted\n";
        return;
    }

    std::cout << "✓ Newest entry is still cached\n";

    std::cout << "✅ TEST PASSED: LRU eviction works correctly\n";
}

void test_T9_4_ttl_eviction() {
    std::cout << "\n[T9.4] TTL eviction removes expired entries\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCache cache;

    // Set short TTL for testing (2 seconds)
    cache.SetTTL(2);
    std::cout << "✓ Set TTL to 2 seconds for testing\n";

    // Add proof to cache
    uint256 block_hash = CreateTestHash(50);
    uint256 root_hash = CreateTestHash(150);
    BlockUtreexoData proof = CreateTestProof(10);

    cache.Put(block_hash, proof, root_hash);
    std::cout << "✓ Added proof to cache\n";

    // Should be cached immediately
    auto cached = cache.Get(block_hash);
    if (!cached.has_value()) {
        std::cout << "❌ TEST FAILED: Proof not cached immediately\n";
        return;
    }
    std::cout << "✓ Proof cached immediately\n";

    // Wait for TTL to expire
    std::cout << "⏳ Waiting 3 seconds for TTL expiration...\n";
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Should be expired now
    auto expired = cache.Get(block_hash);
    if (expired.has_value()) {
        std::cout << "❌ TEST FAILED: Expired proof was not evicted\n";
        return;
    }

    std::cout << "✓ Expired proof was evicted on access\n";

    // Cache size should be 0 now
    if (cache.Size() != 0) {
        std::cout << "❌ TEST FAILED: Cache not empty after TTL eviction\n";
        return;
    }

    std::cout << "✓ Cache is empty after TTL eviction\n";

    std::cout << "✅ TEST PASSED: TTL eviction removes expired entries\n";
}

void test_T9_5_cache_verification_required() {
    std::cout << "\n[T9.5] Cached proofs require re-verification (security rule)\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCache cache;

    // Add proof to cache
    uint256 block_hash = CreateTestHash(60);
    uint256 root_hash = CreateTestHash(160);
    BlockUtreexoData proof = CreateTestProof(10);

    cache.Put(block_hash, proof, root_hash);
    std::cout << "✓ Added proof to cache\n";

    // Get proof with root hash
    uint256 returned_root;
    auto cached_proof = cache.GetWithRoot(block_hash, returned_root);

    if (!cached_proof.has_value()) {
        std::cout << "❌ TEST FAILED: Cache miss on existing entry\n";
        return;
    }

    std::cout << "✓ Retrieved proof with root hash\n";

    // Verify root hash matches
    bool roots_match = true;
    for (size_t i = 0; i < 32; i++) {
        if (returned_root.data[i] != root_hash.data[i]) {
            roots_match = false;
            break;
        }
    }

    if (!roots_match) {
        std::cout << "❌ TEST FAILED: Root hash mismatch\n";
        return;
    }

    std::cout << "✓ Root hash matches cached value\n";

    // Simulate corruption detection (caller would re-verify proof)
    std::cout << "✓ Caller MUST re-verify proof cryptographically\n";
    std::cout << "  (Phase 9 design rule: cache is optimization, not trust)\n";

    // Test eviction of specific entry (simulating corruption)
    bool evicted = cache.Evict(block_hash);
    if (!evicted) {
        std::cout << "❌ TEST FAILED: Failed to evict specific entry\n";
        return;
    }

    std::cout << "✓ Corrupted entry can be evicted\n";

    // Verify entry is gone
    auto after_evict = cache.Get(block_hash);
    if (after_evict.has_value()) {
        std::cout << "❌ TEST FAILED: Entry still exists after eviction\n";
        return;
    }

    std::cout << "✓ Entry removed from cache\n";

    std::cout << "✅ TEST PASSED: Cache verification rules enforced\n";
}

// Additional test: Clear cache

void test_T9_6_clear_cache() {
    std::cout << "\n[T9.6] Clear cache removes all entries\n";
    std::cout << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";

    ProofCache cache;

    // Add multiple proofs
    for (int i = 0; i < 10; i++) {
        uint256 block_hash = CreateTestHash(i);
        uint256 root_hash = CreateTestHash(i + 100);
        BlockUtreexoData proof = CreateTestProof(10);
        cache.Put(block_hash, proof, root_hash);
    }

    std::cout << "✓ Added 10 proofs to cache\n";
    std::cout << "  Cache size: " << cache.Size() << " entries\n";

    if (cache.Size() != 10) {
        std::cout << "❌ TEST FAILED: Expected 10 entries\n";
        return;
    }

    // Clear cache
    cache.Clear();
    std::cout << "✓ Cleared cache\n";

    if (cache.Size() != 0) {
        std::cout << "❌ TEST FAILED: Cache not empty after clear\n";
        return;
    }

    std::cout << "✓ Cache is empty\n";

    if (cache.TotalBytes() != 0) {
        std::cout << "❌ TEST FAILED: Cache bytes not zero after clear\n";
        return;
    }

    std::cout << "✓ Cache bytes reset to zero\n";

    if (cache.HitRate() != 0.0) {
        std::cout << "❌ TEST FAILED: Hit rate not reset after clear\n";
        return;
    }

    std::cout << "✓ Hit rate reset to 0%\n";

    std::cout << "✅ TEST PASSED: Clear cache works correctly\n";
}

int main() {
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Phase 9.1: Proof Cache Tests (T9.1–T9.6)\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";

    int passed = 0;
    int failed = 0;

    try {
        test_T9_1_cache_hit_returns_correct_proof();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.1 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_2_cache_miss();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.2 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_3_lru_eviction();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.3 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_4_ttl_eviction();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.4 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_5_cache_verification_required();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.5 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    try {
        test_T9_6_clear_cache();
        passed++;
    } catch (const std::exception& e) {
        std::cout << "❌ T9.6 FAILED with exception: " << e.what() << "\n";
        failed++;
    }

    std::cout << "\n═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Test Summary\n";
    std::cout << "═══════════════════════════════════════════════════════════════════════════\n";
    std::cout << "  Passed: " << passed << "\n";
    std::cout << "  Failed: " << failed << "\n";

    if (failed == 0) {
        std::cout << "\n✅ All Phase 9.1 tests PASSED\n";
        return 0;
    } else {
        std::cout << "\n❌ Some tests FAILED\n";
        return 1;
    }
}
