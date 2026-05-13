#pragma once

/**
 * Phase G.2 + G.7 + G.8 + G.13: Block Propagation & Headers-First Sync & Compact Blocks
 *
 * Scope:
 * - Announce blocks via inv messages (after successful connect)
 * - Handle getdata requests for blocks
 * - Handle incoming block messages
 * - Route blocks through Phase F consensus (BlockValidator → ActivateBestChain)
 * - Orphan block handling (Phase G.7):
 *   * Store blocks with unknown parents temporarily
 *   * Schedule parent downloads automatically
 *   * Resolve orphans when parents arrive
 *   * DoS protection (limits per peer, total size, expiry)
 * - Headers-first sync (Phase G.8):
 *   * Handle getheaders/headers protocol messages
 *   * Download and validate headers before full blocks
 *   * Parallel block downloads after headers validated
 *   * Integrate with HeaderSyncManager for header chain management
 * - Compact block relay (Phase G.13):
 *   * Bandwidth optimization (~95% reduction)
 *   * Intelligent peer selection based on scoring + sync phase
 *   * Block reconstruction from mempool
 *   * Missing transaction request protocol
 *
 * NOT in scope (deferred to later phases):
 * - Mempool relay
 * - Transaction gossip
 *
 * Safety:
 * - Every block goes through BlockValidator::ConnectBlock()
 * - Never mutate UTXO directly
 * - Never bypass consensus validation
 * - Duplicate/replay protection via seen_blocks set
 */

#include "primitives/block.h"
#include "primitives/transaction.h"
#include "primitives/uint256.h"
#include "p2p/orphan_block_pool.h"    // Phase G.7: Orphan handling
#include "p2p/compact_block.h"        // Phase G.13: Compact blocks
#include "p2p/block_download_scheduler.h"  // Phase W.1: SyncPhase enum
#include <functional>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <map>      // Phase G.10: Peer performance tracking
#include <chrono>   // Phase G.10: Timestamps

namespace dinero {

// Forward declarations
class P2PManager;
class ChainstateService;
class Mempool;  // Phase G.13
class ILogger;
class HeaderSyncManager;  // Phase G.8
class ChainDB;  // Phase W.1: For sync phase detection

/**
 * BlockRelayManager - Phase G.2 minimal block propagation
 *
 * Responsibilities:
 * 1. Announce blocks after successful consensus validation
 * 2. Handle getdata requests for known blocks
 * 3. Route incoming blocks to consensus validation
 * 4. Prevent duplicate block processing
 */
class BlockRelayManager {
public:
    /**
     * Callback type for sending P2P messages
     * @param peer_address Peer to send to ("" = broadcast)
     * @param command Message command (inv, getdata, block)
     * @param payload Message payload
     */
    using SendMessageCallback = std::function<void(
        const std::string& peer_address,
        const std::string& command,
        const std::vector<uint8_t>& payload
    )>;

    /**
     * Callback type for block validation request
     * @param block Block to validate
     * @param peer_address Peer that sent the block
     * @return true if block was accepted
     */
    using ValidateBlockCallback = std::function<bool(
        const Block& block,
        const std::string& peer_address
    )>;

    /**
     * Callback type for block retrieval from ChainDB
     * @param block_hash Hash of block to retrieve
     * @param out_block Output parameter for retrieved block
     * @return true if block was found and retrieved
     */
    using RetrieveBlockCallback = std::function<bool(
        const uint256& block_hash,
        Block& out_block
    )>;

    /**
     * Callback type for checking if block exists (Phase G.7)
     * @param block_hash Hash of block to check
     * @return true if block exists in chain or orphan pool
     */
    using HasBlockCallback = std::function<bool(const uint256& block_hash)>;

    /**
     * Phase P.2: Block data availability status
     * Distinguishes between pruned, unknown, and corrupted blocks
     */
    enum class BlockDataStatus {
        Available,      // Block data is available and can be served
        Pruned,         // Block was intentionally pruned (header exists, data deleted)
        Unknown,        // Block hash is unknown (never seen this block)
        Corrupted       // Block should exist but data is unreadable
    };

    /**
     * Callback type for checking block data status (Phase P.2)
     * @param block_hash Hash of block to check
     * @return Status indicating availability of block data
     */
    using GetBlockStatusCallback = std::function<BlockDataStatus(const uint256& block_hash)>;

    /**
     * Constructor
     * @param logger Logger instance
     * @param scheduler Block download scheduler (optional - enables download coordination)
     */
    explicit BlockRelayManager(ILogger* logger, std::shared_ptr<BlockDownloadScheduler> scheduler = nullptr);

    ~BlockRelayManager() = default;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * Set callback for sending P2P messages
     */
    void SetSendMessageCallback(SendMessageCallback callback) {
        send_message_callback_ = callback;
    }

    /**
     * Set callback for block validation
     */
    void SetValidateBlockCallback(ValidateBlockCallback callback) {
        validate_block_callback_ = callback;
    }

    /**
     * Set callback for block retrieval from ChainDB
     */
    void SetRetrieveBlockCallback(RetrieveBlockCallback callback) {
        retrieve_block_callback_ = callback;
    }

    /**
     * Set callback for checking if block exists (Phase G.7)
     */
    void SetHasBlockCallback(HasBlockCallback callback) {
        has_block_callback_ = callback;
    }

    /**
     * Set callback for checking block data status (Phase P.2)
     */
    void SetGetBlockStatusCallback(GetBlockStatusCallback callback) {
        get_block_status_callback_ = callback;
    }

    /**
     * Set header sync manager (Phase G.8)
     */
    void SetHeaderSyncManager(HeaderSyncManager* manager) {
        header_sync_manager_ = manager;
    }

    // ========================================================================
    // Block Announcement (Outbound)
    // ========================================================================

    /**
     * Announce a block to all peers
     *
     * Called by ChainstateService after BlockValidator::ConnectBlock() succeeds.
     * Broadcasts inv(MSG_BLOCK, block_hash) to all connected peers.
     *
     * Safety: Only announce blocks that passed full consensus validation.
     *
     * @param block_hash Hash of successfully connected block
     */
    void AnnounceBlock(const uint256& block_hash);

    /**
     * Callback type for getting the current best block hash
     * @return Current best tip hash
     */
    using GetBestBlockHashCallback = std::function<uint256()>;

    /**
     * Set callback for getting the current best block hash
     */
    void SetGetBestBlockHashCallback(GetBestBlockHashCallback callback) {
        get_best_block_hash_callback_ = callback;
    }

    /**
     * Announce current tip to all peers (Phase G.X: Fork Resolution)
     *
     * Called periodically or after mining stops to ensure peers
     * know about our best chain. This helps resolve diverged tips
     * after rapid block production.
     *
     * If a peer doesn't have our tip, they'll request it via getdata,
     * which triggers ActivateBestChain if our chain has more work.
     */
    void AnnounceTip();

    // ========================================================================
    // Block Request Handling (Inbound)
    // ========================================================================

    /**
     * Handle inv message from peer
     *
     * If block is unknown, request it via getdata.
     * If block is known (in seen_blocks), ignore.
     *
     * @param peer_address Peer that sent inv
     * @param block_hash Block hash from inv message
     */
    void HandleInv(const std::string& peer_address, const uint256& block_hash);

    /**
     * Handle getdata request from peer
     *
     * If we have the block, send it to the requesting peer.
     * Uses ChainDB to retrieve block.
     *
     * @param peer_address Peer that requested block
     * @param block_hash Requested block hash
     */
    void HandleGetData(const std::string& peer_address, const uint256& block_hash);

    /**
     * Handle incoming block message
     *
     * Routes block to consensus validation via callback.
     * If validation succeeds, adds to seen_blocks.
     * If validation fails, block is rejected (no relay).
     *
     * @param peer_address Peer that sent block
     * @param block Block data
     */
    void HandleBlock(const std::string& peer_address, const Block& block);

    // ========================================================================
    // Phase G.8: Headers-First Sync
    // ========================================================================

    /**
     * Handle getheaders request from peer (Phase G.8)
     *
     * Responds with up to 2000 headers starting from the requested hash.
     *
     * @param peer_address Peer that requested headers
     * @param start_hash Hash to start from (or genesis if unknown)
     * @param stop_hash Hash to stop at (or tip if empty)
     */
    void HandleGetHeaders(const std::string& peer_address,
                         const uint256& start_hash,
                         const uint256& stop_hash = uint256());

    /**
     * Handle incoming headers message (Phase G.8)
     *
     * Routes headers to HeaderSyncManager for validation and chain selection.
     * After headers validated, schedules parallel block downloads.
     *
     * @param peer_address Peer that sent headers
     * @param headers Vector of block headers
     */
    void HandleHeaders(const std::string& peer_address,
                      const std::vector<BlockHeader>& headers);

    /**
     * Request headers from peer (Phase G.8)
     *
     * Sends getheaders message to specified peer.
     *
     * @param peer_address Peer to request from
     * @param start_hash Hash to start from
     */
    void RequestHeaders(const std::string& peer_address, const uint256& start_hash);

    // ========================================================================
    // Phase G.13: Compact Block Relay
    // ========================================================================

    /**
     * Handle incoming compact block message (Phase G.13)
     *
     * Attempts to reconstruct full block from compact block + mempool.
     * If successful: validates block normally.
     * If missing transactions: sends getblocktxn request.
     *
     * @param peer_address Peer that sent compact block
     * @param compact Compact block data
     */
    void HandleCompactBlock(const std::string& peer_address, const CompactBlock& compact);

    /**
     * Handle getblocktxn request (Phase G.13)
     *
     * Peer is requesting missing transactions for block reconstruction.
     * Responds with blocktxn message containing requested transactions.
     *
     * @param peer_address Peer that requested transactions
     * @param request Transaction request (block hash + indexes)
     */
    void HandleGetBlockTxn(const std::string& peer_address, const BlockTransactionsRequest& request);

    /**
     * Handle blocktxn message (Phase G.13)
     *
     * Completes block reconstruction with missing transactions.
     * Validates the completed block normally.
     *
     * @param peer_address Peer that sent missing transactions
     * @param response Missing transactions
     */
    void HandleBlockTxn(const std::string& peer_address, const BlockTransactions& response);

    /**
     * Set mempool for compact block reconstruction
     * @param mempool Mempool instance (non-owning pointer)
     */
    void SetMempool(Mempool* mempool) {
        mempool_ = mempool;
    }

    /**
     * Set ChainDB for sync phase detection (Phase W.1)
     * @param chain_db ChainDB instance (non-owning pointer)
     */
    void SetChainDB(ChainDB* chain_db) {
        chain_db_ = chain_db;
    }

    // ========================================================================
    // State Queries
    // ========================================================================

    /**
     * Check if we've already seen this block
     */
    bool IsBlockSeen(const uint256& block_hash) const;

    /**
     * Check if block is currently in-flight in the relay download scheduler.
     * Used by daemon ingress to distinguish relay-requested blocks from
     * truly unsolicited payloads during sync.
     */
    bool IsBlockDownloadInFlight(const uint256& block_hash) const;

    /**
     * Get count of seen blocks (for debugging)
     */
    size_t GetSeenBlockCount() const;

    /**
     * Process download queue (should be called periodically)
     * Only needed if scheduler is enabled
     */
    void ProcessDownloadQueue();

    /**
     * Get download scheduler (for testing/debugging)
     */
    std::shared_ptr<BlockDownloadScheduler> GetScheduler() const {
        return download_scheduler_;
    }

    // ========================================================================
    // Phase W.1: Mining Intelligence Signals
    // ========================================================================

    /**
     * Get current sync phase (Phase W.1.1)
     *
     * Returns the current blockchain synchronization state:
     * - IBD:          Node is far behind (1000+ blocks)
     * - CATCHING_UP:  Node is close to tip (1-1000 blocks behind)
     * - STEADY_STATE: Node is fully synced
     *
     * Used by BlockAssembler for context-aware mining optimization.
     *
     * @return Current sync phase
     */
    SyncPhase GetCurrentSyncPhase() const;

    /**
     * Get overall compact block reconstruction success rate (Phase W.1.1)
     *
     * Returns the percentage of compact blocks that were successfully
     * reconstructed without needing a getblocktxn round trip.
     *
     * Range: [0.0, 1.0]
     * - 1.0 = Perfect (all compact blocks reconstructed)
     * - 0.5 = Moderate (50% success)
     * - 0.0 = Poor (all required round trips)
     *
     * Used by BlockAssembler for compact-friendly template bias.
     *
     * @return Success rate [0.0, 1.0]
     */
    double GetCompactBlockSuccessRate() const;

    // ========================================================================
    // Phase G.9: Telemetry & Debug Visibility
    // ========================================================================

    /**
     * Block relay statistics for monitoring and debugging
     */
    struct Stats {
        // Block relay metrics
        size_t blocks_seen = 0;              // Total blocks encountered
        size_t blocks_validated = 0;         // Blocks that passed validation
        size_t blocks_rejected = 0;          // Blocks that failed validation
        size_t blocks_relayed = 0;           // Blocks announced to peers

        // Orphan handling (Phase G.7)
        size_t orphans_current = 0;          // Current orphans in pool
        size_t orphans_added = 0;            // Total orphans added
        size_t orphans_resolved = 0;         // Orphans resolved (parent found)
        size_t orphans_evicted = 0;          // Orphans evicted (DoS limits)
        size_t orphan_pool_bytes = 0;        // Current pool size (bytes)

        // Download coordination (Phase G.6.B)
        size_t downloads_queued = 0;         // Blocks queued for download
        size_t downloads_in_flight = 0;      // Active downloads
        size_t downloads_completed = 0;      // Successful downloads
        size_t downloads_failed = 0;         // Failed downloads

        // Headers-first sync (Phase G.8)
        size_t headers_requested = 0;        // getheaders sent
        size_t headers_received = 0;         // headers messages processed
        size_t headers_accepted = 0;         // Headers validated
        size_t headers_rejected = 0;         // Headers rejected

        // Compact blocks (Phase G.14)
        size_t compact_blocks_received = 0;       // Total compact blocks received
        size_t compact_blocks_reconstructed = 0;  // Successfully reconstructed (no round trip)
        size_t compact_blocks_failed = 0;         // Required getblocktxn round trip
        size_t compact_txns_requested = 0;        // Missing transactions requested
        size_t compact_txns_received = 0;         // Missing transactions received
    };

    /**
     * Get current statistics (Phase G.9)
     *
     * Returns snapshot of block relay metrics for monitoring,
     * debugging, and performance analysis.
     *
     * Thread-safe: Uses internal mutex for consistency.
     */
    Stats GetStats() const;

    // ========================================================================
    // Phase G.10: Peer-Aware Intelligence
    // ========================================================================

    /**
     * Per-peer performance metrics for intelligent scheduling
     */
    struct PeerPerformance {
        std::string peer_address;

        // Delivery metrics
        size_t blocks_delivered = 0;         // Blocks successfully received
        size_t blocks_failed = 0;            // Blocks that failed/timed out
        size_t headers_delivered = 0;        // Headers successfully received
        size_t headers_failed = 0;           // Header requests that failed

        // Compact block metrics (Phase G.14)
        size_t compact_blocks_sent = 0;          // Compact blocks sent to peer
        size_t compact_blocks_received = 0;      // Compact blocks received from peer
        size_t compact_blocks_succeeded = 0;     // Reconstructed without round trip
        size_t compact_blocks_failed = 0;        // Needed getblocktxn

        // Timing metrics (milliseconds)
        uint64_t total_response_time_ms = 0; // Cumulative response time
        uint64_t avg_response_time_ms = 0;   // Average response time

        // Timestamps (for recency weighting)
        uint64_t last_success_ms = 0;        // Last successful delivery
        uint64_t last_failure_ms = 0;        // Last failure

        // Derived metrics
        double success_rate = 0.0;           // blocks_delivered / total_attempts
        double compact_success_rate = 0.0;   // compact_blocks_succeeded / compact_blocks_received (Phase G.14)
        double score = 0.0;                  // Overall peer quality score (0-100)

        // Calculate success rate and score
        void UpdateMetrics() {
            size_t total = blocks_delivered + blocks_failed;
            success_rate = (total > 0) ? static_cast<double>(blocks_delivered) / total : 0.0;

            if (blocks_delivered > 0) {
                avg_response_time_ms = total_response_time_ms / blocks_delivered;
            }

            // Phase G.14: Calculate compact block success rate
            size_t compact_total = compact_blocks_succeeded + compact_blocks_failed;
            compact_success_rate = (compact_total > 0) ?
                static_cast<double>(compact_blocks_succeeded) / compact_total : 0.0;

            // Score calculation (0-100):
            // - Base score from success rate (0-60 points)
            // - Latency bonus for fast responses (0-30 points)
            // - Recency bonus for recent activity (0-10 points)
            // - Phase G.14: Compact block penalty for unreliable peers (up to -20 points)
            score = (success_rate * 60.0);

            // Latency bonus: faster = better (assume 1000ms is baseline)
            if (avg_response_time_ms > 0 && avg_response_time_ms < 5000) {
                double latency_factor = 1.0 - (avg_response_time_ms / 5000.0);
                score += (latency_factor * 30.0);
            }

            // Recency bonus: active peers get a boost
            uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            if (last_success_ms > 0) {
                uint64_t age_ms = now_ms - last_success_ms;
                if (age_ms < 60000) {  // Active in last minute
                    score += 10.0;
                } else if (age_ms < 300000) {  // Active in last 5 minutes
                    score += 5.0;
                }
            }

            // Phase G.14: Penalty for poor compact block reliability
            // Only apply if we have enough data (10+ compact blocks)
            if (compact_total >= 10) {
                if (compact_success_rate < 0.3) {
                    // Very unreliable: -20 points
                    score -= 20.0;
                } else if (compact_success_rate < 0.5) {
                    // Somewhat unreliable: -10 points
                    score -= 10.0;
                } else if (compact_success_rate < 0.7) {
                    // Moderately unreliable: -5 points
                    score -= 5.0;
                }
            }

            // Cap score at 100, floor at 0
            if (score > 100.0) score = 100.0;
            if (score < 0.0) score = 0.0;
        }
    };

    /**
     * Get performance metrics for a specific peer
     *
     * Returns performance data for intelligent peer selection.
     * If peer not tracked, returns empty PeerPerformance.
     *
     * @param peer_address Peer to query
     * @return Performance metrics for the peer
     */
    PeerPerformance GetPeerPerformance(const std::string& peer_address) const;

    /**
     * Get performance metrics for all tracked peers
     *
     * Returns map of peer_address -> performance metrics.
     * Useful for scheduler to compare all available peers.
     *
     * @return Map of all peer performance data
     */
    std::map<std::string, PeerPerformance> GetAllPeerPerformance() const;

    /**
     * Record successful block delivery from peer
     *
     * Updates peer performance metrics to reflect successful delivery.
     * Used by scheduler to reward good peers.
     *
     * @param peer_address Peer that delivered the block
     * @param response_time_ms Time taken to deliver (milliseconds)
     */
    void RecordBlockDelivery(const std::string& peer_address, uint64_t response_time_ms = 0);

    /**
     * Record failed block delivery from peer
     *
     * Updates peer performance metrics to reflect failure.
     * Used by scheduler to penalize flaky peers.
     *
     * @param peer_address Peer that failed to deliver
     */
    void RecordBlockFailure(const std::string& peer_address);

    /**
     * Record successful headers delivery from peer
     *
     * @param peer_address Peer that delivered headers
     */
    void RecordHeadersDelivery(const std::string& peer_address);

    /**
     * Record failed headers delivery from peer
     *
     * @param peer_address Peer that failed to deliver headers
     */
    void RecordHeadersFailure(const std::string& peer_address);

    // ========================================================================
    // Phase G.14: Compact Block Telemetry
    // ========================================================================

    /**
     * Record successful compact block reconstruction (no round trip needed)
     *
     * Updates both global stats and per-peer performance metrics.
     * Peer gets credit for delivering a compact block that was successfully
     * reconstructed from mempool without needing getblocktxn.
     *
     * @param peer_address Peer that sent the compact block
     */
    void RecordCompactBlockSuccess(const std::string& peer_address);

    /**
     * Record failed compact block reconstruction (needed getblocktxn round trip)
     *
     * Updates both global stats and per-peer performance metrics.
     * Peer gets penalized for delivering a compact block that required
     * a round trip to request missing transactions.
     *
     * @param peer_address Peer that sent the compact block
     */
    void RecordCompactBlockFailure(const std::string& peer_address);

private:
    struct PendingCompactBlockState {
        CompactBlock compact;
        Block partial_block;
        std::vector<uint32_t> missing_indexes;
    };

    // Logger
    ILogger* logger_;

    // Callbacks
    SendMessageCallback send_message_callback_;
    ValidateBlockCallback validate_block_callback_;
    RetrieveBlockCallback retrieve_block_callback_;
    HasBlockCallback has_block_callback_;  // Phase G.7: Check if block exists
    GetBlockStatusCallback get_block_status_callback_;  // Phase P.2: Check block data status
    GetBestBlockHashCallback get_best_block_hash_callback_;  // Phase G.X: Tip announcement

    // Block download scheduler (Phase G.6.B integration)
    std::shared_ptr<BlockDownloadScheduler> download_scheduler_;

    // Orphan block pool (Phase G.7)
    std::unique_ptr<p2p::OrphanBlockPool> orphan_pool_;

    // Header sync manager (Phase G.8)
    HeaderSyncManager* header_sync_manager_;  // Non-owning pointer

    // Mempool for compact block reconstruction (Phase G.13)
    Mempool* mempool_;  // Non-owning pointer

    // ChainDB for sync phase detection (Phase W.1)
    ChainDB* chain_db_;  // Non-owning pointer

    // Sync state tracking (Phase W.1)
    mutable SyncPhase current_sync_phase_;  // Cached sync phase

    // Compact block reconstruction tracking (Phase G.13)
    mutable std::mutex compact_block_mutex_;
    std::unordered_map<uint256, PendingCompactBlockState> pending_compact_blocks_;  // Awaiting missing txs

    // Seen blocks (duplicate prevention)
    mutable std::mutex seen_blocks_mutex_;
    std::unordered_set<uint256> seen_blocks_;

    // Legacy relay request tracking (when download_scheduler_ is not configured).
    // Allows daemon ingress to identify payloads that were explicitly requested
    // via getdata and distinguish them from truly unsolicited blocks.
    mutable std::mutex requested_blocks_mutex_;
    mutable std::unordered_map<uint256, std::chrono::steady_clock::time_point> requested_blocks_;
    static constexpr uint32_t REQUEST_TRACK_TIMEOUT_SECONDS = 120;

    // Telemetry (Phase G.9)
    mutable std::mutex stats_mutex_;
    Stats stats_;

    // Peer-aware intelligence (Phase G.10)
    mutable std::mutex peer_perf_mutex_;
    std::map<std::string, PeerPerformance> peer_performance_;

    // Helper: Add block to seen set
    void MarkBlockAsSeen(const uint256& block_hash);
    void MarkBlockRequested(const uint256& block_hash);
    void ConsumeBlockRequest(const uint256& block_hash);
    bool IsLegacyBlockRequested(const uint256& block_hash) const;

    // Helper: Serialize inv message
    std::vector<uint8_t> SerializeInv(const uint256& block_hash) const;

    // Helper: Serialize getdata message
    std::vector<uint8_t> SerializeGetData(const uint256& block_hash) const;

    // Helper: Serialize block message
    std::vector<uint8_t> SerializeBlock(const Block& block) const;

    // Phase P.2: Serialize notfound message (for pruned/unavailable blocks)
    std::vector<uint8_t> SerializeNotFound(const uint256& block_hash) const;

    // ========================================================================
    // Phase G.8: Headers-First Serialization Helpers
    // ========================================================================

    // Regression test access (keeps helpers private but testable).
    // The test ensures we never regress to 80-byte Bitcoin-style headers on the wire.
    friend class HeadersWireV1_BlockRelayManagerSerializeHeaders_Uses128ByteHeader_Test;

    // Helper: Serialize getheaders message
    std::vector<uint8_t> SerializeGetHeaders(const uint256& start_hash,
                                             const uint256& stop_hash) const;

    // Helper: Serialize headers message
    std::vector<uint8_t> SerializeHeaders(const std::vector<BlockHeader>& headers) const;

    // Helper: Schedule block downloads from validated headers
    void ScheduleBlocksFromHeaders();

    // ========================================================================
    // Phase G.13: Compact Block Serialization Helpers
    // ========================================================================

    // Helper: Serialize compact block message
    std::vector<uint8_t> SerializeCompactBlock(const CompactBlock& compact) const;

    // Helper: Serialize getblocktxn message
    std::vector<uint8_t> SerializeGetBlockTxn(const BlockTransactionsRequest& request) const;

    // Helper: Serialize blocktxn message
    std::vector<uint8_t> SerializeBlockTxn(const BlockTransactions& response) const;

    // ========================================================================
    // Phase G.7: Orphan Handling Helpers
    // ========================================================================

    /**
     * Process orphans that are now resolvable
     * Called after a block is successfully validated
     */
    void ProcessOrphans(const uint256& parent_hash);

    /**
     * Schedule download of missing parent block
     */
    void ScheduleParentDownload(const uint256& parent_hash, const std::string& peer_address);
};

} // namespace dinero
