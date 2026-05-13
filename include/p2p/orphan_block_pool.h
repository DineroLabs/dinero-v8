#pragma once

#include "primitives/block.h"
#include "common/status.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <memory>
#include <mutex>
#include <chrono>

namespace dinero {
namespace p2p {

/**
 * Phase 27.1: OrphanBlockPool - Bitcoin Core-style orphan block management
 *
 * Handles blocks received out-of-order (parent not yet known).
 * Critical for parallel block downloads where blocks arrive asynchronously.
 *
 * Features:
 * - Stores orphan blocks temporarily until parent arrives
 * - LRU eviction when pool is full
 * - Automatic orphan resolution when parent connects
 * - DoS protection (max blocks per peer, max total size)
 * - Thread-safe for concurrent access
 *
 * Lifecycle:
 * 1. Block arrives via P2P (handleBlockMessage)
 * 2. Parent not in chain → add to orphan pool
 * 3. When parent arrives → resolve orphans recursively
 * 4. Orphan becomes processable → submit to ValidationQueue
 * 5. Timeout/eviction → remove stale orphans
 *
 * This is how Bitcoin Core, Litecoin, and all production chains handle
 * out-of-order block delivery during parallel sync.
 */

/**
 * Orphan block metadata
 */
struct OrphanBlock {
    Block block;                                    // Full block data
    std::string block_hash;                         // Block hash (for quick lookup)
    std::string prev_hash;                          // Parent hash (for resolution)
    std::string peer_id;                            // Peer who sent this (for DoS tracking)
    std::chrono::steady_clock::time_point received_time;  // When received
    size_t block_size;                              // Size in bytes (for memory tracking)

    OrphanBlock() : block_size(0) {}

    OrphanBlock(const Block& blk, const std::string& hash, const std::string& prev,
                const std::string& peer, size_t size)
        : block(blk)
        , block_hash(hash)
        , prev_hash(prev)
        , peer_id(peer)
        , received_time(std::chrono::steady_clock::now())
        , block_size(size)
    {}
};

/**
 * OrphanBlockPool - Temporary storage for blocks with unknown parents
 *
 * Thread-safe pool for managing orphan blocks during sync.
 * Implements LRU eviction, per-peer limits, and automatic resolution.
 */
class OrphanBlockPool {
public:
    /**
     * Configuration for orphan pool
     */
    struct Config {
        size_t max_orphan_blocks;            // Max total orphan blocks
        size_t max_orphan_size_mb;             // Max total memory (MB)
        size_t max_orphans_per_peer;          // Max from single peer (DoS protection)
        uint64_t orphan_timeout_seconds;     // Evict after 10 minutes
        bool enable_resolution;              // Auto-resolve when parent arrives

        Config()
            : max_orphan_blocks(100)
            , max_orphan_size_mb(5)
            , max_orphans_per_peer(10)
            , orphan_timeout_seconds(600)
            , enable_resolution(true)
        {}
    };

    explicit OrphanBlockPool(const Config& config = Config());
    ~OrphanBlockPool();

    /**
     * @brief Set BlockPresenceTracker for HaveBlock() integration
     *
     * Phase 31.1: Enables checking if parent blocks exist before adding orphans.
     * Prevents unnecessary orphan storage when parent is already in chain.
     */
    void setBlockPresenceTracker(class BlockPresenceTracker* tracker) {
        block_presence_ = tracker;
    }

    // ========================================================================
    // Core Operations
    // ========================================================================

    /**
     * Add orphan block to pool
     *
     * @param block         Block to add
     * @param block_hash    Block hash
     * @param prev_hash     Parent hash
     * @param peer_id       Peer who sent it
     * @return              True if added, false if rejected (duplicate, full, etc.)
     */
    bool addOrphan(const Block& block, const std::string& block_hash,
                   const std::string& prev_hash, const std::string& peer_id);

    /**
     * Get orphan block by hash
     *
     * @param block_hash    Block hash to lookup
     * @return              Orphan block if found, nullptr otherwise
     */
    std::shared_ptr<OrphanBlock> getOrphan(const std::string& block_hash);

    /**
     * Get all orphans that depend on this parent
     *
     * When a block connects, check if any orphans are now processable.
     *
     * @param parent_hash   Parent hash
     * @return              List of orphans waiting for this parent
     */
    std::vector<std::shared_ptr<OrphanBlock>> getOrphansForParent(const std::string& parent_hash);

    /**
     * Remove orphan from pool
     *
     * @param block_hash    Block hash to remove
     * @return              True if removed, false if not found
     */
    bool removeOrphan(const std::string& block_hash);

    /**
     * Check if block is in orphan pool
     *
     * @param block_hash    Block hash
     * @return              True if orphan exists
     */
    bool hasOrphan(const std::string& block_hash) const;

    // ========================================================================
    // Orphan Resolution (Automatic Chain Building)
    // ========================================================================

    /**
     * Resolve orphans after parent connects
     *
     * Recursively processes orphans that are now valid.
     * Returns list of blocks ready for validation.
     *
     * @param parent_hash   Hash of newly connected block
     * @return              List of orphans that can now be processed
     */
    std::vector<std::shared_ptr<OrphanBlock>> resolveOrphans(const std::string& parent_hash);

    // ========================================================================
    // DoS Protection & Maintenance
    // ========================================================================

    /**
     * Evict stale orphans
     *
     * Remove orphans older than timeout threshold.
     *
     * @return              Number of orphans evicted
     */
    size_t evictStaleOrphans();

    /**
     * Evict orphans from specific peer
     *
     * DoS protection: ban misbehaving peers and remove their orphans.
     *
     * @param peer_id       Peer to evict
     * @return              Number of orphans removed
     */
    size_t evictOrphansFromPeer(const std::string& peer_id);

    /**
     * Evict LRU orphan (oldest received)
     *
     * Called when pool is full and new orphan needs space.
     *
     * @return              True if eviction succeeded
     */
    bool evictLRU();

    /**
     * Clear all orphans
     */
    void clear();

    // ========================================================================
    // Statistics & Monitoring
    // ========================================================================

    struct Stats {
        size_t total_orphans = 0;               // Current orphan count
        size_t total_size_bytes = 0;            // Total memory used
        size_t orphans_added = 0;               // Lifetime count
        size_t orphans_resolved = 0;            // Successfully resolved
        size_t orphans_evicted = 0;             // Evicted (timeout/full)
        size_t orphans_rejected = 0;            // Rejected (duplicate/DoS)
        size_t max_orphans_per_peer = 0;        // Current max from single peer
    };

    Stats getStats() const;

    /**
     * Get orphan count for specific peer (DoS monitoring)
     */
    size_t getOrphanCountForPeer(const std::string& peer_id) const;

    /**
     * Get total memory usage
     */
    size_t getTotalSize() const;

    /**
     * Check if pool is full
     */
    bool isFull() const;

private:
    Config config_;
    mutable std::mutex mutex_;

    // Primary storage: hash → orphan
    std::unordered_map<std::string, std::shared_ptr<OrphanBlock>> orphans_;

    // Secondary index: parent_hash → list of children
    std::unordered_map<std::string, std::unordered_set<std::string>> orphans_by_parent_;

    // Per-peer tracking: peer_id → list of orphan hashes
    std::unordered_map<std::string, std::unordered_set<std::string>> orphans_by_peer_;

    // LRU list: ordered by receive time (oldest first)
    std::list<std::string> lru_list_;

    // Statistics
    mutable Stats stats_;

    // Phase 31.1: BlockPresenceTracker for HaveBlock() checks
    class BlockPresenceTracker* block_presence_{nullptr};

    // Helper: update LRU on access
    void touchOrphan(const std::string& block_hash);

    // Helper: check if adding orphan would exceed limits
    bool canAddOrphan(const std::string& peer_id, size_t block_size) const;

    // Helper: internal remove (assumes lock held)
    void removeOrphanInternal(const std::string& block_hash);
};

} // namespace p2p
} // namespace dinero
