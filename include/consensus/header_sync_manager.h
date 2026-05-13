#pragma once

#include "primitives/block.h"
#include "common/status.h"
#include "storage/tip_info.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <memory>
#include <chrono>
#include <mutex>
#include <filesystem>

namespace dinero {

// Forward declarations
class ChainDB;
class IChainManager;

/**
 * ═══════════════════════════════════════════════════════════════════════════
 * Phase H — HeaderSyncManager: Header-First Synchronization Coordinator
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * HeaderSyncManager is an **orchestration-only** component that coordinates
 * header-first synchronization for Dinero nodes. It sits ABOVE the
 * consensus layer (G.3.3-G.3.5) and decides what to download next.
 *
 * ARCHITECTURAL PRINCIPLE: Pure Coordination - Zero Consensus Logic
 *
 * HeaderSyncManager MUST:
 *   ✅ Validate headers (PoW, linkage, timestamps only)
 *   ✅ Build header tree (in-memory + disk persistence)
 *   ✅ Select best header chain (by cumulative chainwork)
 *   ✅ Schedule block downloads (winning chain only)
 *   ✅ Track IBD state (complete when headers == blocks)
 *   ✅ Persist header metadata (restart-safe)
 *
 * HeaderSyncManager MUST NOT:
 *   ❌ Validate transactions (that's G.3.3 ConsensusValidator)
 *   ❌ Mutate UTXOs (that's G.3.4 ConnectBlock/DisconnectBlock)
 *   ❌ Execute reorgs (that's G.3.5 ActivateBestChain)
 *   ❌ Store consensus logic (delegates to frozen layers)
 *   ❌ Bypass existing validation pipeline
 *
 * Data Flow:
 *   P2P (headers) → HeaderSyncManager (validate PoW, select best)
 *                     ↓
 *                   Block download requests
 *                     ↓
 *   P2P (blocks) → BlockAcceptor (G.3.3) → ChainManager (G.3.4/G.3.5)
 *
 * This design respects the consensus architecture freeze and treats
 * HeaderSyncManager as pure orchestration.
 */
class HeaderSyncManager {
public:
    HeaderSyncManager();
    ~HeaderSyncManager();

    // Disable copy and move: internal mutex/state are not safely movable.
    HeaderSyncManager(const HeaderSyncManager&) = delete;
    HeaderSyncManager& operator=(const HeaderSyncManager&) = delete;
    HeaderSyncManager(HeaderSyncManager&&) noexcept = delete;
    HeaderSyncManager& operator=(HeaderSyncManager&&) noexcept = delete;

    // ═══════════════════════════════════════════════════════════════════════
    // Initialization
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Initialize HeaderSyncManager
     *
     * Loads persisted headers from ChainDB, rebuilds in-memory tree,
     * and restores download queue.
     *
     * @param chain_db  Non-owning pointer to ChainDB (for header persistence)
     * @param chain_manager  Non-owning pointer to IChainManager (for active tip queries)
     * @param datadir  Data directory for metadata storage
     * @return true if initialization succeeded
     */
    bool Initialize(ChainDB* chain_db, IChainManager* chain_manager,
                   const std::filesystem::path& datadir);

    /**
     * Shutdown HeaderSyncManager
     *
     * Persists final state to disk.
     */
    void Shutdown();

    // ═══════════════════════════════════════════════════════════════════════
    // Header Processing (from P2P)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Process headers received from a peer
     *
     * Validates headers (PoW, linkage, timestamps), adds to header tree,
     * selects best chain, and updates download queue.
     *
     * VALIDATION PERFORMED:
     *   - PoW check (ASERT difficulty)
     *   - Timestamp check (BIP113 median-time-past)
     *   - Header linkage (previousHash correctness)
     *
     * VALIDATION NOT PERFORMED:
     *   - Transaction validation (that's G.3.3)
     *   - UTXO checks (that's G.3.4)
     *
     * @param peer_id  Peer that sent the headers
     * @param headers  Vector of headers to process
     * @return true if headers were accepted
     */
    bool ProcessHeaders(const std::string& peer_id,
                       const std::vector<BlockHeader>& headers);

    // ═══════════════════════════════════════════════════════════════════════
    // Block Download Scheduling
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Get blocks to download from peers
     *
     * Returns block hashes that need to be downloaded to advance
     * the active chain toward the best header chain.
     *
     * DOWNLOAD RULES:
     *   1. Only blocks on best header chain
     *   2. Only blocks whose parent is already downloaded
     *   3. Oldest blocks first (height order)
     *
     * @param max_blocks  Maximum number of block hashes to return
     * @return Vector of block hashes to download
     */
    std::vector<std::string> GetBlocksToDownload(size_t max_blocks = 16);

    /**
     * Check if a specific block is needed
     *
     * Returns true if the block:
     *   1. Is on the best header chain
     *   2. Has parent already downloaded (connected)
     *   3. Is not already downloaded
     *
     * @param block_hash  Block hash to check
     * @return true if block should be downloaded
     */
    bool IsBlockNeeded(const std::string& block_hash) const;

    /**
     * Mark a block as requested from a peer
     *
     * Tracks in-flight block requests for timeout handling.
     *
     * @param block_hash  Block hash being requested
     * @param peer_id  Peer from which block is requested
     */
    void MarkBlockRequested(const std::string& block_hash,
                           const std::string& peer_id);

    /**
     * Mark a block as received
     *
     * Updates HeaderNode status (BLOCK_HAVE_DATA flag).
     * Called by the active P2P message router when a block arrives (before validation).
     *
     * @param block_hash  Block hash that was received
     * @param peer_id  Peer from which block was received
     */
    void MarkBlockReceived(const std::string& block_hash,
                          const std::string& peer_id);

    /**
     * Mark a block as connected to the active chain
     *
     * Updates HeaderNode status to indicate block is fully validated
     * and applied to UTXO set. Called after G.3.3-G.3.5 succeeds.
     *
     * @param block_hash  Block hash that was connected
     */
    void MarkBlockConnected(const std::string& block_hash);

    /**
     * Mark a block as failed validation
     *
     * Sets BLOCK_FAILED flag to prevent re-downloading.
     * Called after G.3.3 validation fails.
     *
     * @param block_hash  Block hash that failed validation
     */
    void MarkBlockFailed(const std::string& block_hash);

    /**
     * Check for download timeouts and re-queue blocks
     *
     * Should be called periodically (e.g., every 10 seconds).
     * Re-queues blocks that timed out (no response after 60 seconds).
     * Gives up after MAX_DOWNLOAD_ATTEMPTS failures.
     */
    void CheckDownloadTimeouts();

    // ═══════════════════════════════════════════════════════════════════════
    // IBD State Detection
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Check if node is in Initial Block Download (IBD)
     *
     * IBD is complete when:
     *   1. best_header_height == active_tip_height (all headers have blocks)
     *   2. Download queue is empty (no pending downloads)
     *   3. No recent header activity (peers caught up)
     *
     * @return true if still in IBD, false if sync complete
     */
    bool IsInitialBlockDownload() const;

    /**
     * Update IBD state and persist to metadata.dat
     *
     * Should be called after each block connects.
     * Detects IBD completion and updates persistent flag.
     */
    void UpdateIBDState();

    // ═══════════════════════════════════════════════════════════════════════
    // Status Queries
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Get best header height
     *
     * @return Height of best header chain tip
     */
    uint32_t GetBestHeaderHeight() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return best_header_height_;
    }

    /**
     * Get best header hash
     *
     * @return Hash of best header chain tip (as hex string for RPC)
     */
    std::string GetBestHeaderHash() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return best_header_hash_.GetHex();
    }

    /**
     * Get best block height (from ChainManager)
     *
     * @return Height of fully-validated active tip
     */
    uint32_t GetBestBlockHeight() const;

    /**
     * Sync statistics
     */
    struct SyncStats {
        uint32_t best_header_height{0};
        uint32_t best_block_height{0};
        uint32_t blocks_in_flight{0};
        uint32_t headers_downloaded{0};
        uint32_t blocks_downloaded{0};
        uint32_t blocks_pending{0};
        bool is_ibd{true};
        double sync_progress{0.0};  // (blocks / headers) * 100
    };

    /**
     * Get sync statistics
     *
     * @return Current sync statistics
     */
    SyncStats GetStats() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Phase W.2.6 Enhancement #2: Header Sync Status (Separate Header/Block Tracking)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Simple header sync status for Phase W.2.6 RPC surfacing
     *
     * Provides separate tracking of headers vs blocks (eliminates estimation)
     */
    struct HeaderSyncStatus {
        uint32_t headers_synced{0};  ///< Current best header height
        uint32_t headers_target{0};  ///< Target header height (from best peer)
        bool is_syncing{false};      ///< True if actively syncing headers

        /// Calculate header sync progress (0.0 to 1.0)
        double progress() const {
            if (headers_target == 0) return 1.0;
            return std::min(1.0, static_cast<double>(headers_synced) / headers_target);
        }
    };

    /**
     * Get header sync status (Phase W.2.6 RPC integration)
     *
     * @return Current header sync status
     */
    HeaderSyncStatus GetStatus() const;

private:
    // ═══════════════════════════════════════════════════════════════════════
    // Header Tree (In-Memory)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Header node in the in-memory tree
     *
     * Represents a single block header with parent/child linkage.
     * Stores chainwork for fast best-chain selection.
     *
     * HeaderNode::status is the SINGLE SOURCE OF TRUTH for block state.
     * All state queries (IsBlockNeeded, etc.) check this field only.
     */
    struct HeaderNode {
        uint256 hash;
        uint256 prev_hash;
        uint32_t height{0};
        arith_uint256 chainwork;
        BlockHeader header;
        uint32_t status{0};  // Single source of truth for block state

        // Tree linkage (not persisted)
        HeaderNode* parent{nullptr};
        std::vector<HeaderNode*> children;
    };

    // Block status flags (HeaderNode::status is authoritative)
    static constexpr uint32_t BLOCK_VALID_HEADER = 1 << 0;  // Header validated (PoW, linkage)
    static constexpr uint32_t BLOCK_HAVE_DATA    = 1 << 1;  // Block data received from peer
    static constexpr uint32_t BLOCK_FAILED       = 1 << 2;  // Validation failed (permanent)

    // Header index: hash → HeaderNode
    std::unordered_map<uint256, std::unique_ptr<HeaderNode>> header_index_;

    // Best header chain tip
    HeaderNode* best_header_tip_{nullptr};
    uint32_t best_header_height_{0};
    uint256 best_header_hash_;
    arith_uint256 best_header_work_;

    // ═══════════════════════════════════════════════════════════════════════
    // Block Download Queue (Phase H.2)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Download request tracking (Phase H.2)
     *
     * Tracks in-flight block requests for timeout/retry logic.
     * This is an INDEX only - HeaderNode::status is authoritative state.
     */
    struct DownloadRequest {
        uint256 block_hash;
        uint32_t height{0};
        std::string peer_id;
        std::chrono::steady_clock::time_point request_time;
        uint32_t attempt_count{0};  // Phase H.2: Retry tracking

        bool isTimedOut() const {
            auto elapsed = std::chrono::steady_clock::now() - request_time;
            return elapsed > BLOCK_DOWNLOAD_TIMEOUT;
        }
    };

    // Blocks to download (oldest first)
    std::deque<uint256> download_queue_;

    // Blocks being downloaded (INDEX for fast lookup)
    // Note: This is NOT authoritative state - check HeaderNode::status
    std::unordered_map<uint256, DownloadRequest> in_flight_;

    // Download limits (Phase H.2)
    static constexpr size_t MAX_IN_FLIGHT = 16;
    static constexpr uint32_t MAX_DOWNLOAD_ATTEMPTS = 3;
    static constexpr std::chrono::seconds BLOCK_DOWNLOAD_TIMEOUT{60};

    // ═══════════════════════════════════════════════════════════════════════
    // Dependencies (Non-Owning Pointers)
    // ═══════════════════════════════════════════════════════════════════════

    ChainDB* chain_db_{nullptr};
    IChainManager* chain_manager_{nullptr};
    std::filesystem::path datadir_;

    // ═══════════════════════════════════════════════════════════════════════
    // IBD Tracking
    // ═══════════════════════════════════════════════════════════════════════

    bool is_ibd_{true};
    std::chrono::steady_clock::time_point last_header_time_;

    // ═══════════════════════════════════════════════════════════════════════
    // Statistics
    // ═══════════════════════════════════════════════════════════════════════

    uint32_t headers_downloaded_{0};
    uint32_t blocks_downloaded_{0};

    // ═══════════════════════════════════════════════════════════════════════
    // Thread Safety
    // ═══════════════════════════════════════════════════════════════════════

    mutable std::mutex mutex_;

    // ═══════════════════════════════════════════════════════════════════════
    // Header Validation (PoW Only - No Transactions!)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Validate a chain of headers
     *
     * Checks PoW, timestamps, and linkage for all headers in the batch.
     * Rejects entire batch if any header is invalid.
     *
     * @param headers  Headers to validate
     * @param prev_header  Previous header (for linkage check), or nullptr for genesis
     * @return true if all headers are valid
     */
    bool ValidateHeaderChain(const std::vector<BlockHeader>& headers,
                            const BlockHeader* prev_header);

    /**
     * Validate header Proof of Work
     *
     * Verifies hash meets difficulty target using ASERT algorithm.
     *
     * @param header  Header to validate
     * @return true if PoW is valid
     */
    bool ValidateHeaderPoW(const BlockHeader& header) const;

    /**
     * Validate header timestamp
     *
     * Checks median-time-past (BIP113) and future time limit.
     *
     * @param header  Header to validate
     * @param prev_header  Previous header (for median time calculation)
     * @return true if timestamp is valid
     */
    bool ValidateHeaderTimestamp(const BlockHeader& header,
                                 const BlockHeader* prev_header) const;

    // ═══════════════════════════════════════════════════════════════════════
    // Header Tree Management
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Create a header node from a header
     *
     * @param header  Header to convert
     * @param parent  Parent node (or nullptr for genesis)
     * @return Unique pointer to new HeaderNode
     */
    std::unique_ptr<HeaderNode> CreateHeaderNode(const BlockHeader& header,
                                                 HeaderNode* parent);

    /**
     * Find a header node by hash
     *
     * @param hash  Block hash to find
     * @return Pointer to HeaderNode, or nullptr if not found
     */
    HeaderNode* FindHeaderNode(const uint256& hash);

    /**
     * Find a header node by hash (const version)
     *
     * @param hash  Block hash to find
     * @return Pointer to HeaderNode, or nullptr if not found
     */
    const HeaderNode* FindHeaderNode(const uint256& hash) const;

    /**
     * Update best header tip
     *
     * Scans all leaf nodes and selects the one with most chainwork.
     * Uses ByWorkThenHash comparator (work primary, hash tiebreaker).
     */
    void UpdateBestHeaderTip();

    /**
     * Check if a node is on the best header chain
     *
     * @param node  Node to check
     * @return true if node is ancestor of best_header_tip_
     */
    bool IsOnBestHeaderChain(const HeaderNode* node) const;

    /**
     * Check if a block hash is on the best header chain
     *
     * @param block_hash  Block hash to check
     * @return true if block is on best header chain
     */
    bool IsOnBestHeaderChain(const std::string& block_hash) const;

    /**
     * Check if in initial block download (internal, unlocked version)
     *
     * Helper for UpdateIBDState() to avoid mutex re-locking.
     * Assumes mutex is already locked by caller.
     *
     * @return true if still in IBD
     */
    bool IsInitialBlockDownload_Unlocked() const;

    // ═══════════════════════════════════════════════════════════════════════
    // Download Queue Management
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Update download queue
     *
     * Rebuilds queue from best header tip to active tip.
     * Adds missing blocks in height order (oldest first).
     */
    void UpdateDownloadQueue();

    // ═══════════════════════════════════════════════════════════════════════
    // Persistence (Restart Safety)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Load headers from ChainDB CF #3
     *
     * Reads all persisted headers and builds in-memory tree.
     *
     * @return true if load succeeded
     */
    bool LoadHeaderIndex();

    /**
     * Save headers to ChainDB CF #3
     *
     * Persists all headers in header_index_ to disk.
     *
     * @return true if save succeeded
     */
    bool SaveHeaderIndex();

    /**
     * Rebuild header tree after loading
     *
     * Links parent and child pointers after LoadHeaderIndex().
     */
    void RebuildHeaderTree();
};

// Global HeaderSyncManager instance (for RPC and daemon access)
extern std::unique_ptr<HeaderSyncManager> g_header_sync_manager;

} // namespace dinero
