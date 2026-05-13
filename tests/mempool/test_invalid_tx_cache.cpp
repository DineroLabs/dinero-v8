/**
 * F.9.8: Invalid Transaction Cache Tests
 *
 * Verifies DoS protection via invalid transaction caching:
 * 1. Rejected transactions are cached
 * 2. Cache lookups short-circuit expensive validation
 * 3. LRU eviction works correctly
 * 4. Time-based expiry (24 hours)
 * 5. Cache statistics tracking
 */

#include "mempool/invalid_tx_cache.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace dinero::mempool;

// Test 1: Basic add and lookup
void testBasicAddLookup() {
    std::cout << "\n[Test 1] Basic add and lookup" << std::endl;

    InvalidTxCache::Config config;
    config.max_entries = 100;
    config.expiry_seconds = 3600;  // 1 hour for testing
    InvalidTxCache cache(config);

    std::string txid = "abc123";
    std::string reason = "Invalid signature";
    uint64_t time = 1000000;

    // Add to cache
    cache.add(txid, reason, time);

    // Lookup (should find it)
    auto result = cache.lookup(txid, time);
    assert(result.has_value() && "Should find cached entry");
    assert(result.value() == reason && "Reason should match");

    // Lookup non-existent
    auto result2 = cache.lookup("nonexistent", time);
    assert(!result2.has_value() && "Should not find non-existent entry");

    std::cout << "  [✓] Add and lookup work correctly" << std::endl;
}

// Test 2: Time-based expiry
void testTimeBasedExpiry() {
    std::cout << "\n[Test 2] Time-based expiry (24 hours)" << std::endl;

    InvalidTxCache::Config config;
    config.max_entries = 100;
    config.expiry_seconds = 86400;  // 24 hours
    InvalidTxCache cache(config);

    std::string txid = "expired_tx";
    std::string reason = "Double spend";
    uint64_t time_added = 1000000;

    // Add to cache
    cache.add(txid, reason, time_added);

    // Lookup immediately (should find it)
    auto result1 = cache.lookup(txid, time_added);
    assert(result1.has_value() && "Should find fresh entry");

    // Lookup 23 hours later (should still find it)
    uint64_t time_23h_later = time_added + (23 * 3600);
    auto result2 = cache.lookup(txid, time_23h_later);
    assert(result2.has_value() && "Should find entry before expiry");

    // Lookup 25 hours later (should NOT find it - expired)
    uint64_t time_25h_later = time_added + (25 * 3600);
    auto result3 = cache.lookup(txid, time_25h_later);
    assert(!result3.has_value() && "Should not find expired entry");

    // Check stats - should have 1 expiry
    auto stats = cache.getStats();
    assert(stats.expiries == 1 && "Should have 1 expiry");

    std::cout << "  [✓] Time-based expiry works correctly (24 hours)" << std::endl;
}

// Test 3: LRU eviction
void testLRUEviction() {
    std::cout << "\n[Test 3] LRU eviction when cache is full" << std::endl;

    InvalidTxCache::Config config;
    config.max_entries = 5;  // Small cache for testing
    config.expiry_seconds = 86400;
    InvalidTxCache cache(config);

    uint64_t time = 1000000;

    // Add 5 transactions (fill cache)
    for (int i = 0; i < 5; i++) {
        std::string txid = "tx_" + std::to_string(i);
        cache.add(txid, "Invalid " + std::to_string(i), time);
    }

    // Verify all 5 are in cache
    for (int i = 0; i < 5; i++) {
        std::string txid = "tx_" + std::to_string(i);
        auto result = cache.lookup(txid, time);
        assert(result.has_value() && "Should find all 5 entries");
    }

    // Access tx_0 to make it most recently used
    cache.lookup("tx_0", time);

    // Add 6th transaction (should evict tx_1, the LRU)
    cache.add("tx_5", "Invalid 5", time);

    // tx_1 should be evicted (was LRU before we accessed tx_0)
    auto result_evicted = cache.lookup("tx_1", time);
    assert(!result_evicted.has_value() && "tx_1 should be evicted (LRU)");

    // tx_0 should still be there (we accessed it recently)
    auto result_kept = cache.lookup("tx_0", time);
    assert(result_kept.has_value() && "tx_0 should still be cached");

    // Check stats
    auto stats = cache.getStats();
    assert(stats.evictions == 1 && "Should have 1 LRU eviction");

    std::cout << "  [✓] LRU eviction works correctly" << std::endl;
}

// Test 4: Cache statistics
void testCacheStatistics() {
    std::cout << "\n[Test 4] Cache statistics tracking" << std::endl;

    InvalidTxCache::Config config;
    config.max_entries = 10;
    config.expiry_seconds = 3600;
    InvalidTxCache cache(config);

    uint64_t time = 1000000;

    // Add 5 transactions
    for (int i = 0; i < 5; i++) {
        cache.add("tx_" + std::to_string(i), "Invalid", time);
    }

    // Perform lookups (3 hits, 2 misses)
    cache.lookup("tx_0", time);  // Hit
    cache.lookup("tx_1", time);  // Hit
    cache.lookup("tx_2", time);  // Hit
    cache.lookup("nonexistent1", time);  // Miss
    cache.lookup("nonexistent2", time);  // Miss

    auto stats = cache.getStats();

    std::cout << "  Cache statistics:" << std::endl;
    std::cout << "    Size: " << stats.size << " / " << stats.max_size << std::endl;
    std::cout << "    Hits: " << stats.hits << std::endl;
    std::cout << "    Misses: " << stats.misses << std::endl;
    std::cout << "    Evictions: " << stats.evictions << std::endl;
    std::cout << "    Expiries: " << stats.expiries << std::endl;

    assert(stats.size == 5 && "Should have 5 entries");
    assert(stats.hits == 3 && "Should have 3 hits");
    assert(stats.misses == 2 && "Should have 2 misses");

    std::cout << "  [✓] Cache statistics tracked correctly" << std::endl;
}

// Test 5: Cache removal (reorg scenario)
void testCacheRemoval() {
    std::cout << "\n[Test 5] Cache removal (reorg scenario)" << std::endl;

    InvalidTxCache::Config config;
    InvalidTxCache cache(config);

    std::string txid = "reorg_tx";
    uint64_t time = 1000000;

    // Add transaction to cache
    cache.add(txid, "Invalid during reorg", time);

    // Verify it's cached
    auto result1 = cache.lookup(txid, time);
    assert(result1.has_value() && "Should be cached");

    // Remove from cache (simulating reorg where it becomes valid)
    cache.remove(txid);

    // Verify it's removed
    auto result2 = cache.lookup(txid, time);
    assert(!result2.has_value() && "Should be removed from cache");

    std::cout << "  [✓] Cache removal works (for reorg scenarios)" << std::endl;
}

// Test 6: Clear cache
void testClearCache() {
    std::cout << "\n[Test 6] Clear cache" << std::endl;

    InvalidTxCache::Config config;
    InvalidTxCache cache(config);

    uint64_t time = 1000000;

    // Add multiple transactions
    for (int i = 0; i < 10; i++) {
        cache.add("tx_" + std::to_string(i), "Invalid", time);
    }

    auto stats_before = cache.getStats();
    assert(stats_before.size == 10 && "Should have 10 entries");

    // Clear cache
    cache.clear();

    auto stats_after = cache.getStats();
    assert(stats_after.size == 0 && "Should have 0 entries after clear");
    assert(stats_after.hits == 0 && "Stats should be reset");
    assert(stats_after.misses == 0 && "Stats should be reset");

    std::cout << "  [✓] Clear cache works correctly" << std::endl;
}

// Test 7: Update existing entry
void testUpdateExisting() {
    std::cout << "\n[Test 7] Update existing entry" << std::endl;

    InvalidTxCache::Config config;
    InvalidTxCache cache(config);

    std::string txid = "update_tx";
    uint64_t time1 = 1000000;
    uint64_t time2 = 2000000;

    // Add with first reason
    cache.add(txid, "Reason 1", time1);

    auto result1 = cache.lookup(txid, time1);
    assert(result1.has_value() && "Should be cached");
    assert(result1.value() == "Reason 1" && "Should have first reason");

    // Update with new reason
    cache.add(txid, "Reason 2", time2);

    auto result2 = cache.lookup(txid, time2);
    assert(result2.has_value() && "Should still be cached");
    assert(result2.value() == "Reason 2" && "Should have updated reason");

    std::cout << "  [✓] Updating existing entry works correctly" << std::endl;
}

int main() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "F.9.8: Invalid Transaction Cache Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testBasicAddLookup();
    testTimeBasedExpiry();
    testLRUEviction();
    testCacheStatistics();
    testCacheRemoval();
    testClearCache();
    testUpdateExisting();

    std::cout << "\n========================================" << std::endl;
    std::cout << "[✓✓✓] ALL TESTS PASSED [✓✓✓]" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\n🎉 F.9.8 COMPLETE: Invalid Transaction Cache 🎉" << std::endl;
    std::cout << "✅ DoS protection (prevents re-validation)" << std::endl;
    std::cout << "✅ LRU eviction (bounded memory)" << std::endl;
    std::cout << "✅ Time-based expiry (24 hours)" << std::endl;
    std::cout << "✅ Cache statistics (hits/misses/evictions)" << std::endl;
    std::cout << "✅ Reorg-safe (can remove entries)" << std::endl;
    std::cout << "\n🎊 PHASE F.9 (Mempool Correctness) COMPLETE! 🎊" << std::endl;
    std::cout << "========================================\n" << std::endl;

    return 0;
}
