#pragma once

#include "primitives/hash_domains.h"  // Phase M: TxId-based transaction identity
#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <cstdint>
#include <optional>

namespace dinero {
namespace mempool {

/**
 * F.9.8: Invalid Transaction Cache
 *
 * Caches rejected transaction IDs to prevent reprocessing of known-invalid
 * transactions. This is a critical DoS protection mechanism - without it,
 * an attacker can spam the mempool with the same invalid transaction and
 * force expensive validation work.
 *
 * Bitcoin Core has a similar mechanism in validation.cpp (recentRejects).
 *
 * Design:
 * - Bounded cache (10k entries default)
 * - LRU eviction policy
 * - Thread-safe (mutex protected)
 * - Stores rejection reason for debugging
 * - Time-based expiry (24 hours) to prevent permanent blacklisting
 */
class InvalidTxCache {
public:
    struct Config {
        size_t max_entries;        // Maximum cached entries (default: 10000)
        uint64_t expiry_seconds;   // Time-to-live in seconds (default: 86400 = 24 hours)

        Config()
            : max_entries(10000)
            , expiry_seconds(86400)  // 24 hours
        {}
    };

    struct CacheEntry {
        std::string reason;      // Rejection reason (for debugging/logging)
        uint64_t timestamp;      // Time when added (for expiry)
    };

    explicit InvalidTxCache(const Config& config = Config());

    /**
     * Add a rejected transaction to the cache
     *
     * @param txid Transaction ID (Phase M.4.3-C: TxId)
     * @param reason Rejection reason (e.g., "Invalid signature", "Double spend")
     * @param timestamp Current time (seconds since epoch)
     */
    void add(const TxId& txid, const std::string& reason, uint64_t timestamp);

    /**
     * Check if a transaction is in the invalid cache
     *
     * @param txid Transaction ID (Phase M.4.3-C: TxId)
     * @param current_time Current time (for expiry check)
     * @return Rejection reason if found and not expired, nullopt otherwise
     */
    std::optional<std::string> lookup(const TxId& txid, uint64_t current_time);

    /**
     * Remove a transaction from the cache
     *
     * Used when a previously-invalid transaction becomes valid
     * (e.g., after a reorg or policy change).
     * @param txid Transaction ID (Phase M.4.3-C: TxId)
     */
    void remove(const TxId& txid);

    /**
     * Clear all entries
     */
    void clear();

    /**
     * Get cache statistics
     */
    struct Stats {
        size_t size;           // Current number of entries
        size_t max_size;       // Maximum allowed entries
        size_t hits;           // Cache hits (lookups that found entry)
        size_t misses;         // Cache misses (lookups that didn't find entry)
        size_t evictions;      // Number of LRU evictions
        size_t expiries;       // Number of time-based expiries
    };

    Stats getStats() const;

private:
    Config config_;
    mutable std::mutex mutex_;

    // Cache storage: txid → entry (Phase M.4.3-C: TxId keys)
    std::unordered_map<TxId, CacheEntry> cache_;

    // LRU tracking: most recently used at back, least recently used at front (Phase M.4.3-C: TxId values)
    std::list<TxId> lru_list_;

    // LRU position tracker: txid → iterator in lru_list_ (Phase M.4.3-C: TxId keys)
    std::unordered_map<TxId, std::list<TxId>::iterator> lru_map_;

    // Statistics
    mutable size_t hits_;
    mutable size_t misses_;
    size_t evictions_;
    size_t expiries_;

    // Internal helpers (Phase M.4.3-C: TxId parameters)
    void touchLRU(const TxId& txid);
    void evictLRU();
    bool isExpired(const CacheEntry& entry, uint64_t current_time) const;
};

} // namespace mempool
} // namespace dinero
