#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include "consensus/block_index.h"

namespace dinero {

/**
 * Orphan Block Manager - Bounded orphan pool with eviction policies
 *
 * Bitcoin Core ~0.16 behavior:
 * - Orphan blocks are blocks whose parent is unknown
 * - Pool is size-bounded to prevent memory exhaustion
 * - Oldest orphans are evicted when limit reached
 * - Per-peer limits prevent single peer from flooding pool
 * - Time-based eviction removes stale orphans
 *
 * DoS Protection:
 * - MAX_ORPHAN_BLOCKS prevents unbounded growth
 * - Per-peer limits (20% of pool) prevent peer flooding
 * - Time-based expiry (24 hours) prevents stale data
 */

// Orphan pool configuration
constexpr uint32_t MAX_ORPHAN_BLOCKS = 1000;  // Max orphans in pool
constexpr uint32_t MAX_ORPHAN_PER_PEER = 200; // Max orphans from single peer (20%)
constexpr uint64_t ORPHAN_MAX_AGE_SECS = 86400; // 24 hours

/**
 * Metadata for tracking orphan blocks
 */
struct OrphanBlockMetadata {
    CBlockIndex* pindex;              // The orphan block
    uint64_t peer_id;                 // Peer who sent this block
    uint64_t arrival_time;            // When block arrived (Unix timestamp)
    uint256 parent_hash;              // Hash of missing parent

    OrphanBlockMetadata() : pindex(nullptr), peer_id(0), arrival_time(0) {}

    OrphanBlockMetadata(CBlockIndex* idx, uint64_t peer, const uint256& parent)
        : pindex(idx), peer_id(peer), parent_hash(parent) {
        arrival_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool IsExpired() const {
        uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return (now - arrival_time) > ORPHAN_MAX_AGE_SECS;
    }
};

/**
 * Orphan pool statistics for monitoring
 */
struct OrphanPoolStats {
    uint32_t total_orphans{0};        // Current orphan count
    uint32_t total_evictions{0};      // Lifetime eviction count
    uint32_t expired_evictions{0};    // Evictions due to age
    uint32_t size_limit_evictions{0}; // Evictions due to pool full
    uint32_t peer_limit_evictions{0}; // Evictions due to per-peer limit
    uint64_t oldest_orphan_age{0};    // Age of oldest orphan (seconds)
};

/**
 * Orphan Block Manager
 *
 * Manages a bounded pool of orphan blocks with eviction policies.
 * Replaces the unbounded g_orphan_pool with production-grade management.
 */
class OrphanBlockManager {
public:
    OrphanBlockManager() = default;

    // Add orphan to pool (returns false if rejected due to limits)
    bool AddOrphan(CBlockIndex* pindex, uint64_t peer_id);

    // Remove orphan from pool
    void RemoveOrphan(const uint256& block_hash);

    // Get orphans waiting for specific parent
    std::vector<CBlockIndex*> GetOrphansForParent(const uint256& parent_hash);

    // Check if block is in orphan pool
    bool IsOrphan(const uint256& block_hash) const;

    // Eviction policies
    void EvictExpiredOrphans();       // Remove orphans older than MAX_AGE
    void EvictOldestOrphan();         // Remove oldest orphan (by arrival time)
    void EvictOrphansFromPeer(uint64_t peer_id); // Remove all orphans from peer

    // Maintenance
    void EnforcePoolLimits();         // Enforce MAX_ORPHAN_BLOCKS limit
    void ClearAll();                  // Remove all orphans (for shutdown/reset)

    // Statistics
    OrphanPoolStats GetStats() const;
    uint32_t GetOrphanCount() const { return orphan_metadata_.size(); }
    uint32_t GetOrphanCountForPeer(uint64_t peer_id) const;

private:
    // Orphan storage (indexed multiple ways for fast lookups)
    std::unordered_map<uint256, OrphanBlockMetadata> orphan_metadata_;  // hash → metadata
    std::unordered_map<uint256, std::vector<uint256>> orphans_by_parent_; // parent_hash → [child_hashes]
    std::unordered_map<uint64_t, std::unordered_set<uint256>> orphans_by_peer_;  // peer_id → {block_hashes}

    // Statistics
    mutable OrphanPoolStats stats_;

    // Helper functions
    void IncrementEvictionCounter(const std::string& reason);
    bool CanAddOrphanFromPeer(uint64_t peer_id) const;
    uint256 FindOldestOrphan() const;
};

// Global orphan manager instance
extern OrphanBlockManager g_orphan_manager;

/**
 * Migration from old g_orphan_pool to OrphanBlockManager
 *
 * These functions provide backward compatibility while transitioning
 * to the new bounded orphan manager.
 */

// Add orphan (new API - includes peer tracking)
bool AddOrphanBlock(CBlockIndex* pindex, uint64_t peer_id);

// Remove orphan
void RemoveOrphanBlock(const uint256& block_hash);

// Get orphans for parent (replaces direct g_orphan_pool access)
std::vector<CBlockIndex*> GetOrphansWaitingForParent(const uint256& parent_hash);

// Maintenance task (call periodically, e.g., every 60 seconds)
void MaintainOrphanPool();

} // namespace dinero
