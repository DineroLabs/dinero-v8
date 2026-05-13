#include "p2p/block_presence.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {
namespace p2p {

// ═══════════════════════════════════════════════════════════════════════════
// BlockPresenceTracker Implementation
// ═══════════════════════════════════════════════════════════════════════════

BlockPresenceTracker::BlockPresenceTracker() {
    g_logger.info("BlockPresenceTracker initialized");
}

BlockPresenceTracker::~BlockPresenceTracker() {
    std::lock_guard<std::mutex> lock(mutex_);
    g_logger.info("BlockPresenceTracker shutdown - tracked " + std::to_string(blocks_.size()) + " blocks");
}

// ───────────────────────────────────────────────────────────────────────
// Core Presence Queries
// ───────────────────────────────────────────────────────────────────────

BlockPresence BlockPresenceTracker::getBlockPresence(const std::string& block_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = blocks_.find(block_hash);
    if (it == blocks_.end()) {
        return BlockPresence::UNKNOWN;
    }
    return it->second.presence;
}

bool BlockPresenceTracker::haveBlock(const std::string& block_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return have_blocks_.count(block_hash) > 0;
}

bool BlockPresenceTracker::haveHeaderOnly(const std::string& block_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return headers_only_.count(block_hash) > 0;
}

bool BlockPresenceTracker::isInFlight(const std::string& block_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_.count(block_hash) > 0;
}

// ───────────────────────────────────────────────────────────────────────
// State Updates
// ───────────────────────────────────────────────────────────────────────

void BlockPresenceTracker::markHeaderReceived(const std::string& block_hash, uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = blocks_.find(block_hash);
    if (it != blocks_.end()) {
        // Already have this block or better
        if (it->second.presence == BlockPresence::HAVE_BLOCK) {
            g_logger.debug("markHeaderReceived: already have block " + block_hash.substr(0, 16) + "...");
            return;
        }
    }

    // Create or update metadata
    if (it == blocks_.end()) {
        blocks_[block_hash] = BlockMetadata(block_hash, BlockPresence::HEADER_ONLY);
        blocks_[block_hash].height = height;
        headers_only_.insert(block_hash);

        g_logger.debug("Header received: " + block_hash.substr(0, 16) + "... (height=" + std::to_string(height) + ")");
    } else {
        // Update height if we didn't know it
        if (it->second.height == 0 && height > 0) {
            it->second.height = height;
        }
    }
}

void BlockPresenceTracker::markBlockReceived(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = blocks_.find(block_hash);
    if (it != blocks_.end() && it->second.presence == BlockPresence::HAVE_BLOCK) {
        // Already marked as having this block
        return;
    }

    transitionState(block_hash, BlockPresence::HAVE_BLOCK);

    g_logger.debug("Block received: " + block_hash.substr(0, 16) + "...");
}

void BlockPresenceTracker::markInFlight(const std::string& block_hash, const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Don't mark as in-flight if we already have it
    auto it = blocks_.find(block_hash);
    if (it != blocks_.end() && it->second.presence == BlockPresence::HAVE_BLOCK) {
        g_logger.warning("Attempted to mark block " + block_hash.substr(0, 16) + "... as IN_FLIGHT but we already have it");
        return;
    }

    // Transition to IN_FLIGHT
    transitionState(block_hash, BlockPresence::IN_FLIGHT);

    // Update download tracking
    blocks_[block_hash].downloading_from_peer = peer_id;
    blocks_[block_hash].download_start = std::chrono::steady_clock::now();
    blocks_[block_hash].request_count++;

    // Add to per-peer tracking
    peer_inflight_[peer_id].insert(block_hash);

    g_logger.debug("Block " + block_hash.substr(0, 16) + "... marked IN_FLIGHT from peer " + peer_id);
}

void BlockPresenceTracker::clearInFlight(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = blocks_.find(block_hash);
    if (it == blocks_.end() || it->second.presence != BlockPresence::IN_FLIGHT) {
        return;  // Not in-flight
    }

    // Remove from per-peer tracking
    const std::string& peer_id = it->second.downloading_from_peer;
    if (!peer_id.empty()) {
        auto peer_it = peer_inflight_.find(peer_id);
        if (peer_it != peer_inflight_.end()) {
            peer_it->second.erase(block_hash);
            if (peer_it->second.empty()) {
                peer_inflight_.erase(peer_it);
            }
        }
    }

    // Clear download tracking
    it->second.downloading_from_peer.clear();

    // Revert to previous state (HEADER_ONLY if we had header, otherwise UNKNOWN)
    BlockPresence new_state = (it->second.height > 0) ? BlockPresence::HEADER_ONLY : BlockPresence::UNKNOWN;
    transitionState(block_hash, new_state);

    g_logger.debug("Block " + block_hash.substr(0, 16) + "... cleared from IN_FLIGHT");
}

void BlockPresenceTracker::remove(const std::string& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = blocks_.find(block_hash);
    if (it == blocks_.end()) {
        return;
    }

    // Remove from state sets
    removeFromStateSets(block_hash, it->second.presence);

    // Remove from per-peer tracking if in-flight
    if (it->second.presence == BlockPresence::IN_FLIGHT) {
        const std::string& peer_id = it->second.downloading_from_peer;
        if (!peer_id.empty()) {
            auto peer_it = peer_inflight_.find(peer_id);
            if (peer_it != peer_inflight_.end()) {
                peer_it->second.erase(block_hash);
                if (peer_it->second.empty()) {
                    peer_inflight_.erase(peer_it);
                }
            }
        }
    }

    // Remove metadata
    blocks_.erase(it);
}

// ───────────────────────────────────────────────────────────────────────
// Batch Queries
// ───────────────────────────────────────────────────────────────────────

std::vector<std::string> BlockPresenceTracker::filterMissingBlocks(const std::vector<std::string>& hashes) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> missing;
    missing.reserve(hashes.size());

    for (const auto& hash : hashes) {
        // Only add if we don't have the full block
        if (have_blocks_.count(hash) == 0) {
            missing.push_back(hash);
        }
    }

    return missing;
}

std::unordered_set<std::string> BlockPresenceTracker::getInFlightBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_;  // Return copy
}

std::unordered_set<std::string> BlockPresenceTracker::getInFlightBlocksForPeer(const std::string& peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peer_inflight_.find(peer_id);
    if (it != peer_inflight_.end()) {
        return it->second;  // Return copy
    }
    return {};
}

// ───────────────────────────────────────────────────────────────────────
// Statistics and Monitoring
// ───────────────────────────────────────────────────────────────────────

BlockMetadata BlockPresenceTracker::getMetadata(const std::string& block_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = blocks_.find(block_hash);
    if (it != blocks_.end()) {
        return it->second;  // Return copy
    }
    return BlockMetadata();  // Empty metadata
}

size_t BlockPresenceTracker::getTrackedBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocks_.size();
}

size_t BlockPresenceTracker::getCountByState(BlockPresence state) const {
    std::lock_guard<std::mutex> lock(mutex_);

    switch (state) {
        case BlockPresence::HEADER_ONLY:
            return headers_only_.size();
        case BlockPresence::HAVE_BLOCK:
            return have_blocks_.size();
        case BlockPresence::IN_FLIGHT:
            return in_flight_.size();
        case BlockPresence::UNKNOWN:
            // Count blocks with UNKNOWN state explicitly
            return std::count_if(blocks_.begin(), blocks_.end(),
                                [](const auto& pair) { return pair.second.presence == BlockPresence::UNKNOWN; });
        default:
            return 0;
    }
}

void BlockPresenceTracker::clear() {
    std::lock_guard<std::mutex> lock(mutex_);

    blocks_.clear();
    headers_only_.clear();
    have_blocks_.clear();
    in_flight_.clear();
    peer_inflight_.clear();

    g_logger.info("BlockPresenceTracker cleared");
}

// ───────────────────────────────────────────────────────────────────────
// Helper Methods
// ───────────────────────────────────────────────────────────────────────

void BlockPresenceTracker::transitionState(const std::string& block_hash, BlockPresence new_state) {
    // Must be called with mutex_ locked

    auto it = blocks_.find(block_hash);
    BlockPresence old_state = BlockPresence::UNKNOWN;

    if (it != blocks_.end()) {
        old_state = it->second.presence;
        if (old_state == new_state) {
            return;  // No change
        }
        removeFromStateSets(block_hash, old_state);
        it->second.presence = new_state;
    } else {
        // Create new entry
        blocks_[block_hash] = BlockMetadata(block_hash, new_state);
    }

    addToStateSet(block_hash, new_state);
}

void BlockPresenceTracker::removeFromStateSets(const std::string& block_hash, BlockPresence old_state) {
    // Must be called with mutex_ locked

    switch (old_state) {
        case BlockPresence::HEADER_ONLY:
            headers_only_.erase(block_hash);
            break;
        case BlockPresence::HAVE_BLOCK:
            have_blocks_.erase(block_hash);
            break;
        case BlockPresence::IN_FLIGHT:
            in_flight_.erase(block_hash);
            break;
        case BlockPresence::UNKNOWN:
            // Not in any set
            break;
    }
}

void BlockPresenceTracker::addToStateSet(const std::string& block_hash, BlockPresence new_state) {
    // Must be called with mutex_ locked

    switch (new_state) {
        case BlockPresence::HEADER_ONLY:
            headers_only_.insert(block_hash);
            break;
        case BlockPresence::HAVE_BLOCK:
            have_blocks_.insert(block_hash);
            break;
        case BlockPresence::IN_FLIGHT:
            in_flight_.insert(block_hash);
            break;
        case BlockPresence::UNKNOWN:
            // Not added to any set
            break;
    }
}

} // namespace p2p
} // namespace dinero
