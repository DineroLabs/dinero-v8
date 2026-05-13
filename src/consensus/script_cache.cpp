#include "consensus/script_cache.h"
#include "crypto/sha256.h"
#include <cstring>
#include <iostream>
#include <memory>

namespace dinero {
namespace consensus {

// Global script cache instance
std::unique_ptr<ScriptCache> g_script_cache;

// ============================================================================
// ScriptCache Implementation
// ============================================================================

ScriptCache::ScriptCache(size_t max_size_mb) {
    // Convert MB to number of entries
    // Each entry: 32 bytes (key) + 1 byte (result) + 8 bytes (iterator) ≈ 41 bytes
    // But we'll be conservative and assume 64 bytes per entry for overhead
    max_size_ = (max_size_mb * 1024 * 1024) / 64;

    std::cout << "[ScriptCache] Initialized with max size: " << max_size_
              << " entries (" << max_size_mb << " MB)" << std::endl;
}

bool ScriptCache::get(const CacheKey& key, bool& result) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = cache_.find(key);
    if (it == cache_.end()) {
        misses_++;
        return false;  // Cache miss
    }

    // Cache hit - move to front of LRU list (most recently used)
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);

    result = it->second.result;
    hits_++;
    return true;  // Cache hit
}

void ScriptCache::insert(const CacheKey& key, bool result) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if key already exists (update case)
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        // Key exists - update result and move to front
        it->second.result = result;
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second.lru_iter);
        return;
    }

    // New entry - check if cache is full
    if (cache_.size() >= max_size_) {
        evictLRU();
    }

    // Insert new entry at front of LRU list
    lru_list_.push_front(key);
    cache_[key] = {result, lru_list_.begin()};
}

void ScriptCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    cache_.clear();
    lru_list_.clear();
    // Keep hit/miss statistics (useful for debugging)

    std::cout << "[ScriptCache] Cleared (hit rate before clear: "
              << (hits_ + misses_ > 0 ? (100.0 * hits_ / (hits_ + misses_)) : 0.0)
              << "%)" << std::endl;
}

ScriptCache::Stats ScriptCache::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.size = cache_.size();
    stats.max_size = max_size_;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.hit_rate = (hits_ + misses_ > 0) ? (double)hits_ / (hits_ + misses_) : 0.0;
    stats.memory_bytes = cache_.size() * 64;  // Approximate (key + value + overhead)

    return stats;
}

void ScriptCache::evictLRU() {
    // Remove least recently used entry (back of list)
    if (lru_list_.empty()) {
        return;
    }

    CacheKey key_to_evict = lru_list_.back();
    lru_list_.pop_back();
    cache_.erase(key_to_evict);
}

// ============================================================================
// Cache Key Computation
// ============================================================================

ScriptCache::CacheKey ScriptCache::computeKey(
    const uint256& txid,
    uint32_t input_index,
    uint32_t script_flags,
    const std::vector<std::vector<uint8_t>>& witness_data
) {
    // Compute SHA256(txid || input_index || script_flags || witness_hash)
    //
    // This uniquely identifies a script execution:
    //   - txid: Transaction being validated (Phase M.0: uint256 identity)
    //   - input_index: Which input in the transaction
    //   - script_flags: Consensus rules (e.g., P2SH, WITNESS)
    //   - witness_data: Signature + pubkey (or taproot witness)
    //
    // If any input changes, the cache key changes, forcing re-validation.

    crypto::CSHA256 hasher;

    // 1. Hash txid (Phase M.0: direct 32-byte identity, no hex parsing)
    hasher.Write(txid.data, 32);

    // 2. Hash input_index (4 bytes, little-endian)
    uint8_t index_bytes[4];
    index_bytes[0] = input_index & 0xFF;
    index_bytes[1] = (input_index >> 8) & 0xFF;
    index_bytes[2] = (input_index >> 16) & 0xFF;
    index_bytes[3] = (input_index >> 24) & 0xFF;
    hasher.Write(index_bytes, 4);

    // 3. Hash script_flags (4 bytes, little-endian)
    uint8_t flags_bytes[4];
    flags_bytes[0] = script_flags & 0xFF;
    flags_bytes[1] = (script_flags >> 8) & 0xFF;
    flags_bytes[2] = (script_flags >> 16) & 0xFF;
    flags_bytes[3] = (script_flags >> 24) & 0xFF;
    hasher.Write(flags_bytes, 4);

    // 4. Hash witness data (variable length)
    //    For P2WPKH: [signature, pubkey]
    //    For Taproot: [witness stack items]
    for (const auto& witness_item : witness_data) {
        // Hash length (4 bytes)
        uint32_t len = witness_item.size();
        uint8_t len_bytes[4];
        len_bytes[0] = len & 0xFF;
        len_bytes[1] = (len >> 8) & 0xFF;
        len_bytes[2] = (len >> 16) & 0xFF;
        len_bytes[3] = (len >> 24) & 0xFF;
        hasher.Write(len_bytes, 4);

        // Hash data
        if (!witness_item.empty()) {
            hasher.Write(witness_item.data(), witness_item.size());
        }
    }

    // Finalize hash
    CacheKey key;
    hasher.Finalize(key.data);

    return key;
}

// ============================================================================
// Global Initialization
// ============================================================================

bool InitializeScriptCache(size_t max_size_mb) {
    if (g_script_cache) {
        std::cerr << "[ScriptCache] Already initialized" << std::endl;
        return false;
    }

    g_script_cache = std::make_unique<ScriptCache>(max_size_mb);
    std::cout << "[ScriptCache] Initialized successfully" << std::endl;
    return true;
}

void ShutdownScriptCache() {
    if (g_script_cache) {
        auto stats = g_script_cache->getStats();
        std::cout << "[ScriptCache] Shutdown - Final stats:" << std::endl;
        std::cout << "  Entries: " << stats.size << " / " << stats.max_size << std::endl;
        std::cout << "  Hits: " << stats.hits << ", Misses: " << stats.misses << std::endl;
        std::cout << "  Hit rate: " << (stats.hit_rate * 100.0) << "%" << std::endl;
        std::cout << "  Memory: " << (stats.memory_bytes / 1024 / 1024) << " MB" << std::endl;

        g_script_cache.reset();
    }
}

} // namespace consensus
} // namespace dinero
