/**
 * Phase N.4: Blocks-after-Headers Download Scheduler Implementation
 */

#include "consensus/block_download_scheduler.h"
#include "consensus/header_chain.h"
#include "storage/block_storage.h"
#include "common/logger.h"
#include <algorithm>

namespace dinero {
namespace consensus {

// ============================================================================
// Constructor / Destructor
// ============================================================================

BlockDownloadScheduler::BlockDownloadScheduler(HeaderChainSelector* header_chain,
                                               dinero::BlockStorage* block_storage)
    : header_chain_(header_chain)
    , block_storage_(block_storage) {

    if (!header_chain_) {
        g_logger.error("BlockDownloadScheduler: header_chain is null");
        return;
    }

    if (!block_storage_) {
        g_logger.warning("[BlockDownloadScheduler] block_storage is null - blocks will not be persisted");
    }

    g_logger.info("[BlockDownloadScheduler] Initialized");
}

BlockDownloadScheduler::~BlockDownloadScheduler() {
    g_logger.info("[BlockDownloadScheduler] Shutdown");
}

// ============================================================================
// Core Methods
// ============================================================================

void BlockDownloadScheduler::OnHeadersProcessed() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Mark headers as processed (unblocks the IBD guard in OnInv).
    // No call-once guard: ScanForMissingBlocks must re-run whenever new
    // headers arrive (e.g., peer A sends empty headers at same tip, then
    // peer B sends 291 real headers — the second call MUST populate
    // missing_blocks_ for the new headers).
    headers_processed_.store(true);
    g_logger.info("[BlockDownloadScheduler] OnHeadersProcessed called");

    // Scan header chain for missing blocks
    ScanForMissingBlocks();

    // Log status
    g_logger.info("[BlockDownloadScheduler] Missing blocks: " +
                 std::to_string(missing_blocks_.size()));
}

void BlockDownloadScheduler::OnBlockNotFound(const uint256& hash) {
    // Delegate without acquiring the lock here (the 2-arg form locks).
    OnBlockNotFound(hash, std::string());
}

void BlockDownloadScheduler::OnBlockNotFound(const uint256& hash,
                                             const std::string& peer_key) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Clear from in-flight so Tick() doesn't count this toward the window
    // and so the stale-request timeout doesn't reset it to MISSING.
    in_flight_blocks_.erase(hash);

    g_logger.info("[BlockDownloadScheduler] NOTFOUND for " +
                 hash.GetHex().substr(0, 16) + "..." +
                 (peer_key.empty() ? std::string() : (" from " + peer_key)));

    // issue #241: record that this peer lacks the body for this block's height.
    // A snapshot-bootstrapped peer advertises the full tip but lacks pre-snapshot
    // bodies; one NOTFOUND marks it body-incapable at that height and below, so
    // subsequent getdata for those heights skip it instead of re-poisoning the
    // in-flight request (which was the catch-up wedge).
    if (!peer_key.empty()) {
        for (const auto& fs : missing_blocks_) {
            if (fs.block_hash == hash) {
                uint32_t& gap = peer_lacks_body_at_or_below_[peer_key];
                if (fs.height > gap) gap = fs.height;
                break;
            }
        }
    }

    // Rate-limited rescan: rebuild missing_blocks_ from the current header
    // chain.  Stale-branch hashes won't appear in the rebuilt list, so they
    // are permanently dropped.  If the hash IS still on the header chain
    // (peer was just behind), it reappears as MISSING and gets re-requested.
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_notfound_rescan_).count();
    if (elapsed >= notfound_rescan_cooldown_seconds_ && get_tip_height_callback_) {
        uint32_t actual_tip = get_tip_height_callback_();
        g_logger.info("[BlockDownloadScheduler] NOTFOUND rescan from tip " +
                     std::to_string(actual_tip));
        RescanFromActualTip(actual_tip);
        last_notfound_rescan_ = now;
    }
}

// issue #241/#214: per-thread handoff slot for the skip-set of the getdata
// currently being dispatched (read by the daemon send callback on the same
// thread). thread_local because sends are dispatched outside mutex_.
thread_local std::unordered_set<std::string>
    BlockDownloadScheduler::current_request_skip_peers_;

// issue #241: stage a getdata with its skip-set snapshot — every peer known
// to lack bodies at or above this height. Caller MUST hold mutex_. The send
// itself happens in DispatchDeferredSends(), after the lock is released
// (issue #241/#214: a blocking peer-socket send under mutex_ wedged every
// scheduler entry point — peer handler threads, the tick loop — freezing
// block ingest while the process looked healthy).
void BlockDownloadScheduler::StageGetdataLocked(const uint256& block_hash,
                                                uint32_t block_height) {
    DeferredGetdata deferred;
    deferred.block_hash = block_hash;
    deferred.height = block_height;
    if (block_height > 0) {
        for (const auto& kv : peer_lacks_body_at_or_below_) {
            if (kv.second >= block_height) {
                deferred.skip_peers.insert(kv.first);
            }
        }
    }
    deferred_sends_.push_back(std::move(deferred));
}

// issue #241/#214: drain staged getdata sends and invoke the network callback
// WITHOUT holding mutex_, so a peer socket that stops draining (full send
// buffer, half-dead connection) can only block this one dispatching thread —
// never the scheduler itself. Caller MUST NOT hold mutex_.
void BlockDownloadScheduler::DispatchDeferredSends() {
    std::vector<DeferredGetdata> sends;
    SendGetDataCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sends.swap(deferred_sends_);
        callback = send_getdata_callback_;
    }
    if (!callback) {
        return;
    }
    for (auto& deferred : sends) {
        // Same-thread handoff to the callback (see CurrentRequestSkipPeers).
        current_request_skip_peers_ = std::move(deferred.skip_peers);
        callback(deferred.block_hash, deferred.height);
    }
    current_request_skip_peers_.clear();
}

bool BlockDownloadScheduler::OnBlockReceived(const Block& block) {
    std::lock_guard<std::mutex> lock(mutex_);
    g_logger.info("[BlockDownloadScheduler] OnBlockReceived: " + block.GetHash().GetHex());

    // Validate block against header
    if (!ValidateBlockAgainstHeader(block)) {
        g_logger.warning("[BlockDownloadScheduler] Block validation failed: " +
                        block.GetHash().GetHex());
        return false;
    }

    // Store block (but do NOT activate chainstate)
    FilePosition stored_pos;
    if (!StoreBlock(block, stored_pos)) {
        g_logger.error("[BlockDownloadScheduler] Failed to store block: " +
                      block.GetHash().GetHex());
        return false;
    }

    // Mark block as received and record its storage position
    uint256 block_hash = block.GetHash();
    for (auto& fetch_state : missing_blocks_) {
        if (fetch_state.block_hash == block_hash) {
            fetch_state.status = FetchStatus::RECEIVED;
            fetch_state.stored_pos = stored_pos;
            g_logger.info("[BlockDownloadScheduler] Block marked RECEIVED: " +
                         block_hash.GetHex());
            break;
        }
    }

    // Remove from in-flight and expected sets
    in_flight_blocks_.erase(block_hash);
    expected_blocks_.erase(block_hash);

    return true;
}

void BlockDownloadScheduler::Tick() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        TickLocked();
    }
    // issue #241/#214: getdata sends staged by TickLocked() go out here, after
    // mutex_ release. A peer socket that stops draining (blocking send) can
    // then only park THIS caller thread — other peer handler threads, the
    // tick loop, and status probes still enter the scheduler freely.
    DispatchDeferredSends();
}

void BlockDownloadScheduler::TickLocked() {
    // FIX 2 (issue #186): central deferral. While a snapshot bootstrap is
    // pending, request no new blocks regardless of which call site invoked
    // Tick() — this keeps the UTXO set empty so the snapshot can load. The
    // predicate resolves to false once the snapshot loads or the bootstrap is
    // abandoned (→ full IBD resumes).
    if (defer_check_ && defer_check_()) {
        return;
    }

    // Restart bootstrap: HeaderChainSelector may already contain a persisted
    // best chain from HeaderStore before any fresh headers message arrives.
    // If we only ever set headers_processed_ from OnHeadersProcessed(), the
    // scheduler can remain permanently inert after restart with a preloaded
    // header backlog. Prime the queue from the existing selector view here.
    if (!headers_processed_.load() && header_chain_) {
        if (const HeaderIndexEntry* best = header_chain_->GetBestHeader()) {
            headers_processed_.store(true);
            g_logger.info("[BlockDownloadScheduler] Bootstrap scan from persisted headers: best=" +
                          std::to_string(best->height) + " local=" +
                          std::to_string(local_tip_height_));
            ScanForMissingBlocks();
            g_logger.info("[BlockDownloadScheduler] Missing blocks: " +
                          std::to_string(missing_blocks_.size()));
        }
    }

    auto request_stateless_frontier = [&](size_t gap_idx,
                                          std::chrono::steady_clock::time_point request_now) -> bool {
        if (gap_idx >= missing_blocks_.size()) {
            return false;
        }
        auto& gap_state = missing_blocks_[gap_idx];
        if (gap_state.status != FetchStatus::MISSING) {
            return false;
        }

        gap_state.status = FetchStatus::REQUESTED;
        gap_state.request_time = request_now;
        in_flight_blocks_.insert(gap_state.block_hash);
        next_missing_idx_ = (gap_idx + 1) % missing_blocks_.size();

        if (send_getdata_callback_) {
            StageGetdataLocked(gap_state.block_hash, gap_state.height);  // #241/#214
            g_logger.info("[BlockDownloadScheduler] Requested stateless frontier block: " +
                          gap_state.block_hash.GetHex() +
                          " (height " + std::to_string(gap_state.height) + ")");
        } else {
            g_logger.warning("[BlockDownloadScheduler] send_getdata_callback not set");
        }

        return true;
    };

    // Expire stale in-flight requests: if a block has been REQUESTED for longer
    // than the timeout, reset it to MISSING so it gets re-requested (potentially
    // to different peers that actually have the block).
    const auto now = std::chrono::steady_clock::now();
    for (auto& fetch_state : missing_blocks_) {
        if (fetch_state.status == FetchStatus::REQUESTED) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - fetch_state.request_time).count();
            if (elapsed >= stale_request_timeout_seconds_) {
                // issue #216: before re-requesting, check we don't ALREADY have
                // the block. A block we requested can be received AND connected
                // via a parallel path (BlockRelayManager -> chainstate) without
                // OnBlockReceived() ever marking it RECEIVED here — e.g. the
                // block's bytes arrived after this stale timeout had already
                // flipped it out of in_flight_blocks_, so the OnNewBlock router
                // took the relay path and skipped our OnBlockReceived(); or its
                // redundant StoreBlock raced the relay path. Left unchecked, the
                // block sits REQUESTED below our own advancing tip and we
                // re-request it every timeout — the catch-up re-request
                // amplification (observed ~7x on a slow node).
                //
                // HASH-PRECISE check: a bare "height <= local_tip_height_" is NOT
                // safe — on a reorg, ScanForMissingBlocks queues new-fork blocks
                // below the old tip that we genuinely don't have. We only "have
                // it" if we recorded the receipt, OR the ACTIVE chain holds THIS
                // exact block hash at its height (a different hash on a fork →
                // re-request, which is correct).
                bool already_have =
                    received_blocks_.count(fetch_state.block_hash) > 0;
                if (!already_have && fetch_state.height <= local_tip_height_ &&
                    get_block_hash_at_height_callback_) {
                    uint256 chain_hash;
                    if (get_block_hash_at_height_callback_(fetch_state.height, chain_hash) &&
                        chain_hash == fetch_state.block_hash) {
                        already_have = true;
                    }
                }
                if (already_have) {
                    g_logger.info("[BlockDownloadScheduler] Stale REQUESTED block already present "
                                  "(height " + std::to_string(fetch_state.height) + " <= tip " +
                                  std::to_string(local_tip_height_) + ") — marking RECEIVED, not "
                                  "re-requesting: " + fetch_state.block_hash.GetHex().substr(0, 16) + "...");
                    fetch_state.status = FetchStatus::RECEIVED;
                    in_flight_blocks_.erase(fetch_state.block_hash);
                    received_blocks_.insert(fetch_state.block_hash);
                    continue;
                }
                g_logger.info("[BlockDownloadScheduler] Stale request expired: " +
                             fetch_state.block_hash.GetHex().substr(0, 16) +
                             "... (height " + std::to_string(fetch_state.height) +
                             ", waited " + std::to_string(elapsed) + "s)");
                fetch_state.status = FetchStatus::MISSING;
                in_flight_blocks_.erase(fetch_state.block_hash);
            }
        }
    }

    std::optional<size_t> stateless_gap_idx;
    bool stateless_reorg_barrier = false;
    // In stateless CSN mode, proof validation is strictly ordered outside the
    // scheduler. The flat-file drainer never runs there, so it cannot repair
    // drift back to the true frontier gap. Treat the active-chain tip as the
    // only source of truth and keep retrying that frontier block until the tip
    // advances.
    if (stateless_mode_ && get_tip_height_callback_ && !missing_blocks_.empty()) {
        const uint32_t actual_tip = get_tip_height_callback_();
        if (local_tip_height_ != actual_tip) {
            g_logger.info("[BlockDownloadScheduler] Stateless active-tip sync: local=" +
                          std::to_string(local_tip_height_) + " -> active=" +
                          std::to_string(actual_tip));
            local_tip_height_ = actual_tip;
        }

        if (auto frontier = FindStatelessFrontierLocked(actual_tip)) {
            auto& gap_state = missing_blocks_[frontier->idx];
            stateless_gap_idx = frontier->idx;
            stateless_reorg_barrier = frontier->reorg_barrier;
            const uint32_t want = gap_state.height;
            const bool gap_in_flight = in_flight_blocks_.count(gap_state.block_hash) > 0;
            if (gap_state.status == FetchStatus::INVALID) {
                g_logger.error("[BlockDownloadScheduler] Stateless frontier is INVALID at height " +
                               std::to_string(want) + " hash=" +
                               gap_state.block_hash.GetHex().substr(0, 16) +
                               "... — halting further block requests");
                return;
            }
            bool retry_gap = false;
            std::string retry_reason;
            if (gap_state.status == FetchStatus::REQUESTED && !gap_in_flight) {
                retry_gap = true;
                retry_reason = "request lost";
            } else if (gap_state.status == FetchStatus::RECEIVED) {
                retry_gap = true;
                retry_reason = "received but active tip still behind";
            } else if (gap_state.status == FetchStatus::CONNECTED) {
                bool active_chain_matches = false;
                if (get_block_hash_at_height_callback_ && want > 0) {
                    uint256 chain_hash;
                    if (get_block_hash_at_height_callback_(want, chain_hash) &&
                        chain_hash == gap_state.block_hash) {
                        active_chain_matches = true;
                    }
                }
                if (!active_chain_matches) {
                    retry_gap = true;
                    retry_reason = "connected state ahead of active chain";
                }
            }

            if (retry_gap) {
                g_logger.warning("[BlockDownloadScheduler] Stateless frontier retry at height " +
                                 std::to_string(want) + " (" + retry_reason + ")");
                gap_state.status = FetchStatus::MISSING;
                gap_state.stored_pos = FilePosition();
                in_flight_blocks_.erase(gap_state.block_hash);
                received_blocks_.erase(gap_state.block_hash);
            }

            if (next_missing_idx_ != frontier->idx) {
                g_logger.info("[BlockDownloadScheduler] Stateless gap: resetting cursor from " +
                              std::to_string(next_missing_idx_) + " to " +
                              std::to_string(frontier->idx) + " (height " +
                              std::to_string(want) + ")");
                next_missing_idx_ = frontier->idx;
            }
        }
    }

    // Request blocks until window is full or no more MISSING blocks.
    // External backpressure (e.g., CSN pending reorder buffer) is included so
    // all Tick() call sites respect the same effective outstanding window.
    while (true) {
        size_t external_backpressure = 0;
        if (external_backpressure_callback_) {
            external_backpressure = external_backpressure_callback_();
        }

        const size_t max_window = static_cast<size_t>(max_in_flight_);
        if (stateless_mode_ && stateless_gap_idx.has_value() && max_window > 0 &&
            external_backpressure >= max_window) {
            request_stateless_frontier(*stateless_gap_idx, now);
            break;
        }

        // Liveness guard: keep one scheduler request slot available even when
        // external backpressure is saturated, so gap blocks can still be fetched.
        const size_t clamped_backpressure =
            (max_window > 0) ? std::min(external_backpressure, max_window - 1) : 0;

        if (stateless_mode_ && stateless_gap_idx.has_value() && max_window > 0) {
            auto& gap_state = missing_blocks_[*stateless_gap_idx];
            if (gap_state.status == FetchStatus::MISSING &&
                in_flight_blocks_.size() + clamped_backpressure >= max_window) {
                auto reclaim_it = std::find_if(
                    missing_blocks_.rbegin(),
                    missing_blocks_.rend(),
                    [&](const BlockFetchState& fs) {
                        return fs.height > gap_state.height &&
                               fs.status == FetchStatus::REQUESTED &&
                               in_flight_blocks_.count(fs.block_hash) > 0;
                    });
                if (reclaim_it != missing_blocks_.rend()) {
                    g_logger.warning("[BlockDownloadScheduler] Reclaiming stateless in-flight slot at height " +
                                     std::to_string(reclaim_it->height) +
                                     " to prioritize frontier height " +
                                     std::to_string(gap_state.height));
                    in_flight_blocks_.erase(reclaim_it->block_hash);
                    reclaim_it->status = FetchStatus::MISSING;
                }

                if (in_flight_blocks_.size() + clamped_backpressure < max_window) {
                    request_stateless_frontier(*stateless_gap_idx, now);
                    break;
                }
            }

            // On a competing branch, descendants are only valid after the
            // replacement block at or below the current active tip becomes
            // active. Hold the frontier here until chainstate catches up.
            if (stateless_reorg_barrier) {
                if (gap_state.status == FetchStatus::MISSING &&
                    in_flight_blocks_.size() + clamped_backpressure < max_window) {
                    request_stateless_frontier(*stateless_gap_idx, now);
                }
                break;
            }
        }

        if (in_flight_blocks_.size() + clamped_backpressure >= max_window) {
            g_logger.debug("[Tick] Window full: inflight=" +
                           std::to_string(in_flight_blocks_.size()) +
                           " backpressure=" + std::to_string(external_backpressure) +
                           " clamped_backpressure=" + std::to_string(clamped_backpressure) +
                           " max=" + std::to_string(max_in_flight_));
            break;
        }
        if (!RequestNextBlock()) {
            // Count MISSING blocks to diagnose stall
            size_t missing_count = 0;
            for (const auto& fs : missing_blocks_) {
                if (fs.status == FetchStatus::MISSING) missing_count++;
            }
            g_logger.debug("[Tick] RequestNextBlock=false: missing_blocks=" +
                           std::to_string(missing_blocks_.size()) +
                           " MISSING_status=" + std::to_string(missing_count) +
                           " cursor=" + std::to_string(next_missing_idx_));
            break;
        }
    }

    // Try to connect stored blocks to chainstate in height order.
    // This is the primary block connection mechanism during IBD —
    // blocks are stored out-of-order by the scheduler, then connected
    // sequentially here as contiguous runs become available.
    TryConnectStoredBlocks();
}

// ============================================================================
// Status Queries
// ============================================================================

bool BlockDownloadScheduler::IsFullySynchronized() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Before OnHeadersProcessed() is called, we haven't even scanned — not synced.
    if (!headers_processed_) {
        return false;
    }

    // If blocks are queued, check if all are RECEIVED or CONNECTED
    if (!missing_blocks_.empty()) {
        for (const auto& fetch_state : missing_blocks_) {
            if (fetch_state.status != FetchStatus::RECEIVED &&
                fetch_state.status != FetchStatus::CONNECTED) {
                return false;
            }
        }
        return true;  // All queued blocks downloaded
    }

    // No blocks queued — check if headers indicate we need any.
    // Refresh local_tip_height_ from callback to prevent stale-tip false negatives.
    // Without this, blocks connected through the post-IBD ProcessIncomingBlock path
    // advance the chainstate tip but leave local_tip_height_ behind, causing this
    // gate to permanently return false and drop all subsequent INV blocks.
    if (get_tip_height_callback_) {
        uint32_t actual_tip = get_tip_height_callback_();
        if (actual_tip > local_tip_height_) {
            local_tip_height_ = actual_tip;
        }
    }
    if (header_chain_) {
        const HeaderIndexEntry* best = header_chain_->GetBestHeader();
        if (best && best->height > local_tip_height_) {
            return false;  // Headers ahead of our validated tip
        }
    }
    return true;
}

bool BlockDownloadScheduler::IsBlockExpected(const uint256& hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return expected_blocks_.count(hash) > 0;
}

bool BlockDownloadScheduler::IsBlockInFlight(const uint256& hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_blocks_.count(hash) > 0;
}

bool BlockDownloadScheduler::IsBlockKnown(const uint256& hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_blocks_.count(hash) > 0 || expected_blocks_.count(hash) > 0;
}

bool BlockDownloadScheduler::HasReceivedBlock(const uint256& hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_blocks_.count(hash) > 0;
}

bool BlockDownloadScheduler::IsBlockConnected(const uint256& hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& fetch_state : missing_blocks_) {
        if (fetch_state.block_hash == hash) {
            return fetch_state.status == FetchStatus::CONNECTED;
        }
    }
    return false;
}

size_t BlockDownloadScheduler::GetMissingBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = 0;
    for (const auto& fetch_state : missing_blocks_) {
        if (fetch_state.status == FetchStatus::MISSING) {
            count++;
        }
    }
    return count;
}

size_t BlockDownloadScheduler::GetInFlightCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_blocks_.size();
}

size_t BlockDownloadScheduler::GetQueuedBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return expected_blocks_.size();
}

// ============================================================================
// Phase W.2.6 Enhancement #3: Sync Phase Detection
// ============================================================================

dinero::SyncPhase BlockDownloadScheduler::GetCurrentPhase() const {
    // Fully synchronized - no missing blocks
    if (IsFullySynchronized()) {
        return dinero::SyncPhase::STEADY_STATE;
    }

    // Calculate percentage of missing blocks
    size_t total_blocks = missing_blocks_.size();
    size_t missing_count = GetMissingBlockCount();

    if (total_blocks == 0) {
        return dinero::SyncPhase::STEADY_STATE;
    }

    double missing_percentage = static_cast<double>(missing_count) / total_blocks;

    // IBD: More than 5% missing
    if (missing_percentage > 0.05) {
        return dinero::SyncPhase::IBD;
    }

    // CATCHING_UP: 1-5% missing
    if (missing_percentage > 0.01) {
        return dinero::SyncPhase::CATCHING_UP;
    }

    // Nearly synchronized
    return dinero::SyncPhase::STEADY_STATE;
}

// ============================================================================
// Private Helpers
// ============================================================================

// ============================================================================
// AssumeUTXO pre-base body backfill — queue population + accounting (Task 1)
// ============================================================================

void BlockDownloadScheduler::SetHasBlockBodyCallback(
        std::function<bool(const uint256&, uint32_t)> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    has_block_body_ = std::move(cb);
}

void BlockDownloadScheduler::EnableBackfill(uint32_t start_height,
                                             uint32_t end_height) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Idempotent: same range while already enabled → preserve in-progress state.
    if (backfill_progress_.enabled &&
        backfill_progress_.start_height == start_height &&
        backfill_progress_.end_height == end_height) {
        return;
    }

    // Full reset before (re-)populating.
    backfill_blocks_.clear();
    backfill_expected_.clear();
    next_backfill_idx_ = 0;
    backfill_progress_ = BackfillProgress{};
    backfill_progress_.enabled = true;
    backfill_progress_.start_height = start_height;
    backfill_progress_.end_height = end_height;

    if (!header_chain_ || start_height > end_height) {
        // No header chain yet, or degenerate range — queue stays empty.
        return;
    }

    // Single backward walk: get the anchor at end_height ONCE, then follow
    // parent pointers to start_height.  This is O(best_height - start_height)
    // rather than O((end-start)^2) from calling GetHeaderAtHeight(h) per
    // height (each call walks from best_header — the same O(n²) bug class
    // that commit 9bc061782 fixed in ScanForMissingBlocks on the main branch,
    // but which is NOT yet on this branch's base — so we must avoid it here).
    // Parent links are append-only/immutable once added; the same invariant
    // GetBestHeader()'s raw-pointer return already relies on.
    const HeaderIndexEntry* anchor = header_chain_->GetHeaderAtHeight(end_height);
    if (!anchor) {
        // Headers not yet available for the full range; queue stays empty.
        g_logger.warning("[BlockDownloadScheduler] EnableBackfill: no header at end_height=" +
                         std::to_string(end_height));
        return;
    }

    // Collect entries from end_height down to start_height (reverse order),
    // then process forward so backfill_blocks_ is in ascending-height order.
    std::vector<const HeaderIndexEntry*> window;
    window.reserve(anchor->height - start_height + 1);
    for (const HeaderIndexEntry* e = anchor; e && e->height >= start_height;
         e = e->parent) {
        window.push_back(e);
        if (e->height == start_height) break;  // guard: avoid underflow at h==0
    }

    for (auto rit = window.rbegin(); rit != window.rend(); ++rit) {
        const HeaderIndexEntry* entry = *rit;
        if (has_block_body_ && has_block_body_(entry->hash, entry->height)) {
            continue;  // body already present; skip
        }
        backfill_blocks_.emplace_back(entry->hash, entry->height);
        backfill_expected_.insert(entry->hash);
    }
    backfill_progress_.total = backfill_blocks_.size();

    g_logger.info("[BlockDownloadScheduler] EnableBackfill: range=" +
                  std::to_string(start_height) + ".." + std::to_string(end_height) +
                  " missing=" + std::to_string(backfill_progress_.total));
}

void BlockDownloadScheduler::DisableBackfill() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Release any in-flight accounting so the main window stays consistent.
    for (const auto& fs : backfill_blocks_) {
        in_flight_blocks_.erase(fs.block_hash);
    }
    backfill_blocks_.clear();
    backfill_expected_.clear();
    next_backfill_idx_ = 0;
    backfill_progress_ = BackfillProgress{};

    g_logger.info("[BlockDownloadScheduler] DisableBackfill");
}

BlockDownloadScheduler::BackfillProgress
BlockDownloadScheduler::GetBackfillProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backfill_progress_;
}

void BlockDownloadScheduler::ScanForMissingBlocks() {
    if (!header_chain_) {
        return;
    }

    // Refresh local_tip_height_ from the actual chainstate tip.
    // This is critical after local mining (generatetoaddress), which updates
    // the chainstate but does NOT call SetLocalTipHeight on the scheduler.
    // Without this, the scheduler would queue already-synced blocks as MISSING.
    if (get_tip_height_callback_) {
        uint32_t actual_tip = get_tip_height_callback_();
        if (actual_tip > local_tip_height_) {
            g_logger.info("[BlockDownloadScheduler] Refreshed local tip: " +
                         std::to_string(local_tip_height_) + " -> " +
                         std::to_string(actual_tip));
            local_tip_height_ = actual_tip;
        }
    }

    // Snapshot existing non-MISSING blocks so rescan preserves scheduler state.
    // This allows repeated OnHeadersProcessed calls (during multi-batch headers sync)
    // without dropping REQUESTED/RECEIVED/CONNECTED progress.
    std::unordered_map<uint256, BlockFetchState> preserved;
    for (const auto& fs : missing_blocks_) {
        if (fs.status != FetchStatus::MISSING) {
            preserved.emplace(fs.block_hash, fs);
        }
    }

    // Clear existing queue and reset cursors.
    // in_flight_blocks_ is rebuilt from preserved REQUESTED entries.
    missing_blocks_.clear();
    expected_blocks_.clear();
    in_flight_blocks_.clear();
    next_missing_idx_ = 0;

    // Get best header
    const HeaderIndexEntry* best_header = header_chain_->GetBestHeader();
    if (!best_header) {
        g_logger.info("[BlockDownloadScheduler] No headers available");
        return;
    }

    uint32_t best_height = best_header->height;
    g_logger.info("[BlockDownloadScheduler] Scanning for missing blocks: best header height = " +
                 std::to_string(best_height) + ", local tip height = " +
                 std::to_string(local_tip_height_));

    // Walk header chain from local_tip + 1 to best_header
    // Blocks at or below local_tip_height_ are already in chainstate
    uint32_t start_height = local_tip_height_ + 1;

    // Fork detection: if the best header chain diverges from our local chain,
    // we need to download blocks from the fork point, not from local_tip+1.
    // Without this, reorg blocks below our tip would never be queued.
    if (get_block_hash_at_height_callback_ && local_tip_height_ > 0) {
        const HeaderIndexEntry* tip_header = header_chain_->GetHeaderAtHeight(local_tip_height_);
        if (tip_header) {
            uint256 local_hash;
            if (get_block_hash_at_height_callback_(local_tip_height_, local_hash)) {
                if (local_hash != tip_header->hash) {
                    // Fork detected: best header chain differs from our active chain.
                    // Walk backwards to find the common ancestor (fork point).
                    uint32_t fork_height = local_tip_height_;
                    while (fork_height > 0) {
                        fork_height--;
                        const HeaderIndexEntry* entry = header_chain_->GetHeaderAtHeight(fork_height);
                        uint256 chain_hash;
                        if (entry && get_block_hash_at_height_callback_(fork_height, chain_hash)) {
                            if (chain_hash == entry->hash) {
                                break;  // Found the common ancestor
                            }
                        } else {
                            break;
                        }
                    }
                    g_logger.info("[BlockDownloadScheduler] Fork detected: local tip=" +
                                 std::to_string(local_tip_height_) +
                                 " fork point=" + std::to_string(fork_height) +
                                 " best header=" + std::to_string(best_height));
                    start_height = fork_height + 1;
                }
            }
        }
    }

    if (start_height > best_height) {
        g_logger.info("[BlockDownloadScheduler] Already synchronized (local tip >= header tip)");
        return;
    }

    // issue #241 perf: collect the [start_height, best_height] window with ONE
    // backward walk over parent pointers instead of GetHeaderAtHeight(h) per
    // height — each of those calls walks from the best header, making the scan
    // O(n^2). On a from-genesis sync (~39k headers) that pinned a core inside
    // this loop under mutex_ on EVERY headers message and throttled ingest to
    // a few blocks/min. Parent links are append-only/immutable once added, the
    // same invariant GetBestHeader()'s raw-pointer return already relies on.
    std::vector<const HeaderIndexEntry*> window;
    window.reserve(best_height - start_height + 1);
    for (const HeaderIndexEntry* e = best_header; e && e->height >= start_height;
         e = e->parent) {
        window.push_back(e);
        if (e->height == start_height) {
            break;  // height 0 entries make `e->height >= start_height` never false
        }
    }

    size_t preserved_count = 0;
    for (auto rit = window.rbegin(); rit != window.rend(); ++rit) {
        const HeaderIndexEntry* entry = *rit;

        // Restore preserved status from previous scan.
        auto it = preserved.find(entry->hash);
        if (it != preserved.end()) {
            missing_blocks_.push_back(it->second);
            if (it->second.status == FetchStatus::REQUESTED) {
                in_flight_blocks_.insert(entry->hash);
            }
            preserved_count++;
        } else {
            missing_blocks_.emplace_back(entry->hash, entry->height);
        }
        expected_blocks_.insert(entry->hash);
    }

    g_logger.info("[BlockDownloadScheduler] Queued " + std::to_string(missing_blocks_.size()) +
                 " blocks for download (" + std::to_string(preserved_count) + " preserved states)");
}

std::optional<BlockDownloadScheduler::StatelessFrontier>
BlockDownloadScheduler::FindStatelessFrontierLocked(uint32_t actual_tip) {
    for (size_t idx = 0; idx < missing_blocks_.size(); ++idx) {
        auto& fetch_state = missing_blocks_[idx];

        bool active_chain_matches = false;
        if (fetch_state.height <= actual_tip &&
            get_block_hash_at_height_callback_ &&
            fetch_state.height > 0) {
            uint256 chain_hash;
            if (get_block_hash_at_height_callback_(fetch_state.height, chain_hash) &&
                chain_hash == fetch_state.block_hash) {
                active_chain_matches = true;
            }
        }

        if (active_chain_matches) {
            if (fetch_state.status != FetchStatus::CONNECTED) {
                g_logger.info("[BlockDownloadScheduler] Stateless active-chain match at height " +
                              std::to_string(fetch_state.height) + ": " +
                              fetch_state.block_hash.GetHex().substr(0, 16) +
                              "... -> marking CONNECTED");
                fetch_state.status = FetchStatus::CONNECTED;
                in_flight_blocks_.erase(fetch_state.block_hash);
            }
            continue;
        }

        if (fetch_state.status == FetchStatus::CONNECTED) {
            continue;
        }

        return StatelessFrontier{idx, fetch_state.height <= actual_tip};
    }

    return std::nullopt;
}

bool BlockDownloadScheduler::RequestNextBlock() {
    // Cursor-based scan: start from next_missing_idx_ to avoid O(window*n) per Tick()
    size_t n = missing_blocks_.size();
    if (n == 0) return false;

    for (size_t i = 0; i < n; ++i) {
        size_t idx = (next_missing_idx_ + i) % n;
        auto& fetch_state = missing_blocks_[idx];

        if (fetch_state.status == FetchStatus::MISSING) {
            // Mark as REQUESTED with timestamp for stale detection
            fetch_state.status = FetchStatus::REQUESTED;
            fetch_state.request_time = std::chrono::steady_clock::now();
            in_flight_blocks_.insert(fetch_state.block_hash);

            // Advance cursor past this entry
            next_missing_idx_ = (idx + 1) % n;

            // Stage getdata for dispatch after mutex_ release (#241/#214)
            if (send_getdata_callback_) {
                StageGetdataLocked(fetch_state.block_hash, fetch_state.height);  // #241
                g_logger.info("[BlockDownloadScheduler] Requested block: " +
                             fetch_state.block_hash.GetHex() +
                             " (height " + std::to_string(fetch_state.height) + ")");
            } else {
                g_logger.warning("[BlockDownloadScheduler] send_getdata_callback not set");
            }

            return true;
        }
    }
    return false;
}

bool BlockDownloadScheduler::ReRequestBlock(const uint256& block_hash) {
    for (auto& fetch_state : missing_blocks_) {
        if (fetch_state.block_hash == block_hash) {
            fetch_state.status = FetchStatus::MISSING;
            in_flight_blocks_.erase(block_hash);
            g_logger.info("[BlockDownloadScheduler] Re-requesting block: " +
                         block_hash.GetHex() +
                         " (height " + std::to_string(fetch_state.height) + ")");
            return true;
        }
    }
    return false;
}

bool BlockDownloadScheduler::MarkBlockInvalid(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& fetch_state : missing_blocks_) {
        if (fetch_state.block_hash == block_hash) {
            fetch_state.status = FetchStatus::INVALID;
            fetch_state.stored_pos = FilePosition();
            in_flight_blocks_.erase(block_hash);
            received_blocks_.erase(block_hash);
            g_logger.error("[BlockDownloadScheduler] Marked block INVALID: " +
                           block_hash.GetHex() +
                           " (height " + std::to_string(fetch_state.height) + ")");
            return true;
        }
    }
    return false;
}

bool BlockDownloadScheduler::MarkBlockConnected(const uint256& block_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& fetch_state : missing_blocks_) {
        if (fetch_state.block_hash == block_hash) {
            if (fetch_state.status != FetchStatus::INVALID) {
                fetch_state.status = FetchStatus::CONNECTED;
                in_flight_blocks_.erase(block_hash);
                g_logger.info("[BlockDownloadScheduler] Block marked CONNECTED: " +
                              block_hash.GetHex());
            }
            return true;
        }
    }
    return false;
}

bool BlockDownloadScheduler::GetExpectedHashAtHeight(uint32_t height, uint256& out_hash) const {
    if (!header_chain_) return false;
    const HeaderIndexEntry* entry = header_chain_->GetHeaderAtHeight(height);
    if (!entry) return false;
    out_hash = entry->hash;
    return true;
}

bool BlockDownloadScheduler::ValidateBlockAgainstHeader(const Block& block) {
    if (!header_chain_) {
        return false;
    }

    // Get block hash
    uint256 block_hash = block.GetHash();

    // Find corresponding header
    const HeaderIndexEntry* header = header_chain_->GetHeader(block_hash);
    if (!header) {
        g_logger.warning("[BlockDownloadScheduler] No header found for block: " +
                        block_hash.GetHex());
        return false;
    }

    // Validate hash matches (redundant but explicit)
    if (block_hash != header->hash) {
        g_logger.error("[BlockDownloadScheduler] Block hash mismatch!");
        return false;
    }

    // Validate merkle root matches
    // Phase N.4.1: For skeleton, just compare header fields
    // Phase N.4.2: Will compute merkle root from transactions
    uint256 block_merkle = block.header.merkle_root;
    uint256 header_merkle = header->header.merkle_root;

    if (block_merkle != header_merkle) {
        g_logger.error("[BlockDownloadScheduler] Merkle root mismatch!");
        g_logger.error("  Block merkle:  " + block_merkle.GetHex());
        g_logger.error("  Header merkle: " + header_merkle.GetHex());
        return false;
    }

    // Header linkage already trusted by HeaderChainSelector
    // No need to re-validate previous block hash, timestamp, PoW, etc.

    g_logger.info("[BlockDownloadScheduler] Block validated successfully: " +
                 block_hash.GetHex());
    return true;
}

bool BlockDownloadScheduler::StoreBlock(const Block& block, FilePosition& out_pos) {
    // Phase N.4.4: Store block to flat file storage
    //
    // This writes the block to blk*.dat files via BlockStorage.
    // IMPORTANT: This does NOT activate chainstate or update UTXO set.
    // Block connection is done by TryConnectStoredBlocks() in height order.

    uint256 block_hash = block.GetHash();

    if (!block_storage_) {
        // No storage configured - log and skip
        g_logger.warning("[BlockDownloadScheduler] StoreBlock: no block_storage configured, skipping: " +
                        block_hash.GetHex());
        return true;  // Return true to allow continued sync without storage
    }

    // Write block to flat file storage
    auto result = block_storage_->writeBlock(block_hash, block);
    if (!result.ok()) {
        g_logger.error("[BlockDownloadScheduler] StoreBlock failed: " +
                      block_hash.GetHex() + " - " + dinero::StatusToString(result.status()));
        return false;
    }

    out_pos = result.value();
    g_logger.info("[BlockDownloadScheduler] StoreBlock success: " +
                 block_hash.GetHex() +
                 " (file=" + std::to_string(out_pos.file_number) +
                 ", offset=" + std::to_string(out_pos.offset) +
                 ", size=" + std::to_string(out_pos.size) + ")");

    // Track that we've received this block (for parent-known checks).
    received_blocks_.insert(block_hash);

    return true;
}

// ============================================================================
// Block Connection Drainer
// ============================================================================

size_t BlockDownloadScheduler::TryConnectStoredBlocks(size_t max_blocks) {
    if (!connect_block_callback_ || !block_storage_) {
        return 0;
    }

    // In CSN/stateless mode, block activation is driven by the ordered
    // OnUtxoBlock proof-validation path, not by the scheduler's flat-file
    // drainer. Trying to connect RECEIVED blocks here races ahead of proof
    // validation and reads proof-less blocks back from storage.
    if (stateless_mode_) {
        return 0;
    }

    size_t connected = 0;

    while (true) {
        if (max_blocks > 0 && connected >= max_blocks) {
            break;
        }

        // Determine which block to connect next.
        // Normal case: chainstate tip + 1.
        // Fork/reorg case: missing_blocks_ may contain blocks below the tip
        // that are on a better fork — connect them first to trigger the reorg.
        uint32_t actual_tip = get_tip_height_callback_
                                  ? get_tip_height_callback_()
                                  : local_tip_height_;
        if (actual_tip < local_tip_height_) {
            actual_tip = local_tip_height_;
        }
        uint32_t want = actual_tip + 1;

        // Check for fork blocks below the current tip. If missing_blocks_
        // starts below actual_tip, we're in a reorg scenario and need to
        // connect fork blocks first (they'll be accepted into the block
        // index and eventually trigger ActivateBestChain reorg).
        //
        // However, in CSN mode blocks are connected via OnUtxoBlock (bypassing
        // the scheduler), so blocks below the tip may still appear as MISSING/
        // REQUESTED even though the chainstate already has them. Auto-mark
        // these CONNECTED if the chainstate hash matches the expected hash.
        if (!missing_blocks_.empty()) {
            for (auto& fs : missing_blocks_) {
                if (fs.status != FetchStatus::CONNECTED &&
                    fs.status != FetchStatus::INVALID &&
                    fs.height <= actual_tip) {
                    // Check if chainstate already has this block at this height.
                    // If hash matches, the block was connected via an external
                    // path (e.g., CSN OnUtxoBlock) — mark it CONNECTED.
                    if (get_block_hash_at_height_callback_ && fs.height > 0) {
                        uint256 chain_hash;
                        if (get_block_hash_at_height_callback_(fs.height, chain_hash) &&
                            chain_hash == fs.block_hash) {
                            fs.status = FetchStatus::CONNECTED;
                            in_flight_blocks_.erase(fs.block_hash);
                            continue;  // Already in chainstate, skip
                        }
                    }
                    want = fs.height;
                    break;
                }
            }
        }

        auto want_it = std::find_if(
            missing_blocks_.begin(),
            missing_blocks_.end(),
            [want](const BlockFetchState& fs) { return fs.height == want; });

        if (want_it == missing_blocks_.end()) {
            // Gap between chainstate tip and queued range; rescan from actual tip.
            uint32_t first_unconnected_height = 0;
            bool found_unconnected = false;
            for (const auto& fs : missing_blocks_) {
                if (fs.status != FetchStatus::CONNECTED) {
                    first_unconnected_height = fs.height;
                    found_unconnected = true;
                    break;
                }
            }
            if (found_unconnected && first_unconnected_height > want) {
                g_logger.warning("[BlockDownloadScheduler] Gap detected: tip=" +
                                std::to_string(actual_tip) +
                                " want=" + std::to_string(want) +
                                " first_unconnected=" +
                                std::to_string(first_unconnected_height) +
                                " -> rescanning from tip");
                RescanFromActualTip(actual_tip);
            }
            break;
        }

        auto& fetch_state = *want_it;

        if (fetch_state.status == FetchStatus::CONNECTED) {
            // Only trust CONNECTED when the active chain actually has this
            // hash at this height. Older queue state can be polluted by
            // blocks that were accepted into the block index but never
            // activated, especially during CSN fork catch-up.
            bool active_chain_matches = false;
            if (get_block_hash_at_height_callback_ && want > 0) {
                uint256 chain_hash;
                if (get_block_hash_at_height_callback_(want, chain_hash) &&
                    chain_hash == fetch_state.block_hash) {
                    active_chain_matches = true;
                }
            }

            if (active_chain_matches) {
                local_tip_height_ = want;
            } else {
                g_logger.warning("[BlockDownloadScheduler] CONNECTED state mismatch at height " +
                                std::to_string(want) + " hash=" +
                                fetch_state.block_hash.GetHex().substr(0, 16) +
                                "... — downgrading to RECEIVED until active chain catches up");
                fetch_state.status = FetchStatus::RECEIVED;
            }
            break;
        }

        if (fetch_state.status == FetchStatus::INVALID) {
            g_logger.error("[BlockDownloadScheduler] Drain halted at invalid block height " +
                          std::to_string(fetch_state.height) + " hash=" +
                          fetch_state.block_hash.GetHex().substr(0, 16) + "...");
            break;
        }

        bool have_stored = (fetch_state.status == FetchStatus::RECEIVED);
        g_logger.info("[BlockDownloadScheduler] Drain want_height=" +
                     std::to_string(want) +
                     " status=" + std::to_string(static_cast<int>(fetch_state.status)) +
                     " have_stored=" + std::to_string(have_stored));

        // Block must be downloaded before it can be connected.
        // Priority cursor reset: steer next Tick()'s RequestNextBlock() to
        // this exact gap block instead of letting the cursor advance far
        // ahead. Without this, the cursor could be thousands of entries past
        // the gap and would take hours to wrap back at 1 slot per Tick.
        if (!have_stored) {
            auto gap_idx = static_cast<size_t>(
                std::distance(missing_blocks_.begin(), want_it));
            if (next_missing_idx_ != gap_idx) {
                g_logger.info("[BlockDownloadScheduler] Drain gap: resetting cursor from " +
                             std::to_string(next_missing_idx_) + " to " +
                             std::to_string(gap_idx) + " (height " +
                             std::to_string(want) + ")");
                next_missing_idx_ = gap_idx;
            }
            break;
        }

        // Read block back from flat file storage
        auto read_result = block_storage_->readBlock(fetch_state.stored_pos);
        if (!read_result.ok()) {
            g_logger.error("[BlockDownloadScheduler] Failed to read stored block at height " +
                          std::to_string(fetch_state.height) + ": " +
                          fetch_state.block_hash.GetHex().substr(0, 16) + "... (status=" +
                          std::to_string(static_cast<int>(read_result.status())) +
                          "), resetting to MISSING for re-request");

            // Avoid persistent drain spin on bad stored positions: force re-download.
            fetch_state.status = FetchStatus::MISSING;
            fetch_state.stored_pos = FilePosition();
            received_blocks_.erase(fetch_state.block_hash);
            break;
        }

        Block block = std::move(read_result.value());

        // Try to connect to chainstate
        ConnectBlockResult connect_result = connect_block_callback_(block, "scheduler-drain");
        switch (connect_result) {
            case ConnectBlockResult::CONNECTED:
            case ConnectBlockResult::DUPLICATE: {
                fetch_state.status = FetchStatus::CONNECTED;
                // Only advance local_tip if this extends the chain (not a fork
                // block stored as side-chain below the current active tip).
                if (want > local_tip_height_) {
                    local_tip_height_ = want;
                }
                connected++;

                g_logger.info("[BlockDownloadScheduler] Connected block at height " +
                             std::to_string(fetch_state.height) + ": " +
                             fetch_state.block_hash.GetHex().substr(0, 16) + "...");
                break;
            }
            case ConnectBlockResult::ACCEPTED_NOT_ACTIVE:
                // The block is stored/indexed, but not on the active chain at
                // this height yet. Keep it in RECEIVED so the drainer does not
                // advance its local tip or skip earlier heights.
                fetch_state.status = FetchStatus::RECEIVED;
                g_logger.info("[BlockDownloadScheduler] Stored block accepted but not active at height " +
                             std::to_string(want) + ": " +
                             fetch_state.block_hash.GetHex().substr(0, 16) + "...");
                return connected;
            case ConnectBlockResult::MISSING_PARENT: {
                uint256 parent_hash = block.header.prev_block_hash;
                uint256 expected_parent;
                if (want > 0 && GetExpectedHashAtHeight(want - 1, expected_parent)) {
                    parent_hash = expected_parent;
                }

                // Use non-locking helper — mutex_ is already held by Tick().
                bool parent_in_flight = isBlockInFlightLocked(parent_hash);
                const auto now = std::chrono::steady_clock::now();
                bool cooldown_active = false;
                auto req_it = parent_request_times_.find(parent_hash);
                if (req_it != parent_request_times_.end()) {
                    cooldown_active =
                        (now - req_it->second) <
                        std::chrono::seconds(parent_request_cooldown_seconds_);
                }

                bool requested_parent = false;
                if (!parent_in_flight && !cooldown_active && send_getdata_callback_) {
                    // Parent sits one height below the block we're trying to connect.
                    // Staged for dispatch after mutex_ release (#241/#214).
                    StageGetdataLocked(parent_hash, want > 0 ? want - 1 : 0);  // #241
                    parent_request_times_[parent_hash] = now;
                    requested_parent = true;
                }

                g_logger.warning("[BlockDownloadScheduler] Drain blocked at height " +
                                std::to_string(want) +
                                " missing parent " + parent_hash.GetHex().substr(0, 16) +
                                "... requested_parent=" + std::to_string(requested_parent) +
                                " inflight_parent=" + std::to_string(parent_in_flight) +
                                " cooldown_active=" + std::to_string(cooldown_active));
                return connected;
            }
            case ConnectBlockResult::WAITING_PARENT:
                g_logger.debug("[BlockDownloadScheduler] Drain waiting on parent for height " +
                              std::to_string(want));
                return connected;
            case ConnectBlockResult::TEMPORARY_FAIL:
                g_logger.warning("[BlockDownloadScheduler] Drain temporary failure at height " +
                                std::to_string(want) + ", will retry next tick");
                return connected;
            case ConnectBlockResult::INVALID:
                fetch_state.status = FetchStatus::INVALID;
                in_flight_blocks_.erase(fetch_state.block_hash);
                expected_blocks_.erase(fetch_state.block_hash);
                g_logger.error("[BlockDownloadScheduler] Drain marked invalid at height " +
                              std::to_string(want) + " hash=" +
                              fetch_state.block_hash.GetHex().substr(0, 16) + "...");
                return connected;
        }
    }

    if (connected > 0) {
        g_logger.info("[BlockDownloadScheduler] Drain: connected " +
                     std::to_string(connected) + " blocks, tip now at height " +
                     std::to_string(local_tip_height_));
    }

    return connected;
}

// ============================================================================
// Gap Recovery: Rescan from actual chainstate tip
// ============================================================================

void BlockDownloadScheduler::RescanFromActualTip(uint32_t actual_tip) {
    if (!header_chain_) return;

    // Never regress below our known local tip (chainstate may report a lower
    // height during startup while replaying blocks from RocksDB).
    if (actual_tip < local_tip_height_) {
        g_logger.debug("[BlockDownloadScheduler] RescanFromActualTip: ignoring regression " +
                      std::to_string(actual_tip) + " < local_tip " +
                      std::to_string(local_tip_height_));
        return;
    }

    // Preserve local status for blocks we already have.
    std::unordered_map<uint256, std::pair<FetchStatus, FilePosition>> preserved;
    for (const auto& fs : missing_blocks_) {
        if (fs.status == FetchStatus::RECEIVED ||
            fs.status == FetchStatus::CONNECTED ||
            fs.status == FetchStatus::INVALID) {
            preserved[fs.block_hash] = {fs.status, fs.stored_pos};
        }
    }

    // Update tip and rescan
    local_tip_height_ = actual_tip;
    ScanForMissingBlocks();  // Clears and rebuilds missing_blocks_ from actual_tip + 1

    // Restore preserved status
    for (auto& fs : missing_blocks_) {
        auto it = preserved.find(fs.block_hash);
        if (it != preserved.end()) {
            fs.status = it->second.first;
            fs.stored_pos = it->second.second;
        }
    }

    g_logger.info("[BlockDownloadScheduler] Rescanned from tip " +
                 std::to_string(actual_tip) + ": " +
                 std::to_string(missing_blocks_.size()) + " blocks queued, " +
                 std::to_string(preserved.size()) + " already downloaded");
}

} // namespace consensus
} // namespace dinero
