/**
 * Phase N.4: Blocks-after-Headers Download Scheduler
 *
 * Purpose: Download block bodies for validated headers.
 *
 * Responsibilities:
 * - Identify missing blocks (headers without bodies)
 * - Schedule sequential block downloads (one at a time)
 * - Validate blocks against headers
 * - Store blocks (but NOT activate chainstate)
 *
 * Constraints (Locked):
 * ❌ NO ActivateBestChain
 * ❌ NO UTXO mutation
 * ❌ NO parallelism (one at a time)
 * ❌ NO reorg logic
 * ✅ Download + verify only
 * ✅ Single-threaded, deterministic order
 *
 * Architecture:
 * - Scheduler decides intent (which blocks to fetch)
 * - P2P layer executes (sends getdata messages)
 * - Callbacks translate intent → action (like Phase N.3)
 */

#pragma once

#include "primitives/uint256.h"
#include "primitives/block.h"
#include "storage/block_storage.h"         // For FilePosition
#include "p2p/block_download_scheduler.h"  // For SyncPhase enum (Phase W.2.6 Enhancement #3)
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdint>

namespace dinero {

// Forward declaration for block storage
class BlockStorage;

namespace consensus {

// Forward declarations
class HeaderChainSelector;

// ============================================================================
// Block Fetch State
// ============================================================================

enum class FetchStatus {
    MISSING,     // Header exists, body not downloaded
    REQUESTED,   // getdata sent, waiting for response
    RECEIVED,    // Block downloaded and verified (stored to flat file)
    CONNECTED,   // Block connected to chainstate (chain tip advanced)
    INVALID      // Block failed connection validation (permanent stop)
};

struct BlockFetchState {
    uint256 block_hash;
    uint32_t height;
    FetchStatus status;
    FilePosition stored_pos;  // Set when status becomes RECEIVED
    std::chrono::steady_clock::time_point request_time;  // When REQUESTED was set

    BlockFetchState(const uint256& hash, uint32_t h)
        : block_hash(hash), height(h), status(FetchStatus::MISSING) {}
};

// Result classification for scheduler-driven chainstate connection.
// This is richer than a bool so TryConnectStoredBlocks can make deterministic
// recovery decisions without re-request loops.
enum class ConnectBlockResult {
    CONNECTED,           // Block is now on the active chain
    ACCEPTED_NOT_ACTIVE, // Block accepted/indexed but not active at this height yet
    INVALID,             // Invalid block (do not re-request this child)
    MISSING_PARENT,      // Parent block/header not present yet
    WAITING_PARENT,      // Parent is known/queued but not connected yet
    DUPLICATE,           // Already connected/known
    TEMPORARY_FAIL       // Retry same child later
};

// ============================================================================
// Block Download Scheduler
// ============================================================================

class BlockDownloadScheduler {
public:
    /**
     * Construct block download scheduler.
     *
     * @param header_chain Header chain to observe for missing blocks
     * @param block_storage Block storage for persisting downloaded blocks (optional)
     */
    explicit BlockDownloadScheduler(HeaderChainSelector* header_chain,
                                    dinero::BlockStorage* block_storage = nullptr);

    ~BlockDownloadScheduler();

    // ========================================================================
    // Core Methods
    // ========================================================================

    /**
     * Called when headers are processed.
     * Identifies new headers without bodies and queues them for download.
     */
    void OnHeadersProcessed();

    /** True after OnHeadersProcessed() has been called at least once. */
    bool HeadersSynced() const { return headers_processed_.load(); }

    /**
     * Called when a peer returns NOTFOUND for a block we requested.
     * Clears the hash from in-flight tracking and triggers a rate-limited
     * rescan from the actual chainstate tip, dropping stale-branch entries
     * that will never be served.
     *
     * @param hash Hash of the block the peer doesn't have
     */
    void OnBlockNotFound(const uint256& hash);

    /**
     * Same as above, but records WHICH peer returned NOTFOUND (issue #241).
     * A peer that NOTFOUNDs a block at height H is treated as lacking block
     * *bodies* at all heights <= H — e.g. AssumeUTXO snapshot peers advertise
     * the full chain tip yet lack pre-snapshot bodies — and is excluded from
     * subsequent getdata for those heights, so a from-genesis catch-up no
     * longer wedges when the peer set includes such peers.
     */
    void OnBlockNotFound(const uint256& hash, const std::string& peer_key);

    /**
     * Peers excluded from the *current* in-flight getdata (body-incapable for
     * that block's height). DispatchDeferredSends() sets this immediately
     * before invoking the send_getdata callback; the callback reads it
     * synchronously on the same thread. thread_local (issue #241/#214: sends
     * are dispatched outside mutex_, so concurrent dispatching threads each
     * carry their own handoff slot). Only meaningful inside the callback.
     */
    const std::unordered_set<std::string>& CurrentRequestSkipPeers() const {
        return current_request_skip_peers_;
    }

    /**
     * Called when a block is received from a peer.
     * Validates block against header and stores if valid.
     *
     * @param block The received block
     * @return true if block is valid and stored
     */
    bool OnBlockReceived(const Block& block);

    /**
     * Main tick - drives download scheduler.
     * Should be called periodically (e.g., every second).
     *
     * Decides when to request the next block.
     */
    void Tick();

    /**
     * FIX 2 (issue #186): central block-download deferral. If set, Tick() calls
     * this predicate and requests NO new blocks while it returns true —
     * regardless of which call site invoked Tick(). This is the single choke
     * point that lets a pending AssumeUTXO snapshot bootstrap keep the UTXO set
     * empty until the snapshot loads. The predicate must be cheap + non-blocking
     * (it runs under the scheduler mutex); wire it to
     * ChainstateService::IsSnapshotBootstrapPending().
     */
    void SetDeferCheck(std::function<bool()> defer_check) {
        defer_check_ = std::move(defer_check);
    }

    // ========================================================================
    // Status Queries
    // ========================================================================

    /**
     * Check if all blocks for current header chain are downloaded.
     */
    bool IsFullySynchronized() const;

    /**
     * Check if a block hash is in the scheduler queue (missing/requested/received).
     * This spans the full queued range and is broader than in-flight.
     */
    bool IsBlockExpected(const uint256& hash) const;

    /**
     * Check if a block hash was explicitly requested and is currently in-flight.
     * Use this for strict unsolicited-block gating during IBD.
     */
    bool IsBlockInFlight(const uint256& hash) const;

    /**
     * Check if a block is either in-flight or expected (atomic query).
     * Thread-safe: holds mutex for the entire check, avoiding TOCTOU races
     * when ScanForMissingBlocks clears and rebuilds in_flight_blocks_.
     */
    bool IsBlockKnown(const uint256& hash) const;

    /**
     * Check if a block was received and stored by this scheduler.
     */
    bool HasReceivedBlock(const uint256& hash) const;

    /**
     * Get number of blocks still missing.
     */
    size_t GetMissingBlockCount() const;

    /**
     * Get number of blocks currently in flight.
     */
    size_t GetInFlightCount() const;

    /**
     * Get number of queued blocks not yet marked as received.
     * Includes both MISSING and REQUESTED states.
     */
    size_t GetQueuedBlockCount() const;

    /**
     * Get maximum window size (for buffer capacity checks).
     */
    uint32_t GetMaxInFlight() const { return max_in_flight_; }

    /**
     * True once the P2P send callback is wired and the scheduler can issue
     * real getdata requests instead of queueing inert in-flight entries.
     */
    bool HasSendGetDataCallback() const { return static_cast<bool>(send_getdata_callback_); }

    /**
     * Re-request a block that failed validation.
     * Resets the block's status from REQUESTED/RECEIVED back to MISSING
     * so the next Tick() will request it again.
     *
     * @param block_hash Hash of the block to re-request
     * @return true if the block was found and reset
     */
    bool ReRequestBlock(const uint256& block_hash);

    /**
     * Mark a queued block permanently invalid.
     * Clears any in-flight/received bookkeeping so Tick() stops re-requesting it.
     *
     * @param block_hash Hash of the block to mark invalid
     * @return true if the block was found and marked invalid
     */
    bool MarkBlockInvalid(const uint256& block_hash);

    /**
     * Mark a queued block as consumed by the ordered validation path.
     *
     * In stateless CSN mode, proof validation happens outside the scheduler,
     * so a block can be fully validated before the active chain switches to it.
     * Marking it CONNECTED here prevents duplicate frontier retries and allows
     * the scheduler to advance to the next competing-branch block.
     *
     * @param block_hash Hash of the block to mark connected/consumed
     * @return true if the block was found and updated
     */
    bool MarkBlockConnected(const uint256& block_hash);

    /**
     * Check whether a queued block has already been marked CONNECTED.
     */
    bool IsBlockConnected(const uint256& hash) const;

    // ========================================================================
    // Phase W.2.6 Enhancement #3: Sync Phase Detection
    // ========================================================================

    /**
     * Get current sync phase (Phase W.2.6 RPC integration)
     *
     * Determines sync phase based on download state:
     * - IBD: Significant number of blocks missing (> 5% of chain)
     * - CATCHING_UP: Some blocks missing (1-5% of chain)
     * - STEADY_STATE: Fully synchronized (no missing blocks)
     *
     * @return Current sync phase
     */
    dinero::SyncPhase GetCurrentPhase() const;

    // ========================================================================
    // Callbacks (P2P Layer Registers These)
    // ========================================================================

    /**
     * Callback type for sending getdata message.
     *
     * Parameters:
     *   - block_hash: Hash of block to request
     *   - block_height: header-chain height of the block. The wiring layer uses
     *     it to send the getdata only to peers whose advertised height covers
     *     the block. Peers that can't have it would otherwise reply NOTFOUND and
     *     cancel the in-flight request (see OnBlockNotFound), which is what
     *     stalls a far-behind node whose peer set includes other lagging peers.
     */
    using SendGetDataCallback = std::function<void(const uint256& block_hash, uint32_t block_height)>;

    /**
     * Callback type for disconnecting peer (on invalid block).
     *
     * Parameters:
     *   - peer_id: Peer that sent invalid block
     *   - reason: Why we're disconnecting
     */
    using DisconnectPeerCallback = std::function<void(uint64_t peer_id, const std::string& reason)>;

    /**
     * Set callback for sending getdata.
     */
    void SetSendGetDataCallback(SendGetDataCallback callback) {
        send_getdata_callback_ = callback;
    }

    /**
     * Callback type for connecting a stored block to chainstate.
     *
     * Parameters:
     *   - block: The block to connect
     *   - source: Source identifier for logging
     * Returns: classified result used by the drain policy
     */
    using ConnectBlockCallback = std::function<ConnectBlockResult(const Block& block, const std::string& source)>;

    /**
     * Callback type for querying the actual chainstate tip height.
     * Used by TryConnectStoredBlocks to enforce strict tip+1 ordering.
     */
    using GetTipHeightCallback = std::function<uint32_t()>;

    /**
     * Callback type for querying the chainstate block hash at a given height.
     * Used by ScanForMissingBlocks to detect chain forks and find the
     * divergence point so that reorg blocks are correctly queued.
     *
     * @param height  Block height to query
     * @param out_hash  Output: block hash at that height in the active chain
     * @return true if the chainstate has a block at that height
     */
    using GetBlockHashAtHeightCallback = std::function<bool(uint32_t height, uint256& out_hash)>;

    /**
     * Callback type for external backpressure (e.g., CSN pending reorder buffer).
     *
     * Return value should represent the number of additional outstanding blocks
     * currently held outside scheduler in-flight tracking. Tick() will account
     * for this when deciding whether to request more blocks.
     */
    using ExternalBackpressureCallback = std::function<size_t()>;

    /**
     * Set callback for connecting stored blocks to chainstate.
     * Called by TryConnectStoredBlocks() for each block in height order.
     */
    void SetConnectBlockCallback(ConnectBlockCallback callback) {
        connect_block_callback_ = callback;
    }

    /**
     * Set callback for querying the actual chainstate tip height.
     */
    void SetGetTipHeightCallback(GetTipHeightCallback callback) {
        get_tip_height_callback_ = callback;
    }

    /**
     * Set callback for querying the chainstate block hash at a given height.
     */
    void SetGetBlockHashAtHeightCallback(GetBlockHashAtHeightCallback callback) {
        get_block_hash_at_height_callback_ = callback;
    }

    /**
     * Set callback for external backpressure accounting.
     * Pass empty/null callback to disable.
     */
    void SetExternalBackpressureCallback(ExternalBackpressureCallback callback) {
        external_backpressure_callback_ = callback;
    }

    /**
     * Enable/disable stateless CSN mode.
     * In stateless mode, ordered proof validation drives activation and the
     * scheduler must not connect flat-file blocks directly.
     */
    void SetStatelessMode(bool enabled) {
        stateless_mode_ = enabled;
    }

    /**
     * Try to connect stored blocks to chainstate in strict height order.
     *
     * Queries actual chainstate tip, then only connects block at tip+1.
     * If missing_blocks_ has a gap (doesn't cover tip+1), rescans the
     * header chain from the actual tip to fill the gap.
     *
     * On connection failure (missing parent), requests the PARENT hash
     * via SendGetData — never re-requests the same child block.
     *
     * @param max_blocks Maximum blocks to connect per call (0 = unlimited)
     * @return Number of blocks successfully connected
     */
    size_t TryConnectStoredBlocks(size_t max_blocks = 32);

    /**
     * Set callback for disconnecting peer.
     */
    void SetDisconnectPeerCallback(DisconnectPeerCallback callback) {
        disconnect_peer_callback_ = callback;
    }

    /**
     * Get the expected block hash at a given height from the header chain.
     * Used by reorder buffer to reject poisoned blocks.
     *
     * @param height Block height to query
     * @param out_hash Output: expected hash at that height
     * @return true if header exists at that height
     */
    bool GetExpectedHashAtHeight(uint32_t height, uint256& out_hash) const;

    /**
     * Set the local chainstate tip height.
     * Blocks at or below this height are considered already synced.
     * @param height The current local tip height
     */
    void SetLocalTipHeight(uint32_t height) {
        local_tip_height_ = height;
    }

    // issue #216: override the stale-request timeout. Primarily a test seam (the
    // default 30s can't be exercised deterministically otherwise); also usable
    // to tune re-request aggressiveness.
    void SetStaleRequestTimeoutSeconds(uint32_t secs) {
        std::lock_guard<std::mutex> lock(mutex_);
        stale_request_timeout_seconds_ = secs;
    }

    // ========================================================================
    // AssumeUTXO pre-base body backfill (spec Release Gate item 2)
    // ========================================================================
    //
    // Separate low-priority queue: never touches missing_blocks_ or
    // IsFullySynchronized (fast-bootstrap nodes stay "synced" while history
    // backfills). Serviced by Tick() only when tip sync has no pending work
    // (Task 2). Queue population + accounting live here (Task 1).

    /** Progress snapshot returned by GetBackfillProgress(). */
    struct BackfillProgress {
        bool enabled = false;
        uint32_t start_height = 0;
        uint32_t end_height = 0;
        uint64_t total = 0;        ///< bodies missing at Enable time
        uint64_t completed = 0;    ///< bodies received+stored since Enable
        uint64_t in_flight = 0;    ///< bodies currently in-flight
    };

    /**
     * Register a predicate that answers "does this node already have the body
     * for (hash, height)?" — used by EnableBackfill to skip existing bodies.
     * The callback is invoked under mutex_ during EnableBackfill only.
     */
    void SetHasBlockBodyCallback(std::function<bool(const uint256&, uint32_t)> cb);

    /**
     * Queue canonical heights [start_height, end_height] whose bodies are
     * missing, for low-priority backfill.  Idempotent: re-calling with the
     * same range is a no-op (progress is preserved).
     *
     * Population uses a single backward walk over parent pointers rather than
     * per-height GetHeaderAtHeight() calls (avoids O(n²) on a 33k-range).
     */
    void EnableBackfill(uint32_t start_height, uint32_t end_height);

    /**
     * Clear the backfill queue and reset all backfill accounting.
     * In-flight entries are removed from in_flight_blocks_ so the main
     * window accounting stays correct (no orphaned in-flight counts).
     */
    void DisableBackfill();

    /** Return a snapshot of current backfill progress (thread-safe). */
    BackfillProgress GetBackfillProgress() const;

    /**
     * Get the local chainstate tip height.
     */
    uint32_t GetLocalTipHeight() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return local_tip_height_;
    }

private:
    struct StatelessFrontier {
        size_t idx;
        bool reorg_barrier;
    };

    // Header chain to observe
    HeaderChainSelector* header_chain_;

    // Block storage for persisting downloaded blocks (Phase N.4.4)
    dinero::BlockStorage* block_storage_;

    // Queue of blocks to fetch (ordered by height)
    std::vector<BlockFetchState> missing_blocks_;

    // Currently in-flight blocks (windowed download)
    std::unordered_set<uint256> in_flight_blocks_;

    // O(1) lookup for expected blocks (mirrors missing_blocks_ hashes)
    std::unordered_set<uint256> expected_blocks_;

    // Maximum blocks in flight simultaneously (window size)
    uint32_t max_in_flight_ = 16;

    // Cursor: index into missing_blocks_ for next MISSING scan (amortized O(n))
    size_t next_missing_idx_ = 0;

    // Local chainstate tip height (blocks at or below are already synced).
    // Mutable: IsFullySynchronized() refreshes from callback to prevent stale-tip stalls.
    mutable uint32_t local_tip_height_ = 0;

    // True after OnHeadersProcessed() has been called at least once.
    // Before this, the scheduler hasn't populated missing_blocks_ yet,
    // so empty missing_blocks_ does NOT mean "synced".
    std::atomic<bool> headers_processed_{false};

    // FIX 2 (issue #186): when set + returns true, Tick() requests no new blocks
    // (central deferral for a pending AssumeUTXO snapshot bootstrap; keeps the
    // UTXO set empty so the snapshot can load).
    std::function<bool()> defer_check_;

    // CSN/stateless mode uses the ordered OnUtxoBlock path for activation.
    bool stateless_mode_ = false;

    // Guards all mutable scheduler state (missing_blocks_, in_flight_blocks_,
    // expected_blocks_, etc.) against concurrent access from P2P handler
    // threads calling OnHeadersProcessed/OnBlockReceived/Tick simultaneously.
    mutable std::mutex mutex_;

    // Blocks that have been received and stored (for parent-known checks).
    // Allows BlockAcceptor to distinguish "parent received but not connected"
    // from "parent truly unknown".
    std::unordered_set<uint256> received_blocks_;

    // P2P callbacks
    SendGetDataCallback send_getdata_callback_;
    DisconnectPeerCallback disconnect_peer_callback_;
    ConnectBlockCallback connect_block_callback_;
    GetTipHeightCallback get_tip_height_callback_;
    GetBlockHashAtHeightCallback get_block_hash_at_height_callback_;
    ExternalBackpressureCallback external_backpressure_callback_;

    // Parent request throttling to avoid spamming repeated getdata for the same
    // missing parent while waiting for network delivery.
    std::unordered_map<uint256, std::chrono::steady_clock::time_point> parent_request_times_;
    static constexpr uint32_t parent_request_cooldown_seconds_ = 2;

    // Stale in-flight timeout: if a REQUESTED block hasn't been received after
    // this many seconds, reset it to MISSING so the next Tick() re-requests it
    // (potentially to different/newer peers that actually have the block).
    // Non-const so tests can force the timeout to fire (see SetStaleRequestTimeoutSeconds).
    uint32_t stale_request_timeout_seconds_ = 30;

    // NOTFOUND rescan rate-limit: at most one RescanFromActualTip per this
    // many seconds when NOTFOUND responses arrive in bursts.
    std::chrono::steady_clock::time_point last_notfound_rescan_{};
    static constexpr uint32_t notfound_rescan_cooldown_seconds_ = 5;

    // Per-peer body-availability hint (issue #241): peer_key -> highest block
    // height that peer returned NOTFOUND for. The peer is treated as lacking
    // bodies at all heights <= this value (covers AssumeUTXO snapshot peers that
    // advertise the full tip but lack pre-snapshot bodies) and is excluded from
    // getdata for those heights. Guarded by mutex_.
    std::unordered_map<std::string, uint32_t> peer_lacks_body_at_or_below_;

    // Skip-set for the current in-flight getdata, set by DispatchDeferredSends
    // immediately before each send_getdata callback and read synchronously by
    // that callback on the same thread (see CurrentRequestSkipPeers).
    // thread_local because sends are dispatched OUTSIDE mutex_ (issue
    // #241/#214) and concurrent Tick() callers must not race on it.
    static thread_local std::unordered_set<std::string> current_request_skip_peers_;

    // issue #241/#214: a getdata staged under mutex_ for dispatch after the
    // lock is released. Invoking the send callback under mutex_ let one
    // blocked peer-socket send() wedge every scheduler entry point (peer
    // handler threads via OnNewBlock -> Tick, the tick loop, status probes)
    // — the silent-stall / frozen-tip signature. The skip-set is snapshotted
    // at staging time so the daemon wiring sees the same exclusions it would
    // have seen under the lock.
    struct DeferredGetdata {
        uint256 block_hash;
        uint32_t height = 0;
        std::unordered_set<std::string> skip_peers;
    };
    std::vector<DeferredGetdata> deferred_sends_;  // guarded by mutex_

    // ── AssumeUTXO backfill private state ────────────────────────────────────
    // Backfill queue entries reuse BlockFetchState (hash, height, status,
    // request_time) but live in their own vector with their own cursor so they
    // never pollute missing_blocks_, in_flight_blocks_, or expected_blocks_.
    std::vector<BlockFetchState> backfill_blocks_;
    size_t next_backfill_idx_ = 0;
    std::unordered_set<uint256> backfill_expected_;  // routing in OnBlockReceived (Task 2)
    BackfillProgress backfill_progress_;
    std::function<bool(const uint256&, uint32_t)> has_block_body_;

    // ========================================================================
    // Private Helpers
    // ========================================================================

    // Stage a getdata for block_hash/block_height into deferred_sends_ with
    // its skip-set snapshot (from peer_lacks_body_at_or_below_). Caller MUST
    // hold mutex_. The actual send happens in DispatchDeferredSends().
    void StageGetdataLocked(const uint256& block_hash, uint32_t block_height);

    // Drain deferred_sends_ (briefly re-acquiring mutex_ to swap it out) and
    // invoke send_getdata_callback_ for each entry WITHOUT holding mutex_.
    // Caller MUST NOT hold mutex_.
    void DispatchDeferredSends();

    // Tick() body. Caller MUST hold mutex_. Network sends are staged via
    // StageGetdataLocked and dispatched by Tick() after the lock is released.
    void TickLocked();

    /**
     * Scan header chain for missing blocks.
     * Updates missing_blocks_ queue.
     */
    void ScanForMissingBlocks();

    /**
     * Request next block from queue.
     * @return true if a block was requested, false if no MISSING blocks remain
     */
    bool RequestNextBlock();

    /**
     * Validate block against header.
     *
     * @param block Block to validate
     * @return true if block matches header
     */
    bool ValidateBlockAgainstHeader(const Block& block);

    /**
     * Store block to disk.
     * Does NOT activate chainstate.
     *
     * @param block Block to store
     * @param out_pos Output: file position where block was stored
     * @return true if stored successfully
     */
    bool StoreBlock(const Block& block, FilePosition& out_pos);

    /**
     * Rescan header chain from the actual chainstate tip.
     * Preserves RECEIVED/CONNECTED status for already-downloaded blocks.
     * Called when the drain detects a gap between chainstate tip and
     * the first block in missing_blocks_.
     */
    void RescanFromActualTip(uint32_t actual_tip);

    /**
     * In stateless mode, find the earliest queued block that must be
     * validated against the current active-chain pre-state before any later
     * descendants can safely be requested.
     *
     * Blocks below or at the current tip that already match the active chain
     * are auto-marked CONNECTED and skipped. If the first unresolved block is
     * at or below the tip, it becomes a reorg barrier and descendants must
     * wait for it to activate before more requests are issued.
     */
    std::optional<StatelessFrontier> FindStatelessFrontierLocked(uint32_t actual_tip);

    // Non-locking helpers for use within methods that already hold mutex_.
    // The public IsBlockInFlight/IsBlockExpected/etc. acquire mutex_ and
    // must NOT be called from Tick() or TryConnectStoredBlocks() which
    // already hold it (std::mutex is non-recursive → deadlock).
    bool isBlockInFlightLocked(const uint256& hash) const {
        return in_flight_blocks_.count(hash) > 0;
    }
    bool isBlockExpectedLocked(const uint256& hash) const {
        return expected_blocks_.count(hash) > 0;
    }
};

} // namespace consensus
} // namespace dinero
