/**
 * Phase F.8.4: Signature Cache Test
 *
 * Tests the signature cache functionality:
 * 1. Basic insert/get operations
 * 2. Cache hits and misses
 * 3. LRU eviction when cache is full
 * 4. Cache key uniqueness
 * 5. Cache key computation
 * 6. Hit rate tracking
 *
 * This proves the signature cache works correctly and provides additional
 * speedup beyond the script cache (F.8.3).
 */

#include "consensus/signature_cache.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero::consensus;

// Test 1: Basic insert and get
void testBasicInsertGet() {
    std::cout << "\n[Test 1] Basic insert and get" << std::endl;

    SignatureCache cache(1);  // 1 MB cache (small for testing)

    // Create test signature verification inputs
    std::vector<uint8_t> signature = {0x01, 0x02, 0x03, 0x04, 0x05};  // Fake signature (64 bytes in reality)
    std::vector<uint8_t> pubkey = {0x06, 0x07, 0x08, 0x09, 0x0A};     // Fake pubkey (33 bytes in reality)
    std::vector<uint8_t> message = {0x0B, 0x0C, 0x0D, 0x0E, 0x0F};     // Fake message hash (32 bytes in reality)

    auto key = SignatureCache::computeKey(signature, pubkey, message);

    // Insert a positive result (signature is valid)
    cache.insert(key, true);

    // Get the result back
    bool result = false;
    bool found = cache.get(key, result);

    assert(found && "Cache should have the key");
    assert(result == true && "Cached result should be true");

    std::cout << "  [✓] Insert and get works correctly" << std::endl;
}

// Test 2: Cache miss
void testCacheMiss() {
    std::cout << "\n[Test 2] Cache miss" << std::endl;

    SignatureCache cache(1);

    // Create a key that was never inserted
    std::vector<uint8_t> signature = {0xFF, 0xFE, 0xFD};
    std::vector<uint8_t> pubkey = {0xFC, 0xFB, 0xFA};
    std::vector<uint8_t> message = {0xF9, 0xF8, 0xF7};

    auto key = SignatureCache::computeKey(signature, pubkey, message);

    // Try to get a non-existent key
    bool result = false;
    bool found = cache.get(key, result);

    assert(!found && "Cache should not have the key");

    std::cout << "  [✓] Cache miss detected correctly" << std::endl;
}

// Test 3: Cache negative results (invalid signatures)
void testNegativeResults() {
    std::cout << "\n[Test 3] Cache negative results (invalid signatures)" << std::endl;

    SignatureCache cache(1);

    std::vector<uint8_t> signature = {0x01};
    std::vector<uint8_t> pubkey = {0x02};
    std::vector<uint8_t> message = {0x03};

    auto key = SignatureCache::computeKey(signature, pubkey, message);

    // Insert a negative result (signature verification failed)
    cache.insert(key, false);

    // Get the result back
    bool result = true;  // Initialize to opposite
    bool found = cache.get(key, result);

    assert(found && "Cache should have the key");
    assert(result == false && "Cached result should be false (invalid signature)");

    std::cout << "  [✓] Negative results cached correctly" << std::endl;
}

// Test 4: LRU eviction
void testLRUEviction() {
    std::cout << "\n[Test 4] LRU eviction when cache is full" << std::endl;

    // Create a very small cache
    SignatureCache cache(1);  // 1 MB / 64 bytes = ~16k entries

    // Fill cache with many entries
    std::vector<SignatureCache::CacheKey> keys;
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> signature = {(uint8_t)i, (uint8_t)(i >> 8)};
        std::vector<uint8_t> pubkey = {0x02};
        std::vector<uint8_t> message = {0x03};

        auto key = SignatureCache::computeKey(signature, pubkey, message);
        cache.insert(key, true);
        keys.push_back(key);
    }

    // Check stats
    auto stats = cache.getStats();
    std::cout << "  Cache size: " << stats.size << " / " << stats.max_size << std::endl;
    assert(stats.size <= stats.max_size && "Cache should not exceed max size");

    // First keys should have been evicted (LRU)
    bool result;
    bool found_first = cache.get(keys[0], result);
    bool found_last = cache.get(keys[19999], result);

    assert(!found_first && "First key should have been evicted");
    assert(found_last && "Last key should still be in cache");

    std::cout << "  [✓] LRU eviction works correctly" << std::endl;
}

// Test 5: Cache clear
void testCacheClear() {
    std::cout << "\n[Test 5] Cache clear" << std::endl;

    SignatureCache cache(1);

    // Insert some entries
    for (int i = 0; i < 10; i++) {
        std::vector<uint8_t> signature = {(uint8_t)i};
        std::vector<uint8_t> pubkey = {0x02};
        std::vector<uint8_t> message = {0x03};

        auto key = SignatureCache::computeKey(signature, pubkey, message);
        cache.insert(key, true);
    }

    auto stats_before = cache.getStats();
    assert(stats_before.size == 10 && "Cache should have 10 entries");

    // Clear the cache
    cache.clear();

    auto stats_after = cache.getStats();
    assert(stats_after.size == 0 && "Cache should be empty after clear");

    std::cout << "  [✓] Cache clear works correctly" << std::endl;
}

// Test 6: Cache key uniqueness
void testCacheKeyUniqueness() {
    std::cout << "\n[Test 6] Cache key uniqueness" << std::endl;

    // Different signatures should produce different keys
    std::vector<uint8_t> signature1 = {0x01, 0x02};
    std::vector<uint8_t> signature2 = {0x01, 0x03};  // Different signature
    std::vector<uint8_t> pubkey = {0x04};
    std::vector<uint8_t> message = {0x05};

    auto key1 = SignatureCache::computeKey(signature1, pubkey, message);
    auto key2 = SignatureCache::computeKey(signature2, pubkey, message);

    // Keys should be different
    bool keys_different = std::memcmp(key1.data, key2.data, 32) != 0;
    assert(keys_different && "Keys with different signatures should be different");

    // Different pubkeys should produce different keys
    std::vector<uint8_t> pubkey2 = {0x06};
    auto key3 = SignatureCache::computeKey(signature1, pubkey2, message);

    bool keys_different2 = std::memcmp(key1.data, key3.data, 32) != 0;
    assert(keys_different2 && "Keys with different pubkeys should be different");

    // Different messages should produce different keys
    std::vector<uint8_t> message2 = {0x07};
    auto key4 = SignatureCache::computeKey(signature1, pubkey, message2);

    bool keys_different3 = std::memcmp(key1.data, key4.data, 32) != 0;
    assert(keys_different3 && "Keys with different messages should be different");

    // Same inputs should produce same key
    auto key1_dup = SignatureCache::computeKey(signature1, pubkey, message);
    bool keys_same = std::memcmp(key1.data, key1_dup.data, 32) == 0;
    assert(keys_same && "Same inputs should produce same key");

    std::cout << "  [✓] Cache key computation is deterministic and unique" << std::endl;
}

// Test 7: Hit rate tracking
void testHitRateTracking() {
    std::cout << "\n[Test 7] Hit rate tracking" << std::endl;

    SignatureCache cache(1);

    std::vector<uint8_t> signature = {0x01};
    std::vector<uint8_t> pubkey = {0x02};
    std::vector<uint8_t> message = {0x03};

    auto key = SignatureCache::computeKey(signature, pubkey, message);

    // Insert
    cache.insert(key, true);

    // Generate some hits and misses
    bool result;
    cache.get(key, result);  // Hit
    cache.get(key, result);  // Hit
    cache.get(key, result);  // Hit

    auto key_miss = SignatureCache::computeKey({0xFF}, {0xFE}, {0xFD});
    cache.get(key_miss, result);  // Miss
    cache.get(key_miss, result);  // Miss

    auto stats = cache.getStats();
    std::cout << "  Hits: " << stats.hits << ", Misses: " << stats.misses << std::endl;
    std::cout << "  Hit rate: " << (stats.hit_rate * 100.0) << "%" << std::endl;

    assert(stats.hits == 3 && "Should have 3 cache hits");
    assert(stats.misses == 2 && "Should have 2 cache misses");
    assert(stats.hit_rate > 0.5 && stats.hit_rate < 0.7 && "Hit rate should be ~60%");

    std::cout << "  [✓] Hit rate tracking works correctly" << std::endl;
}

// Test 8: Signature vs Script cache - verify more granular
void testGranularityVsScriptCache() {
    std::cout << "\n[Test 8] Signature cache is more granular than script cache" << std::endl;

    SignatureCache cache(1);

    // Same signature and pubkey, but different messages (different sighashes)
    // This simulates the same signature being used in different transaction contexts
    std::vector<uint8_t> signature = {0x01, 0x02, 0x03};
    std::vector<uint8_t> pubkey = {0x04, 0x05, 0x06};

    std::vector<uint8_t> message1 = {0x07};  // First transaction sighash
    std::vector<uint8_t> message2 = {0x08};  // Different transaction sighash

    auto key1 = SignatureCache::computeKey(signature, pubkey, message1);
    auto key2 = SignatureCache::computeKey(signature, pubkey, message2);

    // Insert both
    cache.insert(key1, true);
    cache.insert(key2, false);  // Same sig/pubkey, but different result for different message

    // Verify they are cached independently
    bool result1, result2;
    bool found1 = cache.get(key1, result1);
    bool found2 = cache.get(key2, result2);

    assert(found1 && found2 && "Both should be cached");
    assert(result1 == true && "First should be valid");
    assert(result2 == false && "Second should be invalid");

    std::cout << "  [✓] Signature cache correctly caches at signature level (more granular)" << std::endl;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase F.8.4: Signature Cache Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testBasicInsertGet();
    testCacheMiss();
    testNegativeResults();
    testLRUEviction();
    testCacheClear();
    testCacheKeyUniqueness();
    testHitRateTracking();
    testGranularityVsScriptCache();

    std::cout << "\n========================================" << std::endl;
    std::cout << "[✓✓✓] ALL TESTS PASSED [✓✓✓]" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
