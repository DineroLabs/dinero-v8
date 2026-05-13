/**
 * Phase 27.1: OrphanBlockPool Implementation
 *
 * Bitcoin Core-style orphan block management for parallel sync.
 */

#include "p2p/orphan_block_pool.h"
#include "p2p/block_presence.h"  // Phase 31.2: For HaveBlock() checks
#include "common/logger.h"
#include <algorithm>

namespace dinero {
namespace p2p {

// ============================================================================
// Construction
// ============================================================================

OrphanBlockPool::OrphanBlockPool(const Config& config)
    : config_(config)
{
    g_logger.info("[OrphanBlockPool] Initialized with max_blocks=" +
                 std::to_string(config_.max_orphan_blocks) +
                 ", max_size_mb=" + std::to_string(config_.max_orphan_size_mb));
}

OrphanBlockPool::~OrphanBlockPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    g_logger.info("[OrphanBlockPool] Shutdown: " +
                 std::to_string(stats_.total_orphans) + " orphans, " +
                 std::to_string(stats_.orphans_resolved) + " resolved, " +
                 std::to_string(stats_.orphans_evicted) + " evicted");
}

// ============================================================================
// Core Operations
// ============================================================================

bool OrphanBlockPool::addOrphan(const Block& block, const std::string& block_hash,
                                const std::string& prev_hash, const std::string& peer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already exists (duplicate)
    if (orphans_.find(block_hash) != orphans_.end()) {
        g_logger.debug("[OrphanBlockPool] Duplicate orphan: " + block_hash);
        stats_.orphans_rejected++;
        return false;
    }

    // Phase 31.2: Reject orphans whose parents are already known or being fetched
    // Bitcoin Core behavior: Don't store orphans for blocks we already have or are downloading
    if (block_presence_) {
        BlockPresence parent_state = block_presence_->getBlockPresence(prev_hash);
        if (parent_state != BlockPresence::UNKNOWN) {
            const char* state_str =
                (parent_state == BlockPresence::HAVE_BLOCK) ? "HAVE_BLOCK" :
                (parent_state == BlockPresence::HEADER_ONLY) ? "HEADER_ONLY" :
                (parent_state == BlockPresence::IN_FLIGHT) ? "IN_FLIGHT" : "UNKNOWN";

            g_logger.debug("[OrphanBlockPool] Rejecting orphan " + block_hash.substr(0, 16) +
                          "... parent " + prev_hash.substr(0, 16) + "... already known (state: " +
                          std::string(state_str) + ")");
            stats_.orphans_rejected++;
            return false;
        }
    }

    // Estimate block size (header + transactions)
    // Dinero uses 128-byte headers (BlockHeader v1)
    size_t block_size = 128; // Dinero BlockHeader v1
    for (const auto& tx : block.vtx) {
        block_size += tx.Serialize().size() / 2; // Hex string to bytes
    }

    // Check limits before adding
    if (!canAddOrphan(peer_id, block_size)) {
        g_logger.warning("[OrphanBlockPool] Cannot add orphan: limits exceeded");

        // Try LRU eviction to make space
        if (stats_.total_orphans >= config_.max_orphan_blocks) {
            if (!evictLRU()) {
                stats_.orphans_rejected++;
                return false;
            }
        } else {
            stats_.orphans_rejected++;
            return false;
        }
    }

    // Create orphan entry
    auto orphan = std::make_shared<OrphanBlock>(block, block_hash, prev_hash, peer_id, block_size);

    // Add to primary storage
    orphans_[block_hash] = orphan;

    // Add to parent index
    orphans_by_parent_[prev_hash].insert(block_hash);

    // Add to peer index
    orphans_by_peer_[peer_id].insert(block_hash);

    // Add to LRU list (newest at back)
    lru_list_.push_back(block_hash);

    // Update stats
    stats_.total_orphans++;
    stats_.total_size_bytes += block_size;
    stats_.orphans_added++;

    g_logger.info("[OrphanBlockPool] Added orphan " + block_hash.substr(0, 16) +
                 "... (parent: " + prev_hash.substr(0, 16) + "..., peer: " + peer_id +
                 ", total: " + std::to_string(stats_.total_orphans) + ")");

    return true;
}

std::shared_ptr<OrphanBlock> OrphanBlockPool::getOrphan(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orphans_.find(block_hash);
    if (it == orphans_.end()) {
        return nullptr;
    }

    // Touch for LRU (move to back)
    touchOrphan(block_hash);

    return it->second;
}

std::vector<std::shared_ptr<OrphanBlock>> OrphanBlockPool::getOrphansForParent(
    const std::string& parent_hash)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<OrphanBlock>> result;

    auto it = orphans_by_parent_.find(parent_hash);
    if (it == orphans_by_parent_.end()) {
        return result; // No orphans for this parent
    }

    // Collect all orphans waiting for this parent
    for (const auto& orphan_hash : it->second) {
        auto orphan_it = orphans_.find(orphan_hash);
        if (orphan_it != orphans_.end()) {
            result.push_back(orphan_it->second);
        }
    }

    return result;
}

bool OrphanBlockPool::removeOrphan(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orphans_.find(block_hash);
    if (it == orphans_.end()) {
        return false;
    }

    removeOrphanInternal(block_hash);
    return true;
}

bool OrphanBlockPool::hasOrphan(const std::string& block_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return orphans_.find(block_hash) != orphans_.end();
}

// ============================================================================
// Orphan Resolution
// ============================================================================

std::vector<std::shared_ptr<OrphanBlock>> OrphanBlockPool::resolveOrphans(
    const std::string& parent_hash)
{
    if (!config_.enable_resolution) {
        return {};
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::shared_ptr<OrphanBlock>> resolved;

    // Get immediate children
    auto it = orphans_by_parent_.find(parent_hash);
    if (it == orphans_by_parent_.end()) {
        return resolved; // No orphans for this parent
    }

    // Copy hash set (we'll modify it during iteration)
    auto orphan_hashes = it->second;

    for (const auto& orphan_hash : orphan_hashes) {
        auto orphan_it = orphans_.find(orphan_hash);
        if (orphan_it == orphans_.end()) {
            continue; // Already removed
        }

        auto orphan = orphan_it->second;
        resolved.push_back(orphan);

        // Remove from pool (it's now processable)
        removeOrphanInternal(orphan_hash);

        stats_.orphans_resolved++;

        g_logger.info("[OrphanBlockPool] Resolved orphan " + orphan_hash.substr(0, 16) +
                     "... (parent: " + parent_hash.substr(0, 16) + "...)");
    }

    return resolved;
}

// ============================================================================
// DoS Protection & Maintenance
// ============================================================================

size_t OrphanBlockPool::evictStaleOrphans() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    size_t evicted = 0;

    // Iterate LRU list (oldest first)
    auto it = lru_list_.begin();
    while (it != lru_list_.end()) {
        const auto& orphan_hash = *it;
        auto orphan_it = orphans_.find(orphan_hash);

        if (orphan_it == orphans_.end()) {
            // Already removed, clean up LRU
            it = lru_list_.erase(it);
            continue;
        }

        auto orphan = orphan_it->second;
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - orphan->received_time).count();

        if (age > static_cast<int64_t>(config_.orphan_timeout_seconds)) {
            g_logger.warning("[OrphanBlockPool] Evicting stale orphan " +
                           orphan_hash.substr(0, 16) + "... (age: " +
                           std::to_string(age) + "s)");

            removeOrphanInternal(orphan_hash);
            it = lru_list_.erase(it);
            evicted++;
            stats_.orphans_evicted++;
        } else {
            // LRU list is sorted, so if this one isn't stale, rest aren't either
            break;
        }
    }

    if (evicted > 0) {
        g_logger.info("[OrphanBlockPool] Evicted " + std::to_string(evicted) + " stale orphans");
    }

    return evicted;
}

size_t OrphanBlockPool::evictOrphansFromPeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orphans_by_peer_.find(peer_id);
    if (it == orphans_by_peer_.end()) {
        return 0;
    }

    // Copy hash set (we'll modify it during iteration)
    auto orphan_hashes = it->second;
    size_t evicted = 0;

    for (const auto& orphan_hash : orphan_hashes) {
        removeOrphanInternal(orphan_hash);
        evicted++;
        stats_.orphans_evicted++;
    }

    g_logger.warning("[OrphanBlockPool] Evicted " + std::to_string(evicted) +
                    " orphans from peer " + peer_id);

    return evicted;
}

bool OrphanBlockPool::evictLRU() {
    // Assumes lock is already held by caller

    if (lru_list_.empty()) {
        return false;
    }

    // LRU is oldest first
    const auto& oldest_hash = lru_list_.front();

    g_logger.debug("[OrphanBlockPool] Evicting LRU orphan: " + oldest_hash.substr(0, 16) + "...");

    removeOrphanInternal(oldest_hash);
    lru_list_.pop_front();
    stats_.orphans_evicted++;

    return true;
}

void OrphanBlockPool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = orphans_.size();

    orphans_.clear();
    orphans_by_parent_.clear();
    orphans_by_peer_.clear();
    lru_list_.clear();

    stats_.total_orphans = 0;
    stats_.total_size_bytes = 0;

    g_logger.info("[OrphanBlockPool] Cleared " + std::to_string(count) + " orphans");
}

// ============================================================================
// Statistics & Monitoring
// ============================================================================

OrphanBlockPool::Stats OrphanBlockPool::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Update max orphans per peer
    Stats stats = stats_;
    size_t max_per_peer = 0;
    for (const auto& [peer_id, hashes] : orphans_by_peer_) {
        max_per_peer = std::max(max_per_peer, hashes.size());
    }
    stats.max_orphans_per_peer = max_per_peer;

    return stats;
}

size_t OrphanBlockPool::getOrphanCountForPeer(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orphans_by_peer_.find(peer_id);
    if (it == orphans_by_peer_.end()) {
        return 0;
    }

    return it->second.size();
}

size_t OrphanBlockPool::getTotalSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_.total_size_bytes;
}

bool OrphanBlockPool::isFull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_.total_orphans >= config_.max_orphan_blocks ||
           stats_.total_size_bytes >= (config_.max_orphan_size_mb * 1024 * 1024);
}

// ============================================================================
// Private Helpers
// ============================================================================

void OrphanBlockPool::touchOrphan(const std::string& block_hash) {
    // Move to back of LRU (most recently used)
    // Assumes lock is already held

    auto it = std::find(lru_list_.begin(), lru_list_.end(), block_hash);
    if (it != lru_list_.end()) {
        lru_list_.erase(it);
        lru_list_.push_back(block_hash);
    }
}

bool OrphanBlockPool::canAddOrphan(const std::string& peer_id, size_t block_size) const {
    // Assumes lock is already held

    // Check total count limit
    if (stats_.total_orphans >= config_.max_orphan_blocks) {
        return false;
    }

    // Check total size limit
    if (stats_.total_size_bytes + block_size >= config_.max_orphan_size_mb * 1024 * 1024) {
        return false;
    }

    // Check per-peer limit (DoS protection)
    auto it = orphans_by_peer_.find(peer_id);
    if (it != orphans_by_peer_.end()) {
        if (it->second.size() >= config_.max_orphans_per_peer) {
            g_logger.warning("[OrphanBlockPool] Peer " + peer_id + " exceeded orphan limit");
            return false;
        }
    }

    return true;
}

void OrphanBlockPool::removeOrphanInternal(const std::string& block_hash) {
    // Assumes lock is already held

    auto it = orphans_.find(block_hash);
    if (it == orphans_.end()) {
        return;
    }

    auto orphan = it->second;

    // Remove from parent index
    auto parent_it = orphans_by_parent_.find(orphan->prev_hash);
    if (parent_it != orphans_by_parent_.end()) {
        parent_it->second.erase(block_hash);
        if (parent_it->second.empty()) {
            orphans_by_parent_.erase(parent_it);
        }
    }

    // Remove from peer index
    auto peer_it = orphans_by_peer_.find(orphan->peer_id);
    if (peer_it != orphans_by_peer_.end()) {
        peer_it->second.erase(block_hash);
        if (peer_it->second.empty()) {
            orphans_by_peer_.erase(peer_it);
        }
    }

    // Update stats
    stats_.total_orphans--;
    stats_.total_size_bytes -= orphan->block_size;

    // Remove from primary storage
    orphans_.erase(it);

    // LRU removal handled by caller
}

} // namespace p2p
} // namespace dinero
