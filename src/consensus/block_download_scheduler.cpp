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
    , block_storage_(block_storage)
    , backfill_last_progress_(std::chrono::steady_clock::now()) {  // #298 diag seed

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
    // in-flight request (which was the catch-up wedge). The hash→height lookup
    // spans BOTH queues — the demotion map is shared, and AssumeUTXO backfill
    // heights are exactly the pre-snapshot bodies such peers lack.
    if (!peer_key.empty()) {
        uint32_t notfound_height = 0;
        bool height_known = false;
        for (const auto& fs : missing_blocks_) {
            if (fs.block_hash == hash) {
                notfound_height = fs.height;
                height_known = true;
                break;
            }
        }
        if (!height_known) {
            for (const auto& fs : backfill_blocks_) {
                if (fs.block_hash == hash) {
                    notfound_height = fs.height;
                    height_known = true;
                    break;
                }
            }
        }
        if (height_known) {
            uint32_t& gap = peer_lacks_body_at_or_below_[peer_key];
            if (notfound_height > gap) gap = notfound_height;
        }
    }

    // Backfill NOTFOUND: flip the entry back to MISSING so the next service
    // pass retries it via another (non-demoted) peer, and release the
    // in-flight accounting. Mirrors the tip path's NOTFOUND handling: the
    // shared in_flight_blocks_ entry was already erased above; tip entries
    // are re-queued by the rescan below / the stale sweep, while backfill
    // entries are owned here.
    for (auto& fs : backfill_blocks_) {
        if (fs.block_hash == hash) {
            if (fs.status == FetchStatus::REQUESTED) {
                fs.status = FetchStatus::MISSING;
                if (backfill_progress_.in_flight > 0) {
                    backfill_progress_.in_flight--;
                }
            }
            break;
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

void BlockDownloadScheduler::NotifyGetDataDispatched(const uint256& block_hash,
                                                     size_t recipient_count) {
    if (recipient_count > 0) {
        return;  // getdata reached at least one peer — normal path
    }
    // Zero-recipient send: the staged getdata was dispatched but the daemon's
    // per-block peer selection filtered out EVERY peer (CSN bridge filter +
    // body-incapable skip-set leaving no eligible recipient). RequestNextBlock()
    // already marked this block REQUESTED and inserted it into in_flight_blocks_,
    // so it counts against max_in_flight_ — yet no peer will respond. The stale
    // sweep only reclaims it after a full timeout, then it is re-sent to zero
    // peers again; max_in_flight_ such phantoms pin the entire window with the
    // tip frozen (observed live: scheduler in_flight=16, per-peer inflight=0,
    // height stuck). Revert to MISSING now so the slot frees and the next Tick()
    // retries it (against a hopefully-eligible peer).
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& fs : missing_blocks_) {
        if (fs.block_hash == block_hash && fs.status == FetchStatus::REQUESTED) {
            fs.status = FetchStatus::MISSING;
            in_flight_blocks_.erase(block_hash);
            g_logger.warning("[BlockDownloadScheduler] getdata reached 0 peers; "
                             "released phantom in-flight (height " +
                             std::to_string(fs.height) + "): " +
                             block_hash.GetHex().substr(0, 16) + "...");
            return;
        }
    }
    for (auto& fs : backfill_blocks_) {
        if (fs.block_hash == block_hash && fs.status == FetchStatus::REQUESTED) {
            fs.status = FetchStatus::MISSING;
            in_flight_blocks_.erase(block_hash);
            if (backfill_progress_.in_flight > 0) {
                backfill_progress_.in_flight--;
            }
            return;
        }
    }
}

bool BlockDownloadScheduler::OnBlockReceived(const Block& block) {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint256 block_hash = block.GetHash();
    g_logger.info("[BlockDownloadScheduler] OnBlockReceived: " + block_hash.GetHex());

    // AssumeUTXO backfill routing (Task 2): a pre-base body is verified and
    // stored EXACTLY like a tip block (same helper), but is store-only — it
    // never enters tip-connect bookkeeping (received_blocks_, expected_blocks_,
    // TryConnectStoredBlocksLocked inputs). The background validation worker reads
    // it back from flat-file storage via the canonical header chain.
    if (backfill_expected_.count(block_hash) > 0) {
        FilePosition stored_pos;
        if (!StoreVerifiedBlockLocked(block, stored_pos, /*track_received=*/false)) {
            return false;
        }
        for (auto& fs : backfill_blocks_) {
            if (fs.block_hash == block_hash) {
                if (fs.status == FetchStatus::REQUESTED &&
                    backfill_progress_.in_flight > 0) {
                    backfill_progress_.in_flight--;
                }
                fs.status = FetchStatus::RECEIVED;
                fs.stored_pos = stored_pos;
                break;
            }
        }
        backfill_progress_.completed++;
        backfill_last_progress_ = std::chrono::steady_clock::now();  // #298 diag
        in_flight_blocks_.erase(block_hash);
        backfill_expected_.erase(block_hash);
        g_logger.info("[BlockDownloadScheduler] Backfill body stored: " +
                      block_hash.GetHex().substr(0, 16) + "... (" +
                      std::to_string(backfill_progress_.completed) + "/" +
                      std::to_string(backfill_progress_.total) + ")");

        // #298: backfill window just reached completed==total → log a one-line
        // snapshot of the idle transition (helps post-mortem a stalled backfill).
        if (backfill_progress_.total > 0 &&
            backfill_progress_.completed == backfill_progress_.total) {
            LogBackfillDiagLocked();
        }

        // #353 bug-2 companion: persist the backfilled body's position to
        // ChainDB (mirror the tip path's #309 persist below). The background
        // validation worker reads pre-base bodies via RequireFlatfiles, which
        // needs getHeaderMetadata(hash).data_size > 0 — i.e. the persisted
        // body position. A body that arrives ONLY via backfill and is never
        // tip-connected (e.g. while the AssumeUTXO promotion hold keeps the
        // canonical tip at the snapshot base) is stored in the flatfile but
        // otherwise unreadable, so genesis->base replay livelocks. Persisting
        // here makes backfilled bodies readable regardless of whether anything
        // connects the pre-base range. Invoked OUTSIDE mutex_, like the wake
        // and tip paths.
        //
        // #298 wake-on-store: notify the background validation worker that a
        // pre-base body landed so it can re-attempt its read without polling.
        // Copy the std::functions and invoke them AFTER releasing mutex_ — the
        // callbacks touch their own mutex/condvar / write ChainDB, and we must
        // never hold the scheduler lock across a foreign callback (re-entrancy /
        // lock-order deadlock guard, same discipline as the #241 send path).
        std::function<void()> wake = on_backfill_body_stored_;
        auto persist = persist_body_position_callback_;
        lock.unlock();
        if (persist) {
            persist(block_hash, stored_pos);
        }
        if (wake) {
            wake();
        }
        return true;
    }

    // Tip path: validate + store, then enter connect bookkeeping.
    FilePosition stored_pos;
    if (!StoreVerifiedBlockLocked(block, stored_pos, /*track_received=*/true)) {
        return false;
    }

    // Mark block as received and record its storage position
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

    // #309: persist the stored body's position metadata so a not-yet-connected
    // block (a competing side-branch above the active tip) is recognized by
    // HasArchivalBlockBody / the import loop and its branch tip can become a
    // reorg candidate. Invoked OUTSIDE the scheduler mutex (the callback writes
    // ChainDB / may take application locks), mirroring the wake-on-store path.
    auto persist = persist_body_position_callback_;
    lock.unlock();
    if (persist) {
        persist(block_hash, stored_pos);
    }

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

void BlockDownloadScheduler::MaybeRunStallWatchdogLocked(
    std::chrono::steady_clock::time_point now) {
    // Initialize progress tracking on first run (don't fire before any work has
    // had a chance to make progress).
    if (watchdog_last_progress_time_.time_since_epoch().count() == 0) {
        watchdog_last_progress_time_ = now;
        watchdog_last_progress_height_ = local_tip_height_;
        return;
    }
    // Tip advanced since last check? Healthy — reset the stall timer.
    if (local_tip_height_ > watchdog_last_progress_height_) {
        watchdog_last_progress_height_ = local_tip_height_;
        watchdog_last_progress_time_ = now;
        return;
    }
    // Only a stall if there is still tip work queued (MISSING or REQUESTED).
    bool pending = false;
    for (const auto& fs : missing_blocks_) {
        if (fs.status == FetchStatus::MISSING || fs.status == FetchStatus::REQUESTED) {
            pending = true;
            break;
        }
    }
    if (!pending) {
        watchdog_last_progress_time_ = now;  // nothing queued → not stalled
        return;
    }
    const int64_t stall_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - watchdog_last_progress_time_).count();
    const int64_t since_fire = std::chrono::duration_cast<std::chrono::seconds>(
        now - watchdog_last_fire_).count();
    if (stall_elapsed < static_cast<int64_t>(stall_watchdog_seconds_) ||
        since_fire < static_cast<int64_t>(stall_watchdog_seconds_)) {
        return;  // not stalled long enough, or fired too recently (rate-limit)
    }
    // STALL: tip frozen with blocks queued for stall_watchdog_seconds_, despite
    // the per-block retry/stale-sweep machinery. Force-recover regardless of the
    // specific cause (phantom in-flight, skip-set poisoning, peer-selection
    // wedge): drop the body-incapable skip-set so every peer is eligible again,
    // and reset all REQUESTED entries to MISSING so the next request round goes
    // out UNFILTERED. This is the "shouldn't depend on a human noticing" net.
    g_logger.warning("[BlockDownloadScheduler] stall watchdog: no tip progress for " +
                     std::to_string(stall_elapsed) + "s with blocks queued — clearing "
                     "skip-set (" + std::to_string(peer_lacks_body_at_or_below_.size()) +
                     " demoted peers) and resetting in-flight for unfiltered retry");
    peer_lacks_body_at_or_below_.clear();
    for (auto& fs : missing_blocks_) {
        if (fs.status == FetchStatus::REQUESTED) {
            fs.status = FetchStatus::MISSING;
            in_flight_blocks_.erase(fs.block_hash);
        }
    }
    next_missing_idx_ = 0;
    watchdog_last_fire_ = now;
    watchdog_last_progress_time_ = now;  // give recovery a full window before re-firing
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

    // Defense-in-depth stall net: recover if the tip has been frozen with work
    // queued for too long, regardless of the underlying cause.
    if (stall_watchdog_seconds_ > 0) {
        MaybeRunStallWatchdogLocked(std::chrono::steady_clock::now());
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

    // Same #216-lineage stale sweep for the backfill queue: a REQUESTED
    // backfill body that never arrived must return to MISSING so another peer
    // is tried, and must release its slot in the SHARED in-flight window —
    // otherwise stale backfill requests would silently shrink the tip-sync
    // window. Lives here (not in ServiceBackfillLocked) so slots are released
    // even on ticks where backfill yields to tip work. No #216 "already have"
    // guard: backfill bodies are store-only and never connect, so the
    // parallel-connect race that guard covers cannot occur.
    for (auto& fs : backfill_blocks_) {
        if (fs.status == FetchStatus::REQUESTED) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - fs.request_time).count();
            if (elapsed >= stale_request_timeout_seconds_) {
                g_logger.info("[BlockDownloadScheduler] Stale backfill request expired: " +
                              fs.block_hash.GetHex().substr(0, 16) +
                              "... (height " + std::to_string(fs.height) +
                              ", waited " + std::to_string(elapsed) + "s)");
                fs.status = FetchStatus::MISSING;
                in_flight_blocks_.erase(fs.block_hash);
                if (backfill_progress_.in_flight > 0) {
                    backfill_progress_.in_flight--;
                }
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
    TryConnectStoredBlocksLocked();

    // Backfill is strictly lower priority: only when tip sync has nothing
    // MISSING or REQUESTED do we spend request slots on history. Its sends
    // are staged into deferred_sends_ and ride the same post-lock dispatch
    // in Tick() as tip getdata (#241/#214).
    ServiceBackfillLocked();
}

// AssumeUTXO backfill servicing (Task 2). Caller MUST hold mutex_. See the
// header declaration for the priority/cap/staging contract.
void BlockDownloadScheduler::ServiceBackfillLocked() {
    if (!backfill_progress_.enabled || backfill_blocks_.empty()) {
        return;
    }
    // Honor the central deferral (issue #186) even if a future call site
    // reaches here without TickLocked()'s early return.
    if (defer_check_ && defer_check_()) {
        return;
    }

    // Tip sync busy? (ANY MISSING or REQUESTED tip block) → yield. Backfill
    // is history repair; the tip window gets every slot first.
    for (const auto& fs : missing_blocks_) {
        if (fs.status == FetchStatus::MISSING ||
            fs.status == FetchStatus::REQUESTED) {
            return;
        }
    }

    // Shared global in-flight cap with tip sync: backfill hashes live in
    // in_flight_blocks_ too, so tip work resuming next tick sees a truthful
    // window (and backfill can never oversubscribe the peer pipeline).
    const size_t max_window = static_cast<size_t>(max_in_flight_);
    const auto now = std::chrono::steady_clock::now();
    const size_t n = backfill_blocks_.size();
    // Cursor fairness: resume where the last service pass stopped instead of
    // rescanning from index 0 (snapshot the cursor — it advances inside the
    // loop, so indexing off the live value would skip/revisit entries).
    const size_t start = next_backfill_idx_;
    size_t staged = 0;
    for (size_t i = 0; i < n && in_flight_blocks_.size() < max_window; ++i) {
        const size_t idx = (start + i) % n;
        auto& fs = backfill_blocks_[idx];
        if (fs.status != FetchStatus::MISSING) {
            continue;
        }

        fs.status = FetchStatus::REQUESTED;
        fs.request_time = now;  // re-armed by the stale sweep in TickLocked()
        in_flight_blocks_.insert(fs.block_hash);
        backfill_progress_.in_flight++;
        next_backfill_idx_ = (idx + 1) % n;

        if (send_getdata_callback_) {
            // #241 reuse: StageGetdataLocked snapshots the skip-set (every
            // peer known to lack bodies at/above this height) and defers the
            // send; Tick() dispatches after mutex_ release.
            StageGetdataLocked(fs.block_hash, fs.height);
            staged++;
        } else {
            g_logger.warning("[BlockDownloadScheduler] send_getdata_callback not set (backfill)");
        }
    }

    if (staged > 0) {
        g_logger.info("[BlockDownloadScheduler] Backfill: staged " +
                      std::to_string(staged) + " getdata (in_flight=" +
                      std::to_string(backfill_progress_.in_flight) +
                      ", completed=" + std::to_string(backfill_progress_.completed) +
                      "/" + std::to_string(backfill_progress_.total) + ")");
    }
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
                                             uint32_t end_height,
                                             const uint256& end_anchor_hash) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Idempotent: same (range, anchor) while already enabled → preserve
    // in-progress state.
    if (backfill_progress_.enabled &&
        backfill_progress_.start_height == start_height &&
        backfill_progress_.end_height == end_height &&
        backfill_anchor_hash_ == end_anchor_hash) {
        return;
    }

    // (range, anchor) DIFFERS from the active state (or backfill is off):
    // perform the full DisableBackfill cleanup FIRST. Two contracts hang on
    // this ordering:
    //   1. No in-flight leak on a direct base change: the old queue's
    //      REQUESTED hashes are erased from in_flight_blocks_ before the new
    //      queue is populated — re-Enable with requests on the wire cannot
    //      strand slots in the shared window (the wiring's Disable-on-every-
    //      non-validating-tick covers fatal/reset/retire, but a reset + new
    //      snapshot between two ticks would re-Enable directly).
    //   2. A REFUSED Enable (anchor not yet known) leaves enabled == false,
    //      so the caller's periodic re-arm genuinely retries instead of
    //      being swallowed by the idempotency check against stale state.
    DisableBackfillLocked();

    // Resolve the anchor BY HASH, never by height. end_anchor_hash is the
    // AssumeUTXO snapshot base block hash — the trust root. The snapshot
    // load gate is existence-only (any known header passes, side branches
    // included), so the BEST header chain may diverge below the base; a
    // GetHeaderAtHeight(end_height) anchor would then walk the FORK and
    // queue the fork's bodies for download — feeding the validation worker a
    // complete fork replay and a persisted false fatal_mismatch. Anchoring
    // on the hash pins the walk to the snapshot's own chain regardless of
    // what the best chain does. The height check is explicit: a wired-up
    // mismatch is a caller bug we want loud.
    //
    // The resolve + walk + copy happen atomically under the HEADER CHAIN's
    // own mutex (CollectAncestorsByHash), not just ours: the base may be a
    // childless side-branch tip, which EvictBranch (running under the header
    // chain's lock on another thread) can free mid-walk if we held only the
    // scheduler mutex_ — raw GetHeader() pointers don't survive that.
    uint32_t anchor_height = 0;
    std::vector<std::pair<uint256, uint32_t>> window;
    const bool anchor_known =
        header_chain_ && header_chain_->CollectAncestorsByHash(
                             end_anchor_hash, start_height, anchor_height, window);
    if (!anchor_known || anchor_height != end_height) {
        // Do NOT enable: the caller's periodic re-arm retries once headers
        // for the snapshot branch arrive.
        g_logger.warning("[BlockDownloadScheduler] EnableBackfill: anchor " +
                         end_anchor_hash.GetHex().substr(0, 16) +
                         "... not found at height " + std::to_string(end_height) +
                         (anchor_known ? " (header exists at height " +
                                             std::to_string(anchor_height) + ")"
                                       : " (header unknown)") +
                         " — backfill not enabled");
        return;
    }
    if (window.empty()) {
        // Degenerate range — refuse, queue stays empty.
        g_logger.warning("[BlockDownloadScheduler] EnableBackfill: no canonical headers for "
                         "range " + std::to_string(start_height) + ".." +
                         std::to_string(end_height));
        return;
    }

    backfill_progress_.enabled = true;
    backfill_progress_.start_height = start_height;
    backfill_progress_.end_height = end_height;
    backfill_anchor_hash_ = end_anchor_hash;

    for (const auto& [hash, height] : window) {
        if (has_block_body_ && has_block_body_(hash, height)) {
            continue;  // body already present; skip
        }
        backfill_blocks_.emplace_back(hash, height);
        backfill_expected_.insert(hash);
    }
    backfill_progress_.total = backfill_blocks_.size();

    g_logger.info("[BlockDownloadScheduler] EnableBackfill: range=" +
                  std::to_string(start_height) + ".." + std::to_string(end_height) +
                  " missing=" + std::to_string(backfill_progress_.total));
}

void BlockDownloadScheduler::DisableBackfill() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Already disabled and empty → silent no-op. The daemon disarms on every
    // periodic tick while not validating (covers retirement, fatal AND reset
    // with zero extra plumbing), so this must not log every 5 seconds on
    // every non-assumeutxo node.
    if (!backfill_progress_.enabled && backfill_blocks_.empty()) {
        return;
    }

    DisableBackfillLocked();
    g_logger.info("[BlockDownloadScheduler] DisableBackfill");
}

void BlockDownloadScheduler::DisableBackfillLocked() {
    // Release any in-flight accounting so the main window stays consistent.
    for (const auto& fs : backfill_blocks_) {
        in_flight_blocks_.erase(fs.block_hash);
    }
    backfill_blocks_.clear();
    backfill_expected_.clear();
    next_backfill_idx_ = 0;
    backfill_progress_ = BackfillProgress{};
    backfill_anchor_hash_ = uint256();
}

BlockDownloadScheduler::BackfillProgress
BlockDownloadScheduler::GetBackfillProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backfill_progress_;
}

void BlockDownloadScheduler::SetOnBackfillBodyStored(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_backfill_body_stored_ = std::move(cb);
}

// #298 diag: one-line backfill state snapshot. Caller MUST hold mutex_.
void BlockDownloadScheduler::LogBackfillDiagLocked() const {
    size_t missing = 0;
    for (const auto& fs : backfill_blocks_) {
        if (fs.status == FetchStatus::MISSING) ++missing;
    }
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - backfill_last_progress_).count();
    g_logger.info(std::string("[BlockDownloadScheduler] #298 backfill-diag:") +
                  " enabled=" + (backfill_progress_.enabled ? "1" : "0") +
                  " window=[" + std::to_string(backfill_progress_.start_height) +
                  "," + std::to_string(backfill_progress_.end_height) + "]" +
                  " completed=" + std::to_string(backfill_progress_.completed) +
                  "/" + std::to_string(backfill_progress_.total) +
                  " in_flight=" + std::to_string(backfill_progress_.in_flight) +
                  " missing=" + std::to_string(missing) +
                  " since_progress=" + std::to_string(since) + "s");
}

// #298: backfill-aware targeted re-request. See the header for the contract.
// The backfill drain (ServiceBackfillLocked) gates ONLY on enabled + a non-empty
// queue + no pending tip work + the shared in-flight cap + entry status==MISSING
// — it has NO completed<total / "window done" gate — so a re-queued MISSING entry
// is drained on the next Tick with no change to the drain itself. The only thing
// that could strand a re-queue is `enabled==false` after a one-shot window went
// idle/disabled, which is why we (re)assert enabled below.
size_t BlockDownloadScheduler::RequestMissingBackfillBodies(
        const std::vector<std::pair<uint256, uint32_t>>& want) {
    std::lock_guard<std::mutex> lock(mutex_);

    // #298 diag: snapshot state at the top of every reconciliation call.
    LogBackfillDiagLocked();

    size_t requeued = 0;
    for (const auto& [hash, height] : want) {
        // Already durably stored? Validation can read it → nothing to do.
        if (has_block_body_ && has_block_body_(hash, height)) {
            continue;
        }

        // Find the existing entry: the window may still hold it as RECEIVED
        // after reporting itself complete (the #298 false-complete scenario).
        bool found = false;
        for (auto& fs : backfill_blocks_) {
            if (fs.block_hash == hash) {
                if (fs.status == FetchStatus::REQUESTED &&
                    backfill_progress_.in_flight > 0) {
                    backfill_progress_.in_flight--;  // mirror in_flight bookkeeping
                }
                // A RECEIVED entry already counted toward completed; it is no
                // longer durable, so uncount it — keeps the completed==total
                // idle signal honest (the false-complete is exactly #298).
                if (fs.status == FetchStatus::RECEIVED &&
                    backfill_progress_.completed > 0) {
                    backfill_progress_.completed--;
                }
                fs.status = FetchStatus::MISSING;
                fs.stored_pos = FilePosition();
                found = true;
                break;
            }
        }

        if (!found) {
            // Window already cleared: re-create a fresh MISSING entry so the
            // next service pass fetches it, and grow total to match.
            backfill_blocks_.emplace_back(hash, height);
            backfill_progress_.total++;
        }

        // CRITICAL (both paths): re-arm OnBlockReceived routing. A RECEIVED body
        // erased itself from backfill_expected_ on store (OnBlockReceived), so
        // without this the re-delivered block would fall through to the TIP path
        // — never re-counted, polluting received_blocks_, and looping forever as
        // ServiceBackfillLocked re-requests a never-completing entry. Inserting
        // is a no-op for MISSING/REQUESTED entries (still in the set).
        backfill_expected_.insert(hash);
        in_flight_blocks_.erase(hash);  // release any stale shared-window slot

        ++requeued;
        g_logger.info("[BlockDownloadScheduler] #298 re-request backfill body height=" +
                      std::to_string(height) + " hash=" +
                      hash.GetHex().substr(0, 16) + " (reason=validation-gap)");
    }

    // A one-shot window may have gone idle (or been disabled). Re-arming a fetch
    // means backfill is live again, so ServiceBackfillLocked won't early-return.
    if (requeued > 0) {
        backfill_progress_.enabled = true;
    }

    return requeued;
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

    // Backfill in-flight hashes share the window: preserve them across rescans
    // (the rescan only restructures TIP work; backfill requests stay on the wire).
    // This re-add MUST sit immediately after the clear, before ANY early return:
    // the "Already synchronized" exit below is the NORMAL steady state of a
    // snapshot-loaded node at tip, and skipping the re-add there dropped the
    // outstanding backfill hashes from the cap authority on every headers
    // message — the next service pass then staged another full cap's worth
    // (2x oversubscription).
    for (const auto& fs : backfill_blocks_) {
        if (fs.status == FetchStatus::REQUESTED) {
            in_flight_blocks_.insert(fs.block_hash);
        }
    }

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
    // backward walk over parent pointers (shared helper) instead of
    // GetHeaderAtHeight(h) per height — each of those calls walks from the
    // best header, making the scan O(n^2). On a from-genesis sync (~39k
    // headers) that pinned a core inside this loop under mutex_ on EVERY
    // headers message and throttled ingest to a few blocks/min. Tip sync's
    // anchor IS the best header (already resolved above), so the total stays
    // linear.
    std::vector<const HeaderIndexEntry*> window;
    if (!CollectCanonicalHeadersLocked(start_height, best_header, window)) {
        return;
    }

    size_t preserved_count = 0;
    for (const HeaderIndexEntry* entry : window) {
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

// Single-backward-walk collector for the header window
// [start_height, anchor->height], ascending order (carryover from Task 1
// review; used by ScanForMissingBlocks with a BEST-CHAIN anchor). Parent
// links down to start_height: O(anchor->height - start_height) total.
// Calling GetHeaderAtHeight(h) per height instead would be O(n²), the #241
// scan bug (commit 9bc061782).
//
// LIFETIME: walks raw parent pointers WITHOUT the header chain's lock —
// valid only for best-chain anchors (never evicted). EnableBackfill's
// possibly-side-branch anchor must NOT come through here; it uses
// HeaderChainSelector::CollectAncestorsByHash (copies under the header
// chain's own mutex, immune to concurrent EvictBranch).
bool BlockDownloadScheduler::CollectCanonicalHeadersLocked(
        uint32_t start_height, const HeaderIndexEntry* anchor,
        std::vector<const HeaderIndexEntry*>& out_ascending) const {
    out_ascending.clear();
    if (!anchor || start_height > anchor->height) {
        return false;
    }

    out_ascending.reserve(anchor->height - start_height + 1);
    for (const HeaderIndexEntry* e = anchor; e && e->height >= start_height;
         e = e->parent) {
        out_ascending.push_back(e);
        if (e->height == start_height) {
            break;  // height-0 guard: `e->height >= 0` is never false (unsigned)
        }
    }
    std::reverse(out_ascending.begin(), out_ascending.end());
    return true;
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

    // Find corresponding header. COPY the entry out under the header chain's
    // lock: backfill routes side-branch snapshot-chain hashes through here,
    // and EvictBranch (which runs under the header chain's mutex, NOT this
    // class's mutex_) can free a raw GetHeader() pointer concurrently —
    // same use-after-free class as the EnableBackfill anchor walk (C4).
    HeaderIndexEntry header;
    if (!header_chain_->GetHeaderCopy(block_hash, header)) {
        g_logger.warning("[BlockDownloadScheduler] No header found for block: " +
                        block_hash.GetHex());
        return false;
    }

    // Validate hash matches (redundant but explicit)
    if (block_hash != header.hash) {
        g_logger.error("[BlockDownloadScheduler] Block hash mismatch!");
        return false;
    }

    // Validate merkle root matches
    // Phase N.4.1: For skeleton, just compare header fields
    // Phase N.4.2: Will compute merkle root from transactions
    uint256 block_merkle = block.header.merkle_root;
    uint256 header_merkle = header.header.merkle_root;

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

// Shared verify+persist core for the tip and backfill receive paths (Task 2
// DRY): header-match validation, then flat-file write. Caller MUST hold
// mutex_. track_received=false for backfill bodies (store-only — they must
// never enter received_blocks_ / connect bookkeeping).
bool BlockDownloadScheduler::StoreVerifiedBlockLocked(const Block& block,
                                                      FilePosition& out_pos,
                                                      bool track_received) {
    if (!ValidateBlockAgainstHeader(block)) {
        g_logger.warning("[BlockDownloadScheduler] Block validation failed: " +
                        block.GetHash().GetHex());
        return false;
    }

    // Store block (but do NOT activate chainstate)
    if (!StoreBlock(block, out_pos, track_received)) {
        g_logger.error("[BlockDownloadScheduler] Failed to store block: " +
                      block.GetHash().GetHex());
        return false;
    }

    return true;
}

bool BlockDownloadScheduler::StoreBlock(const Block& block, FilePosition& out_pos,
                                        bool track_received) {
    // Phase N.4.4: Store block to flat file storage
    //
    // This writes the block to blk*.dat files via BlockStorage.
    // IMPORTANT: This does NOT activate chainstate or update UTXO set.
    // Block connection is done by TryConnectStoredBlocksLocked() in height order.

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
    // Backfill bodies pass track_received=false: they are store-only and must
    // not enter the tip path's connect bookkeeping (received_blocks_ also
    // feeds the #216 stale-timeout "already have" guard).
    if (track_received) {
        received_blocks_.insert(block_hash);
    }

    return true;
}

// ============================================================================
// Block Connection Drainer
// ============================================================================

size_t BlockDownloadScheduler::TryConnectStoredBlocksLocked(size_t max_blocks) {
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
