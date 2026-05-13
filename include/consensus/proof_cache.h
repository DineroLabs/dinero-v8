#pragma once

#include <optional>
#include <unordered_map>
#include <list>
#include <mutex>
#include <cstdint>
#include "primitives/block.h"
#include "primitives/uint256.h"

namespace dinero {
namespace consensus {

/**
 * Cached proof entry
 *
 * Phase 9.1: Local proof cache for stateless validation performance
 */
struct CachedProof {
    uint256 block_hash;           // Primary key
    BlockUtreexoData proof;       // Cached proof data
    uint256 root_hash;            // Utreexo root for quick validation
    uint64_t timestamp;           // Unix timestamp (for TTL eviction)
    uint32_t access_count;        // Access counter (for LRU tracking)
    size_t size_bytes;            // Memory footprint

    CachedProof() = default;

    CachedProof(const uint256& hash,
                const BlockUtreexoData& p,
                const uint256& root)
        : block_hash(hash),
          proof(p),
          root_hash(root),
          timestamp(GetCurrentTimestamp()),
          access_count(0),
          size_bytes(EstimateSize(p)) {}

    static uint64_t GetCurrentTimestamp();
    static size_t EstimateSize(const BlockUtreexoData& proof);
};

/**
 * Eviction policy for proof cache
 */
enum class EvictionPolicy {
    LRU,      // Evict least recently used
    TTL,      // Evict expired entries
    OLDEST,   // Evict by timestamp
};

/**
 * LRU + TTL proof cache for stateless validation
 *
 * Phase 9.1: Performance optimization layer (non-consensus)
 *
 * Design goals:
 * - Reduce redundant proof requests
 * - Improve stateless node sync speed
 * - Zero consensus impact (cache miss = normal operation)
 *
 * Security rule:
 * - Cached proofs are ALWAYS re-verified before use
 * - Cache is optimization, not trust shortcut
 *
 * Thread safety:
 * - All operations are mutex-protected for concurrent access
 */
class ProofCache {
public:
    /**
     * Cache statistics
     */
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        double hit_rate = 0.0;
    };

    // Configuration constants
    static constexpr size_t MAX_CACHE_SIZE = 1024 * 1024 * 100;  // 100 MB
    static constexpr uint64_t DEFAULT_TTL_SECS = 86400;          // 24 hours

    ProofCache();
    ~ProofCache() = default;

    /**
     * Get proof from cache
     *
     * @param block_hash Block hash to look up
     * @return Cached proof if found and not expired, std::nullopt otherwise
     *
     * Note: Caller MUST re-verify the returned proof before use (security rule)
     */
    std::optional<BlockUtreexoData> Get(const uint256& block_hash);

    /**
     * Get proof with root hash (for quick validation)
     *
     * @param block_hash Block hash to look up
     * @param out_root Output parameter for root hash
     * @return Cached proof if found and not expired, std::nullopt otherwise
     */
    std::optional<BlockUtreexoData> GetWithRoot(const uint256& block_hash, uint256& out_root);

    /**
     * Put proof into cache
     *
     * @param block_hash Block hash (primary key)
     * @param proof Proof data to cache
     * @param root_hash Utreexo root hash for validation
     *
     * Note: May trigger eviction if cache is full
     */
    void Put(const uint256& block_hash, const BlockUtreexoData& proof, const uint256& root_hash);

    /**
     * Evict entries based on policy
     *
     * @param policy Eviction strategy to use
     * @return Number of entries evicted
     */
    size_t Evict(EvictionPolicy policy);

    /**
     * Evict specific entry
     *
     * @param block_hash Entry to remove
     * @return true if entry was found and removed
     */
    bool Evict(const uint256& block_hash);

    /**
     * Clear all cache entries
     */
    void Clear();

    /**
     * Get cache statistics
     */
    size_t Size() const;              // Number of entries
    size_t TotalBytes() const;        // Total memory usage
    double HitRate() const;           // Cache hit rate (hits / requests)

    /**
     * Set custom TTL (for testing or mobile optimization)
     */
    void SetTTL(uint64_t ttl_secs) { ttl_secs_ = ttl_secs; }

private:
    // Cache storage
    std::unordered_map<uint256, CachedProof> cache_;

    // LRU tracking (most recently used at front)
    std::list<uint256> lru_order_;
    std::unordered_map<uint256, std::list<uint256>::iterator> lru_lookup_;

    // Memory accounting
    size_t total_size_bytes_ = 0;

    // Statistics
    mutable uint64_t hits_ = 0;
    mutable uint64_t misses_ = 0;

    // Configuration
    uint64_t ttl_secs_ = DEFAULT_TTL_SECS;

    // Thread safety
    mutable std::mutex mutex_;

    // Internal eviction helpers
    void EvictLRU();           // Remove least recently used
    void EvictTTL();           // Remove expired entries
    void EvictOldest();        // Remove by timestamp
    void CheckAndEvict();      // Check size and evict if needed
    void UpdateLRU(const uint256& block_hash);
    void RemoveFromLRU(const uint256& block_hash);
};

} // namespace consensus
} // namespace dinero
