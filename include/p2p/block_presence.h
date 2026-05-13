#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <chrono>
#include <vector>

namespace dinero {
namespace p2p {

/**
 * @file block_presence.h
 * @brief Bitcoin Core-style Block Presence Tracking System
 *
 * Implements efficient tracking of block states to avoid re-downloading
 * blocks and properly manage the block download pipeline.
 *
 * Inspired by Bitcoin Core's mapBlockIndex and block download logic:
 * - https://github.com/bitcoin/bitcoin/blob/master/src/net_processing.cpp
 * - https://github.com/bitcoin/bitcoin/blob/master/src/chain.h
 */

// ═══════════════════════════════════════════════════════════════════════════
// Block Presence State
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Block presence state enum (Bitcoin Core-compatible)
 *
 * Tracks what we know about a block:
 * - UNKNOWN: We don't know anything about this block
 * - HEADER_ONLY: We have the header but not the full block data
 * - HAVE_BLOCK: We have the complete block stored on disk
 * - IN_FLIGHT: Currently downloading this block from a peer
 */
enum class BlockPresence {
    UNKNOWN,      // No information about this block
    HEADER_ONLY,  // Have header from headers-first sync
    HAVE_BLOCK,   // Complete block stored in ChainDB
    IN_FLIGHT     // Currently being downloaded
};

/**
 * @brief Convert BlockPresence to string for logging
 */
inline const char* BlockPresenceToString(BlockPresence presence) {
    switch (presence) {
        case BlockPresence::UNKNOWN: return "UNKNOWN";
        case BlockPresence::HEADER_ONLY: return "HEADER_ONLY";
        case BlockPresence::HAVE_BLOCK: return "HAVE_BLOCK";
        case BlockPresence::IN_FLIGHT: return "IN_FLIGHT";
        default: return "INVALID";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Block Metadata
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Metadata for a block we've heard about
 *
 * Tracks:
 * - Current presence state
 * - Height in the chain (if known)
 * - When we first heard about it
 * - Download state (peer, start time, timeout)
 */
struct BlockMetadata {
    std::string hash;                                       // Block hash (hex)
    BlockPresence presence{BlockPresence::UNKNOWN};         // Current state
    uint32_t height{0};                                     // Chain height (0 if unknown)

    // Download tracking
    std::string downloading_from_peer;                      // Peer ID (if IN_FLIGHT)
    std::chrono::steady_clock::time_point download_start;   // When download started
    std::chrono::steady_clock::time_point first_seen;       // When we first heard about it

    // Statistics
    uint32_t request_count{0};                              // How many times we've requested it

    BlockMetadata() = default;
    explicit BlockMetadata(const std::string& block_hash, BlockPresence state = BlockPresence::UNKNOWN)
        : hash(block_hash), presence(state), first_seen(std::chrono::steady_clock::now()) {}
};

// ═══════════════════════════════════════════════════════════════════════════
// Block Presence Tracker
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @brief Main block presence tracking system
 *
 * Provides Bitcoin Core-style block state management:
 * 1. Track which blocks we have (headers vs full blocks)
 * 2. Track in-flight block downloads
 * 3. Prevent re-downloading existing blocks
 * 4. Support ChainDB integration for persistence
 *
 * Thread-safe for concurrent access from multiple threads.
 */
class BlockPresenceTracker {
public:
    BlockPresenceTracker();
    ~BlockPresenceTracker();

    // ───────────────────────────────────────────────────────────────────────
    // Core Presence Queries
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Check if we have a block (either full block or just header)
     *
     * Bitcoin Core equivalent: mapBlockIndex.count(hash) > 0
     *
     * @param block_hash Block hash (hex string)
     * @return BlockPresence state
     */
    BlockPresence getBlockPresence(const std::string& block_hash) const;

    /**
     * @brief Check if we have the complete block data
     *
     * Bitcoin Core equivalent: pindex->nStatus & BLOCK_HAVE_DATA
     *
     * @param block_hash Block hash (hex string)
     * @return true if we have complete block on disk
     */
    bool haveBlock(const std::string& block_hash) const;

    /**
     * @brief Check if we have only the header (not full block)
     *
     * @param block_hash Block hash (hex string)
     * @return true if we have header but not full block
     */
    bool haveHeaderOnly(const std::string& block_hash) const;

    /**
     * @brief Check if block is currently being downloaded
     *
     * Bitcoin Core equivalent: mapBlocksInFlight.count(hash) > 0
     *
     * @param block_hash Block hash (hex string)
     * @return true if block is in-flight from a peer
     */
    bool isInFlight(const std::string& block_hash) const;

    // ───────────────────────────────────────────────────────────────────────
    // State Updates
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Mark that we have a header for this block
     *
     * Called when MultiPeerHeadersSync validates a header.
     *
     * @param block_hash Block hash (hex string)
     * @param height Block height (for prioritization)
     */
    void markHeaderReceived(const std::string& block_hash, uint32_t height);

    /**
     * @brief Mark that we have the complete block
     *
     * Called when AcceptBlock() successfully validates and stores a block.
     *
     * @param block_hash Block hash (hex string)
     */
    void markBlockReceived(const std::string& block_hash);

    /**
     * @brief Mark block as in-flight (currently downloading)
     *
     * Called when we send a getdata request to a peer.
     *
     * @param block_hash Block hash (hex string)
     * @param peer_id Peer we're downloading from
     */
    void markInFlight(const std::string& block_hash, const std::string& peer_id);

    /**
     * @brief Clear in-flight status (download failed or cancelled)
     *
     * Called on timeout, peer disconnection, or error.
     *
     * @param block_hash Block hash (hex string)
     */
    void clearInFlight(const std::string& block_hash);

    /**
     * @brief Remove block from tracking (cleanup)
     *
     * @param block_hash Block hash (hex string)
     */
    void remove(const std::string& block_hash);

    // ───────────────────────────────────────────────────────────────────────
    // Batch Queries
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Filter hashes to only those we don't have
     *
     * Used to avoid requesting blocks we already have.
     *
     * @param hashes Vector of block hashes
     * @return Vector of hashes we need (not HAVE_BLOCK)
     */
    std::vector<std::string> filterMissingBlocks(const std::vector<std::string>& hashes) const;

    /**
     * @brief Get all blocks currently in-flight
     *
     * @return Set of block hashes being downloaded
     */
    std::unordered_set<std::string> getInFlightBlocks() const;

    /**
     * @brief Get all blocks in-flight from a specific peer
     *
     * @param peer_id Peer identifier
     * @return Set of block hashes being downloaded from this peer
     */
    std::unordered_set<std::string> getInFlightBlocksForPeer(const std::string& peer_id) const;

    // ───────────────────────────────────────────────────────────────────────
    // Statistics and Monitoring
    // ───────────────────────────────────────────────────────────────────────

    /**
     * @brief Get metadata for a block
     *
     * @param block_hash Block hash (hex string)
     * @return BlockMetadata (or empty if not tracked)
     */
    BlockMetadata getMetadata(const std::string& block_hash) const;

    /**
     * @brief Get total number of tracked blocks
     */
    size_t getTrackedBlockCount() const;

    /**
     * @brief Get count by presence state
     */
    size_t getCountByState(BlockPresence state) const;

    /**
     * @brief Clear all tracking state (for testing/reset)
     */
    void clear();

private:
    // Thread synchronization
    mutable std::mutex mutex_;

    // Block metadata storage
    std::unordered_map<std::string, BlockMetadata> blocks_;  // hash -> metadata

    // Fast lookups by state
    std::unordered_set<std::string> headers_only_;  // Blocks with HEADER_ONLY
    std::unordered_set<std::string> have_blocks_;   // Blocks with HAVE_BLOCK
    std::unordered_set<std::string> in_flight_;     // Blocks with IN_FLIGHT

    // Per-peer in-flight tracking (for cleanup on disconnect)
    std::unordered_map<std::string, std::unordered_set<std::string>> peer_inflight_;  // peer_id -> set<hash>

    // Helper methods
    void transitionState(const std::string& block_hash, BlockPresence new_state);
    void removeFromStateSets(const std::string& block_hash, BlockPresence old_state);
    void addToStateSet(const std::string& block_hash, BlockPresence new_state);
};

} // namespace p2p
} // namespace dinero
