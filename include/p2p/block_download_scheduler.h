// Copyright (c) 2025 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "network/types.h"
#include "primitives/uint256.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <memory>
#include <mutex>
#include <functional>

namespace dinero {

/**
 * Sync phase (Phase G.12)
 * Determines scheduling behavior based on sync state
 */
enum class SyncPhase {
    IBD,           // Initial Block Download - aggressive parallelism
    CATCHING_UP,   // Behind but syncing - balanced approach
    STEADY_STATE   // Caught up - conservative, network-friendly
};

/**
 * Block download request
 */
struct BlockDownloadRequest {
    uint256 block_hash;
    uint32_t height{0};
    int64_t timestamp{0};         // When request was created
    peer_id_t requested_from;     // Peer handling this request
    bool in_flight{false};        // Currently downloading
};

/**
 * BlockDownloadScheduler — Single authority for block download decisions
 *
 * AUTHORITY: Block download scheduling (what to download, when, from whom)
 * DOES NOT: Validate blocks, manage connections, send network messages directly
 *
 * Responsibilities:
 * 1. Maintain priority queue of blocks to download (based on height)
 * 2. Prevent duplicate requests (integrate with InFlightManager pattern)
 * 3. Route requests through callback (active P2P service sends actual GETDATA)
 * 4. Track download timeouts and retry failed requests
 * 5. Select optimal peer for each block download
 *
 * Phase N.0 Inventory Findings (RED FLAG #3):
 * - Block download authority was split between InventoryHandler and legacy peer routing
 * - InventoryHandler decides based on INV messages
 * - A second legacy routing path also decided in parallel
 * - InFlightManager prevents duplicates but doesn't own authority
 * - Need single scheduler to consolidate ALL download decisions
 *
 * Authority Boundaries:
 * - HeaderChainSelector: OWNS header validation
 * - BlockDownloadScheduler: OWNS block download scheduling (this component)
 * - ChainstateService: OWNS block validation (Phase C.1 locked)
 *
 * Integration Pattern:
 * 1. INV handler → scheduleBlock(hash, height, peer_id)
 * 2. Header sync complete → scheduleBlockRange(start, end)
 * 3. Scheduler → callback(peer_id, block_hash) → active P2P routing sends GETDATA
 * 4. BLOCK received → notifyBlockReceived(hash) → mark complete
 */
class BlockDownloadScheduler {
public:
    /**
     * Callback type for sending GETDATA messages
     * @param peer_id Peer to request from
     * @param block_hash Block hash to request
     * @return true if message sent successfully
     */
    using SendGetDataCallback = std::function<bool(peer_id_t peer_id, const uint256& block_hash)>;

    /**
     * Callback type for querying peer performance score (Phase G.11)
     * @param peer_id Peer to query
     * @return Peer score (0-100, higher = better), or -1.0 if peer unknown
     */
    using PeerScoreProvider = std::function<double(const peer_id_t& peer_id)>;

    /**
     * Constructor
     * @param send_callback Callback for sending GETDATA messages
     */
    explicit BlockDownloadScheduler(SendGetDataCallback send_callback);

    /**
     * Schedule a single block for download
     * Called by INV handler when peer announces block
     *
     * @param block_hash Block hash to download
     * @param height Block height (for priority ordering)
     * @param announcing_peer Peer that announced this block
     */
    void scheduleBlock(const uint256& block_hash, uint32_t height, peer_id_t announcing_peer);

    /**
     * Schedule a range of blocks for download
     * Called by header sync when headers-first sync completes
     *
     * @param start_height Starting height (inclusive)
     * @param end_height Ending height (inclusive)
     * @param preferred_peer Optional peer to prefer for downloads
     */
    void scheduleBlockRange(uint32_t start_height, uint32_t end_height, peer_id_t preferred_peer = "");

    /**
     * Notify that a block has been received and validated
     * Removes block from download queue
     *
     * @param block_hash Block hash that was received
     */
    void notifyBlockReceived(const uint256& block_hash);

    /**
     * Notify that a peer has disconnected
     * Reschedules any in-flight requests from that peer
     *
     * @param peer_id Peer that disconnected
     */
    void notifyPeerDisconnected(peer_id_t peer_id);

    /**
     * Process download queue
     * Should be called periodically (e.g., every second)
     * - Starts new downloads for queued blocks
     * - Retries timed-out requests
     */
    void processQueue();

    /**
     * Check if block is currently being downloaded
     *
     * @param block_hash Block hash to check
     * @return true if download in progress
     */
    bool isInFlight(const uint256& block_hash) const;

    /**
     * Get download statistics
     */
    struct Stats {
        uint32_t queued_blocks{0};       // Blocks waiting to download
        uint32_t in_flight_blocks{0};    // Blocks currently downloading
        uint32_t completed_blocks{0};    // Blocks successfully downloaded
        uint32_t failed_blocks{0};       // Blocks that failed/timed out
        uint32_t retry_count{0};         // Number of retries issued
    };
    Stats getStats() const;

    /**
     * Configuration
     */
    void setMaxInFlight(uint32_t max) { max_in_flight_ = max; }
    void setTimeout(int64_t timeout_secs) { timeout_seconds_ = timeout_secs; }
    void setMaxRetries(uint32_t max) { max_retries_ = max; }
    void setMaxPeerInFlight(uint32_t max) { max_peer_in_flight_ = max; }

    /**
     * Register available peers for block downloads
     * @param peer_ids List of peer IDs that can serve blocks
     */
    void registerPeers(const std::vector<peer_id_t>& peer_ids);

    /**
     * Unregister a peer (e.g., when disconnected)
     * @param peer_id Peer to remove from available peers
     */
    void unregisterPeer(peer_id_t peer_id);

    /**
     * Set peer score provider callback (Phase G.11)
     * Enables intelligent peer selection based on external performance metrics
     * @param provider Callback that returns peer scores (0-100, higher = better)
     */
    void setPeerScoreProvider(PeerScoreProvider provider) {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer_score_provider_ = provider;
    }

    // ========================================================================
    // Phase G.12: Sync Phase Awareness
    // ========================================================================

    /**
     * Get current sync phase
     * @return Current sync phase (IBD, CATCHING_UP, or STEADY_STATE)
     */
    SyncPhase getSyncPhase() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return current_sync_phase_;
    }

    /**
     * Set sync phase manually (for testing or external control)
     * @param phase Desired sync phase
     */
    void setSyncPhase(SyncPhase phase) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_sync_phase_ = phase;
        applyPhaseParameters();
    }

    /**
     * Enable automatic sync phase detection
     * @param enabled true to enable automatic detection based on queue size
     */
    void setAutoPhaseDetection(bool enabled) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        auto_phase_detection_ = enabled;
    }

private:
    /**
     * Per-peer performance statistics
     */
    struct PeerStats {
        uint32_t total_downloads{0};      // Total blocks downloaded
        uint32_t failed_downloads{0};     // Failed/timed out downloads
        uint32_t in_flight_count{0};      // Current in-flight downloads
        int64_t total_download_time{0};   // Cumulative download time (seconds)
        int64_t last_success_time{0};     // Timestamp of last successful download

        // Calculate success rate (0.0 to 1.0)
        double getSuccessRate() const {
            if (total_downloads == 0) return 1.0;  // No data = assume good
            return 1.0 - (static_cast<double>(failed_downloads) / total_downloads);
        }

        // Calculate average download time (seconds)
        double getAvgDownloadTime() const {
            uint32_t successful = total_downloads - failed_downloads;
            if (successful == 0) return 999999.0;  // No successful downloads
            return static_cast<double>(total_download_time) / successful;
        }

        // Calculate peer score (higher = better)
        double getScore() const {
            double success_rate = getSuccessRate();
            double avg_time = getAvgDownloadTime();

            // Score formula: success_rate * (1000 / (avg_time + 1))
            // High success rate + low latency = high score
            return success_rate * (1000.0 / (avg_time + 1.0));
        }
    };

    // Configuration
    uint32_t max_in_flight_{16};          // Maximum concurrent downloads
    int64_t timeout_seconds_{60};         // Download timeout (seconds)
    uint32_t max_retries_{3};             // Maximum retry attempts
    uint32_t max_peer_in_flight_{4};      // Maximum downloads per peer

    // Callback for sending GETDATA
    SendGetDataCallback send_callback_;

    // Peer score provider (Phase G.11) - optional, nullptr = use internal scoring
    PeerScoreProvider peer_score_provider_;

    // Phase G.12: Sync phase awareness
    SyncPhase current_sync_phase_{SyncPhase::IBD};  // Start in IBD mode
    bool auto_phase_detection_{true};               // Automatic phase detection enabled by default

    // Phase-specific thresholds (queue size)
    static constexpr size_t IBD_THRESHOLD = 1000;         // > 1000 blocks = IBD
    static constexpr size_t CATCHING_UP_THRESHOLD = 100;  // 100-1000 blocks = catching up
    // < 100 blocks = steady state

    // Download queue (thread-safe)
    mutable std::mutex queue_mutex_;

    // Priority queue (lower height = higher priority)
    std::deque<BlockDownloadRequest> download_queue_;

    // In-flight requests (block_hash → request)
    std::unordered_map<uint256, BlockDownloadRequest> in_flight_;

    // Completed blocks (prevent re-downloading)
    std::unordered_set<uint256> completed_;

    // Failed blocks (track retry attempts)
    std::unordered_map<uint256, uint32_t> retry_count_;

    // Peer tracking (thread-safe)
    mutable std::mutex peer_mutex_;
    std::unordered_map<peer_id_t, PeerStats> peer_stats_;
    std::unordered_set<peer_id_t> available_peers_;

    // Statistics
    mutable std::mutex stats_mutex_;
    Stats stats_;

    /**
     * Get current Unix timestamp
     */
    int64_t getCurrentTime() const;

    /**
     * Start download for next queued block
     * Must be called with queue_mutex_ held
     *
     * @return true if download started
     */
    bool startNextDownload();

    /**
     * Retry timed-out downloads
     * Must be called with queue_mutex_ held
     */
    void retryTimedOutDownloads();

    /**
     * Select best peer for downloading a block
     * Uses peer performance stats to choose optimal peer
     */
    peer_id_t selectPeerForBlock(const BlockDownloadRequest& request);

    /**
     * Update peer stats when download starts
     */
    void recordDownloadStart(peer_id_t peer);

    /**
     * Update peer stats when download completes successfully
     */
    void recordDownloadSuccess(peer_id_t peer, int64_t download_time);

    /**
     * Update peer stats when download fails/times out
     */
    void recordDownloadFailure(peer_id_t peer);

    /**
     * Get list of candidate peers sorted by score (best first)
     */
    std::vector<peer_id_t> getSortedPeersByScore();

    // ========================================================================
    // Phase G.12: Sync Phase Detection & Parameter Application
    // ========================================================================

    /**
     * Detect sync phase based on queue size
     * Must be called with queue_mutex_ held
     */
    void detectSyncPhase();

    /**
     * Apply parameters appropriate for current sync phase
     * Must be called with queue_mutex_ held
     */
    void applyPhaseParameters();
};

} // namespace dinero
