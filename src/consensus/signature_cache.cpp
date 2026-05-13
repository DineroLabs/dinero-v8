#include "consensus/signature_cache.h"
#include "crypto/sha256.h"
#include <cstring>
#include <iostream>
#include <memory>

namespace dinero {
namespace consensus {

// Global signature cache instance
std::unique_ptr<SignatureCache> g_signature_cache;

// ============================================================================
// SignatureCache Implementation
// ============================================================================

SignatureCache::SignatureCache(size_t max_size_mb) {
    // Convert MB to number of entries
    // Each entry: 32 bytes (key) + 1 byte (result) + 8 bytes (iterator) ≈ 41 bytes
    // Conservative estimate: 64 bytes per entry (includes overhead)
    max_size_ = (max_size_mb * 1024 * 1024) / 64;

    std::cout << "[SignatureCache] Initialized with max size: " << max_size_
              << " entries (" << max_size_mb << " MB)" << std::endl;
}

bool SignatureCache::get(const CacheKey& key, bool& result) {
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

void SignatureCache::insert(const CacheKey& key, bool result) {
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

void SignatureCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    cache_.clear();
    lru_list_.clear();
    // Keep hit/miss statistics (useful for debugging)

    std::cout << "[SignatureCache] Cleared (hit rate before clear: "
              << (hits_ + misses_ > 0 ? (100.0 * hits_ / (hits_ + misses_)) : 0.0)
              << "%)" << std::endl;
}

SignatureCache::Stats SignatureCache::getStats() const {
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

void SignatureCache::evictLRU() {
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

SignatureCache::CacheKey SignatureCache::computeKey(
    const std::vector<uint8_t>& signature_der,
    const std::vector<uint8_t>& pubkey_bytes,
    const std::vector<uint8_t>& message_hash
) {
    // Compute SHA256(signature_der || pubkey_bytes || message_hash)
    //
    // This uniquely identifies a signature verification:
    //   - signature_der: DER-encoded ECDSA signature (64-73 bytes)
    //   - pubkey_bytes: Compressed public key (33 bytes)
    //   - message_hash: Message being signed (32 bytes, BIP143 sighash)
    //
    // If any input changes, the cache key changes, forcing re-verification.
    // This is more granular than script cache and allows reuse across transactions.

    crypto::CSHA256 hasher;

    // 1. Hash signature DER bytes
    if (!signature_der.empty()) {
        hasher.Write(signature_der.data(), signature_der.size());
    }

    // 2. Hash public key bytes
    if (!pubkey_bytes.empty()) {
        hasher.Write(pubkey_bytes.data(), pubkey_bytes.size());
    }

    // 3. Hash message hash (sighash)
    if (!message_hash.empty()) {
        hasher.Write(message_hash.data(), message_hash.size());
    }

    // Finalize hash
    CacheKey key;
    hasher.Finalize(key.data);

    return key;
}

// ============================================================================
// Global Initialization
// ============================================================================

bool InitializeSignatureCache(size_t max_size_mb) {
    if (g_signature_cache) {
        std::cerr << "[SignatureCache] Already initialized" << std::endl;
        return false;
    }

    g_signature_cache = std::make_unique<SignatureCache>(max_size_mb);
    std::cout << "[SignatureCache] Initialized successfully" << std::endl;
    return true;
}

void ShutdownSignatureCache() {
    if (g_signature_cache) {
        auto stats = g_signature_cache->getStats();
        std::cout << "[SignatureCache] Shutdown - Final stats:" << std::endl;
        std::cout << "  Entries: " << stats.size << " / " << stats.max_size << std::endl;
        std::cout << "  Hits: " << stats.hits << ", Misses: " << stats.misses << std::endl;
        std::cout << "  Hit rate: " << (stats.hit_rate * 100.0) << "%" << std::endl;
        std::cout << "  Memory: " << (stats.memory_bytes / 1024 / 1024) << " MB" << std::endl;

        g_signature_cache.reset();
    }
}

} // namespace consensus
} // namespace dinero
