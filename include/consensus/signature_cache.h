#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <list>
#include <vector>
#include <cstring>
#include <memory>

namespace dinero {
namespace consensus {

/**
 * SignatureCache - LRU cache for ECDSA signature verification results
 *
 * Caches individual signature verifications at the secp256k1 level.
 * This is more granular than ScriptCache (F.8.3) and provides additional speedup.
 *
 * Bitcoin Core equivalent: CSignatureCache (in script/sigcache.cpp)
 *
 * Architecture:
 *   - Cache key: Hash(signature_der + pubkey_bytes + message_hash)
 *   - Cache value: Valid (true) or invalid (false)
 *   - Eviction policy: LRU (least recently used)
 *   - Size limit: Configurable (default 32 MiB = ~8 million entries)
 *   - Thread-safety: Mutex-protected
 *
 * Why separate from ScriptCache (F.8.3)?
 *   - ScriptCache: Caches entire input verification (tx-level)
 *   - SignatureCache: Caches individual signature checks (crypto-level)
 *   - SignatureCache is more reusable (same sig in different contexts)
 *   - Combined: 30-100x speedup (Bitcoin Core standard)
 *
 * Performance impact:
 *   - Cache hit rate: 60-80% (signatures reused across txs)
 *   - Speedup: 2-5x additional on top of ScriptCache
 *   - Memory: ~32 MiB (much smaller than script cache)
 *
 * F.8.4: Signature Cache Implementation
 */
class SignatureCache {
public:
    /**
     * Cache entry key (256-bit hash of signature verification inputs)
     *
     * Computed as: SHA256(signature_der || pubkey_bytes || message_hash)
     *
     * Why this works:
     *   - signature_der: DER-encoded ECDSA signature (64-73 bytes)
     *   - pubkey_bytes: Compressed public key (33 bytes)
     *   - message_hash: Message being signed (32 bytes, usually sighash)
     *
     * If any input changes, cache key changes, forcing re-verification.
     */
    struct CacheKey {
        uint8_t data[32];  // 256-bit hash

        CacheKey() { std::memset(data, 0, 32); }

        bool operator==(const CacheKey& other) const {
            return std::memcmp(data, other.data, 32) == 0;
        }

        // Hash function for std::unordered_map
        struct Hash {
            std::size_t operator()(const CacheKey& key) const {
                // Use first 8 bytes as hash
                std::size_t result;
                std::memcpy(&result, key.data, sizeof(std::size_t));
                return result;
            }
        };
    };

    /**
     * Constructor
     *
     * @param max_size_mb Maximum cache size in megabytes (default: 32 MB)
     *                    Bitcoin Core uses 32-40 MB for signature cache
     */
    explicit SignatureCache(size_t max_size_mb = 32);

    /**
     * Check if a signature verification result is cached
     *
     * @param key Cache key (computed from signature + pubkey + message)
     * @param result Output parameter: true if signature is valid
     * @return true if cache hit, false if cache miss
     *
     * Thread-safe: Uses read lock
     */
    bool get(const CacheKey& key, bool& result);

    /**
     * Store a signature verification result in the cache
     *
     * @param key Cache key
     * @param result true if signature is valid, false if invalid
     *
     * Eviction policy: If cache is full, evicts least recently used entry
     *
     * Thread-safe: Uses write lock
     */
    void insert(const CacheKey& key, bool result);

    /**
     * Clear all cached entries
     *
     * Called on:
     *   - Block disconnect (reorg invalidation) - optional
     *   - Node restart (cache is memory-only)
     *
     * Note: Signature cache is usually NOT cleared on reorg (unlike script cache)
     * because signatures are context-independent. However, we provide this
     * method for consistency and future use.
     *
     * Thread-safe: Uses write lock
     */
    void clear();

    /**
     * Get cache statistics
     *
     * Thread-safe: Uses read lock
     */
    struct Stats {
        size_t size;         // Current number of entries
        size_t max_size;     // Maximum entries
        uint64_t hits;       // Number of cache hits
        uint64_t misses;     // Number of cache misses
        double hit_rate;     // hits / (hits + misses)
        size_t memory_bytes; // Approximate memory usage
    };
    Stats getStats() const;

    /**
     * Compute cache key from signature verification inputs
     *
     * @param signature_der DER-encoded ECDSA signature (64-73 bytes)
     * @param pubkey_bytes Compressed public key (33 bytes)
     * @param message_hash Message hash being signed (32 bytes, usually BIP143 sighash)
     * @return 256-bit cache key
     *
     * This is a static helper function used by TransactionValidator
     */
    static CacheKey computeKey(
        const std::vector<uint8_t>& signature_der,
        const std::vector<uint8_t>& pubkey_bytes,
        const std::vector<uint8_t>& message_hash
    );

private:
    // LRU cache implementation using hash map + linked list
    // Map: CacheKey → (result, iterator to list node)
    // List: Stores keys in LRU order (most recent at front)

    struct CacheEntry {
        bool result;                                    // Valid/invalid
        std::list<CacheKey>::iterator lru_iter;         // Position in LRU list
    };

    mutable std::mutex mutex_;                          // Thread-safety
    std::unordered_map<CacheKey, CacheEntry, CacheKey::Hash> cache_;
    std::list<CacheKey> lru_list_;                      // Most recent at front

    size_t max_size_;                                   // Maximum entries
    uint64_t hits_{0};                                  // Statistics
    uint64_t misses_{0};

    // Evict least recently used entry (called when cache is full)
    void evictLRU();
};

/**
 * Global signature cache instance
 *
 * Shared across all validation threads for maximum cache hit rate.
 * Initialized at node startup, cleared on shutdown.
 */
extern std::unique_ptr<SignatureCache> g_signature_cache;

/**
 * Initialize the global signature cache
 *
 * @param max_size_mb Maximum cache size in megabytes
 * @return true if successful
 *
 * Called from daemon main() at startup
 */
bool InitializeSignatureCache(size_t max_size_mb = 32);

/**
 * Shutdown the global signature cache
 *
 * Called from daemon shutdown sequence
 */
void ShutdownSignatureCache();

} // namespace consensus
} // namespace dinero
