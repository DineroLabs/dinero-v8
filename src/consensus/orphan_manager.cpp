#include "consensus/orphan_manager.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {

// Global orphan manager instance
OrphanBlockManager g_orphan_manager;

/**
 * Add orphan to pool
 *
 * Returns false if rejected due to limits (pool full or per-peer limit reached).
 * Implements automatic eviction when pool is full.
 */
bool OrphanBlockManager::AddOrphan(CBlockIndex* pindex, uint64_t peer_id) {
    if (!pindex) return false;

    const uint256& block_hash = pindex->GetBlockHash();
    const uint256& parent_hash = pindex->prev_hash;

    // Check if already in pool
    if (orphan_metadata_.count(block_hash)) {
        g_logger.log(LogLevel::DEBUG, "Orphan already in pool");
        return true; // Already have it
    }

    // Check per-peer limit
    if (!CanAddOrphanFromPeer(peer_id)) {
        g_logger.log(LogLevel::WARNING, "Per-peer orphan limit reached: peer=" + std::to_string(peer_id));
        IncrementEvictionCounter("peer_limit");
        return false;
    }

    // Enforce pool size limit (evict oldest if full)
    if (orphan_metadata_.size() >= MAX_ORPHAN_BLOCKS) {
        g_logger.log(LogLevel::INFO, "Orphan pool full, evicting oldest");
        EvictOldestOrphan();
    }

    // Add to pool
    orphan_metadata_.emplace(block_hash, OrphanBlockMetadata(pindex, peer_id, parent_hash));
    orphans_by_parent_[parent_hash].push_back(block_hash);
    orphans_by_peer_[peer_id].insert(block_hash);

    stats_.total_orphans = orphan_metadata_.size();

    g_logger.log(LogLevel::DEBUG, "Added orphan to pool: hash=" + block_hash.GetHex().substr(0, 16) +
        " parent=" + parent_hash.GetHex().substr(0, 16) +
        " peer=" + std::to_string(peer_id) +
        " pool_size=" + std::to_string(orphan_metadata_.size()));

    return true;
}

/**
 * Remove orphan from pool
 */
void OrphanBlockManager::RemoveOrphan(const uint256& block_hash) {
    auto it = orphan_metadata_.find(block_hash);
    if (it == orphan_metadata_.end()) {
        return; // Not in pool
    }

    const OrphanBlockMetadata& meta = it->second;
    const uint256& parent_hash = meta.parent_hash;
    uint64_t peer_id = meta.peer_id;

    // Remove from parent index
    auto& siblings = orphans_by_parent_[parent_hash];
    siblings.erase(std::remove(siblings.begin(), siblings.end(), block_hash), siblings.end());
    if (siblings.empty()) {
        orphans_by_parent_.erase(parent_hash);
    }

    // Remove from peer index
    orphans_by_peer_[peer_id].erase(block_hash);
    if (orphans_by_peer_[peer_id].empty()) {
        orphans_by_peer_.erase(peer_id);
    }

    // Remove metadata
    orphan_metadata_.erase(it);

    stats_.total_orphans = orphan_metadata_.size();

    g_logger.log(LogLevel::DEBUG, "Removed orphan from pool");
}

/**
 * Get all orphans waiting for specific parent
 */
std::vector<CBlockIndex*> OrphanBlockManager::GetOrphansForParent(const uint256& parent_hash) {
    std::vector<CBlockIndex*> result;

    auto it = orphans_by_parent_.find(parent_hash);
    if (it == orphans_by_parent_.end()) {
        return result; // No orphans for this parent
    }

    for (const uint256& orphan_hash : it->second) {
        auto meta_it = orphan_metadata_.find(orphan_hash);
        if (meta_it != orphan_metadata_.end()) {
            result.push_back(meta_it->second.pindex);
        }
    }

    return result;
}

/**
 * Check if block is in orphan pool
 */
bool OrphanBlockManager::IsOrphan(const uint256& block_hash) const {
    return orphan_metadata_.count(block_hash) > 0;
}

/**
 * Evict expired orphans (older than 24 hours)
 */
void OrphanBlockManager::EvictExpiredOrphans() {
    std::vector<uint256> expired;

    for (const auto& [hash, meta] : orphan_metadata_) {
        if (meta.IsExpired()) {
            expired.push_back(hash);
        }
    }

    for (const uint256& hash : expired) {
        g_logger.log(LogLevel::INFO, "Evicting expired orphan");
        RemoveOrphan(hash);
        IncrementEvictionCounter("expired");
    }

    if (!expired.empty()) {
        g_logger.log(LogLevel::INFO, "Evicted expired orphans: count=" + std::to_string(expired.size()));
    }
}

/**
 * Evict oldest orphan (by arrival time)
 */
void OrphanBlockManager::EvictOldestOrphan() {
    uint256 oldest_hash = FindOldestOrphan();
    if (oldest_hash == uint256()) return;

    g_logger.log(LogLevel::INFO, "Evicting oldest orphan");

    RemoveOrphan(oldest_hash);
    IncrementEvictionCounter("size_limit");
}

/**
 * Evict all orphans from specific peer
 */
void OrphanBlockManager::EvictOrphansFromPeer(uint64_t peer_id) {
    auto it = orphans_by_peer_.find(peer_id);
    if (it == orphans_by_peer_.end()) {
        return; // No orphans from this peer
    }

    std::vector<uint256> to_remove(it->second.begin(), it->second.end());

    for (const uint256& hash : to_remove) {
        RemoveOrphan(hash);
    }

    g_logger.log(LogLevel::INFO, "Evicted all orphans from peer: peer=" + std::to_string(peer_id) +
        " count=" + std::to_string(to_remove.size()));
}

/**
 * Enforce pool size limit
 */
void OrphanBlockManager::EnforcePoolLimits() {
    // Evict oldest orphans until under limit
    while (orphan_metadata_.size() > MAX_ORPHAN_BLOCKS) {
        EvictOldestOrphan();
    }
}

/**
 * Clear all orphans (for shutdown/reset)
 */
void OrphanBlockManager::ClearAll() {
    orphan_metadata_.clear();
    orphans_by_parent_.clear();
    orphans_by_peer_.clear();
    stats_.total_orphans = 0;

    g_logger.log(LogLevel::INFO, "Cleared orphan pool");
}

/**
 * Get pool statistics
 */
OrphanPoolStats OrphanBlockManager::GetStats() const {
    stats_.total_orphans = orphan_metadata_.size();

    // Calculate oldest orphan age
    if (!orphan_metadata_.empty()) {
        uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        uint64_t oldest = now;
        for (const auto& [_, meta] : orphan_metadata_) {
            if (meta.arrival_time < oldest) {
                oldest = meta.arrival_time;
            }
        }
        stats_.oldest_orphan_age = now - oldest;
    } else {
        stats_.oldest_orphan_age = 0;
    }

    return stats_;
}

/**
 * Get orphan count for specific peer
 */
uint32_t OrphanBlockManager::GetOrphanCountForPeer(uint64_t peer_id) const {
    auto it = orphans_by_peer_.find(peer_id);
    if (it == orphans_by_peer_.end()) {
        return 0;
    }
    return it->second.size();
}

/**
 * Check if peer can add more orphans
 */
bool OrphanBlockManager::CanAddOrphanFromPeer(uint64_t peer_id) const {
    return GetOrphanCountForPeer(peer_id) < MAX_ORPHAN_PER_PEER;
}

/**
 * Find oldest orphan by arrival time
 */
uint256 OrphanBlockManager::FindOldestOrphan() const {
    if (orphan_metadata_.empty()) return uint256();

    uint256 oldest_hash;
    uint64_t oldest_time = UINT64_MAX;

    for (const auto& [hash, meta] : orphan_metadata_) {
        if (meta.arrival_time < oldest_time) {
            oldest_time = meta.arrival_time;
            oldest_hash = hash;
        }
    }

    return oldest_hash;
}

/**
 * Increment eviction counter
 */
void OrphanBlockManager::IncrementEvictionCounter(const std::string& reason) {
    stats_.total_evictions++;

    if (reason == "expired") {
        stats_.expired_evictions++;
    } else if (reason == "size_limit") {
        stats_.size_limit_evictions++;
    } else if (reason == "peer_limit") {
        stats_.peer_limit_evictions++;
    }
}

// === Backward compatibility API ===

bool AddOrphanBlock(CBlockIndex* pindex, uint64_t peer_id) {
    return g_orphan_manager.AddOrphan(pindex, peer_id);
}

void RemoveOrphanBlock(const uint256& block_hash) {
    g_orphan_manager.RemoveOrphan(block_hash);
}

std::vector<CBlockIndex*> GetOrphansWaitingForParent(const uint256& parent_hash) {
    return g_orphan_manager.GetOrphansForParent(parent_hash);
}

void MaintainOrphanPool() {
    // Evict expired orphans (older than 24 hours)
    g_orphan_manager.EvictExpiredOrphans();

    // Enforce pool size limits
    g_orphan_manager.EnforcePoolLimits();

    // Log statistics
    OrphanPoolStats stats = g_orphan_manager.GetStats();
    if (stats.total_orphans > 0) {
        g_logger.log(LogLevel::INFO, "Orphan pool maintenance: total=" + std::to_string(stats.total_orphans) +
            " evictions=" + std::to_string(stats.total_evictions));
    }
}

} // namespace dinero
