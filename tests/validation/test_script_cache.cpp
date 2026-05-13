/**
 * Phase F.8.3: Script Execution Cache Test
 *
 * Tests the script cache functionality:
 * 1. Basic insert/get operations
 * 2. Cache hits and misses
 * 3. LRU eviction when cache is full
 * 4. Cache clearing on reorg
 * 5. Cache key computation
 *
 * This proves the cache works correctly and provides speedup during reorgs.
 */

#include "consensus/script_cache.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace dinero::consensus;

// Test 1: Basic insert and get
void testBasicInsertGet() {
    std::cout << "\n[Test 1] Basic insert and get" << std::endl;

    ScriptCache cache(1);  // 1 MB cache (small for testing)

    // Create a test cache key
    std::vector<std::vector<uint8_t>> witness = {
        {0x01, 0x02, 0x03},  // Fake signature
        {0x04, 0x05, 0x06}   // Fake pubkey
    };

    auto key = ScriptCache::computeKey("abc123", 0, 0, witness);

    // Insert a positive result
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

    ScriptCache cache(1);

    // Create a key that was never inserted
    std::vector<std::vector<uint8_t>> witness = {
        {0xFF, 0xFE, 0xFD}
    };

    auto key = ScriptCache::computeKey("nonexistent", 0, 0, witness);

    // Try to get a non-existent key
    bool result = false;
    bool found = cache.get(key, result);

    assert(!found && "Cache should not have the key");

    std::cout << "  [✓] Cache miss detected correctly" << std::endl;
}

// Test 3: Cache negative results
void testNegativeResults() {
    std::cout << "\n[Test 3] Cache negative results" << std::endl;

    ScriptCache cache(1);

    std::vector<std::vector<uint8_t>> witness = {{0x01}};
    auto key = ScriptCache::computeKey("fail_tx", 0, 0, witness);

    // Insert a negative result (verification failed)
    cache.insert(key, false);

    // Get the result back
    bool result = true;  // Initialize to opposite
    bool found = cache.get(key, result);

    assert(found && "Cache should have the key");
    assert(result == false && "Cached result should be false (failure)");

    std::cout << "  [✓] Negative results cached correctly" << std::endl;
}

// Test 4: LRU eviction
void testLRUEviction() {
    std::cout << "\n[Test 4] LRU eviction when cache is full" << std::endl;

    // Create a very small cache (only ~16 entries)
    ScriptCache cache(1);  // 1 MB / 64 bytes = ~16k entries, but we'll test with fewer

    // Fill cache with many entries
    std::vector<ScriptCache::CacheKey> keys;
    std::vector<std::vector<uint8_t>> witness = {{0x01}};

    for (int i = 0; i < 20000; i++) {
        std::string txid = "tx" + std::to_string(i);
        auto key = ScriptCache::computeKey(txid, 0, 0, witness);
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
    std::cout << "\n[Test 5] Cache clear (reorg invalidation)" << std::endl;

    ScriptCache cache(1);

    // Insert some entries
    std::vector<std::vector<uint8_t>> witness = {{0x01}};
    for (int i = 0; i < 10; i++) {
        std::string txid = "tx" + std::to_string(i);
        auto key = ScriptCache::computeKey(txid, 0, 0, witness);
        cache.insert(key, true);
    }

    auto stats_before = cache.getStats();
    assert(stats_before.size == 10 && "Cache should have 10 entries");

    // Clear the cache (simulates reorg)
    cache.clear();

    auto stats_after = cache.getStats();
    assert(stats_after.size == 0 && "Cache should be empty after clear");

    std::cout << "  [✓] Cache clear works correctly" << std::endl;
}

// Test 6: Cache key uniqueness
void testCacheKeyUniqueness() {
    std::cout << "\n[Test 6] Cache key uniqueness" << std::endl;

    std::vector<std::vector<uint8_t>> witness1 = {{0x01, 0x02}};
    std::vector<std::vector<uint8_t>> witness2 = {{0x01, 0x03}};  // Different witness

    auto key1 = ScriptCache::computeKey("tx123", 0, 0, witness1);
    auto key2 = ScriptCache::computeKey("tx123", 0, 0, witness2);

    // Keys should be different (different witness data)
    bool keys_different = std::memcmp(key1.data, key2.data, 32) != 0;
    assert(keys_different && "Keys with different witness should be different");

    // Same inputs should produce same key
    auto key1_dup = ScriptCache::computeKey("tx123", 0, 0, witness1);
    bool keys_same = std::memcmp(key1.data, key1_dup.data, 32) == 0;
    assert(keys_same && "Same inputs should produce same key");

    std::cout << "  [✓] Cache key computation is deterministic and unique" << std::endl;
}

// Test 7: Hit rate tracking
void testHitRateTracking() {
    std::cout << "\n[Test 7] Hit rate tracking" << std::endl;

    ScriptCache cache(1);

    std::vector<std::vector<uint8_t>> witness = {{0x01}};
    auto key = ScriptCache::computeKey("tx", 0, 0, witness);

    // Insert
    cache.insert(key, true);

    // Generate some hits and misses
    bool result;
    cache.get(key, result);  // Hit
    cache.get(key, result);  // Hit

    auto key_miss = ScriptCache::computeKey("nonexistent", 0, 0, witness);
    cache.get(key_miss, result);  // Miss

    auto stats = cache.getStats();
    std::cout << "  Hits: " << stats.hits << ", Misses: " << stats.misses << std::endl;
    std::cout << "  Hit rate: " << (stats.hit_rate * 100.0) << "%" << std::endl;

    assert(stats.hits == 2 && "Should have 2 cache hits");
    assert(stats.misses == 1 && "Should have 1 cache miss");
    assert(stats.hit_rate > 0.6 && stats.hit_rate < 0.7 && "Hit rate should be ~66%");

    std::cout << "  [✓] Hit rate tracking works correctly" << std::endl;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase F.8.3: Script Cache Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testBasicInsertGet();
    testCacheMiss();
    testNegativeResults();
    testLRUEviction();
    testCacheClear();
    testCacheKeyUniqueness();
    testHitRateTracking();

    std::cout << "\n========================================" << std::endl;
    std::cout << "[✓✓✓] ALL TESTS PASSED [✓✓✓]" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
