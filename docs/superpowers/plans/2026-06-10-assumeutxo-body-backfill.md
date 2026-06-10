# AssumeUTXO Pre-Base Body Backfill Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After an AssumeUTXO snapshot load, the node downloads historical block bodies 1..base from peers unattended, so the replay-engine background validation (PR #270) can reach `fully_validated` without operator `submitblock`/`--reindex` — closing spec Release Gate item 2's operational blocker.

**Architecture:** A separate low-priority **backfill queue** inside the existing `BlockDownloadScheduler` (own accounting — never pollutes `missing_blocks_`/`IsFullySynchronized`, so fast-bootstrap UX is untouched), serviced only when tip-sync is idle, reusing the #241 NOTFOUND skip-peers mechanism verbatim. Received backfill bodies take the scheduler's existing store-only path (`ValidateBlockAgainstHeader` + `StoreBlock`, no connect). The validation worker gains a **header-chain fallback lookup** (canonical hash from `GetHeaderAtHeight` when the height index is absent) — deliberately NO new `putHeightIndex` writes outside ConnectTip, so canonical-write invariants are untouched; the worker's existing body-hash check (PR #270 A2) guards integrity. Daemon wiring enables backfill after snapshot load / on validating restore and disables it on retirement/fatal/reset. The keystone proof: the e2e's `submitblock` + `--reindex` workaround is DELETED and scenario A passes through real P2P backfill.

**Tech Stack:** C++17, CMake, GoogleTest (the scheduler's excellent callback-mock fixture), regtest shell e2e.

**Branch:** `worktree-assumeutxo-body-backfill` off post-#270 `dinero-main` (98d5e1310). First commit: this plan.

---

## Scope

**In scope:** scheduler backfill queue + servicing + accounting; worker header-chain fallback; daemon enable/disable wiring; RPC progress fields; e2e workaround removal.

**Explicit non-goals:**
- Wallet-send safe-mode gating (still its own follow-up).
- Mainnet fleet validation run (Release Gate item 2's *execution* — this plan removes its blocker).
- New service bits / peer capability advertisement — #241's NOTFOUND demotion already handles snapshot peers; YAGNI until fleet data says otherwise.
- Height-index writes for non-connected blocks (deliberate: fallback lookup instead).
- Worker push-notification from the scheduler (the 30s poll already works; YAGNI).

## Ground truth (verified 2026-06-10, post-#270 dinero-main)

- Scheduler: `include/consensus/block_download_scheduler.h` / `src/consensus/block_download_scheduler.cpp`. `ScanForMissingBlocks` (:567-683) queues `start_height..best_height` from the LOCAL TIP upward — pre-base heights are below the tip and never queued (the gap). `IsFullySynchronized` (:438-475). `RequestNextBlock` (:723-756) cursor-scan + `SetRequestSkipPeersLocked(height)` + `send_getdata_callback_(hash, height)`. `Tick` (:160+) honors `defer_check_` (snapshot pending) then `RequestNextBlock` + `TryConnectStoredBlocks`.
- #241 skip-peers: `peer_lacks_body_at_or_below_` map (:112-120), `OnBlockNotFound(hash, peer_key)` (:67-92) records; `SetRequestSkipPeersLocked` (:539-541) excludes peers for heights ≤ their NOTFOUND high-water. Comment says verbatim it covers "AssumeUTXO snapshot peers".
- `OnBlockReceived(block)` (:122-158): `ValidateBlockAgainstHeader` → `StoreBlock(block, stored_pos)` → mark RECEIVED — store-only, no activation. Blocks not in `expected_blocks_` are rejected (routing falls through to chainstate).
- Headers below base guaranteed: LoadSnapshot gate requires base header known (`chainstate_service.cpp:8113-8126`); headers-first syncs from genesis; `HeaderChainSelector::GetHeaderAtHeight` is the #241-fixed linear walk (`src/consensus/header_chain.cpp:350-358`).
- Height index written ONLY by ConnectTip; `BlockAcceptor` hardcodes `apply_canonical_writes=false` (`src/daemon/block_acceptor.cpp:1859`).
- Worker read path (`chainstate_service.cpp:11965-11984`): `chain_db_->getBlockHashByHeight(h)` → `getBlockByHash(hash)` → body-hash check vs canonical hash → engine. Misses count as `blocks_skipped`; 30s poll loop.
- `getBlockByHash` succeeds for scheduler-stored bodies (ChainDB header metadata with flatfile positions written by the store path).
- Daemon wiring: `daemon_app.cpp:5430-5602` OnNewBlock routing (scheduler first, chainstate fallback); `SetDeferCheck` at :2444; `TryDeferredSnapshotBootstrap` periodic at :5275.
- Scheduler unit fixture: `tests/consensus/test_block_download_scheduler.cpp:144-197` — `SelectParams(REGTEST)`, in-memory `HeaderChainSelector` + `BuildLinearHeaders`, scheduler with nullptr storage, `SetSendGetDataCallback` recording requests, `OnHeadersProcessed()` + `Tick()` driving. Cases are numbered (case 9 = #241 blocking-send, case 10 = linear scan).
- Lifecycle status (for wiring conditions): `ChainstateService::GetAssumeUtxoLifecycle()->GetStatus(now)` — `assumeutxo_active`, `history_fully_validated`, `fatal` (PR #269).
- E2E: `tests/integration/test_assumeutxo_replay_e2e.sh` — `deliver_bodies` via `submitblock` (~:155-169) + `--reindex` restart (~:262-264) are the workarounds this plan deletes.

## File map

| File | Action | Responsibility |
|---|---|---|
| `include/consensus/block_download_scheduler.h` | Modify | Backfill queue state, `EnableBackfill/DisableBackfill/GetBackfillProgress`, has-body callback |
| `src/consensus/block_download_scheduler.cpp` | Modify | Queue population, low-priority servicing in Tick, backfill OnBlockReceived routing, NOTFOUND reuse |
| `tests/consensus/test_block_download_scheduler.cpp` | Modify | Cases 11-14: queueing, priority, receive/complete, NOTFOUND skip |
| `src/daemon/services/chainstate_service.cpp` | Modify | Worker height-lookup fallback via header chain |
| `src/daemon/daemon_app.cpp` | Modify | Enable/disable wiring + has-body callback + OnNewBlock backfill routing (if needed) |
| `src/rpc/methods_blockchain_context.cpp` | Modify | `backfill` progress fields in `getsnapshotbootstrapstatus` |
| `tests/integration/test_assumeutxo_replay_e2e.sh` | Modify | DELETE submitblock/--reindex workarounds; scenario A/D ride real backfill; assert progress fields |

Build/test commands per task:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # once; git submodule update --init --recursive first
cmake --build build -j8 --target <target>
ctest --test-dir build -R <Name> --output-on-failure
```

---

### Task 1: Scheduler backfill queue + accounting (TDD)

**Files:**
- Modify: `include/consensus/block_download_scheduler.h` (public API + private state)
- Modify: `src/consensus/block_download_scheduler.cpp`
- Modify: `tests/consensus/test_block_download_scheduler.cpp` (case 11)

- [ ] **Step 1: Read the fixture** (`tests/consensus/test_block_download_scheduler.cpp:1-250`) and the scheduler header fully. Note how cases are structured (numbered functions called from main, exit-nonzero on failure — NOT gtest; match that style exactly).

- [ ] **Step 2: Write failing case 11** (append in the established case style; adapt helper names to the file's actual ones — `BuildLinearHeaders`, the callback setters — quoted from the fixture you read):

```cpp
// Case 11 (assumeutxo backfill): EnableBackfill queues exactly the missing
// pre-base heights, skipping bodies the node already has, and exposes
// progress; backfill never flips IsFullySynchronized.
static bool TestCase11_BackfillQueueing() {
    dcs::HeaderChainSelector selector;
    BuildLinearHeaders(selector, 8);                 // genesis + 8 headers
    dcs::BlockDownloadScheduler scheduler(&selector, nullptr);
    scheduler.SetLocalTipHeight(8);                  // snapshot base = 8 (tip)
    scheduler.OnHeadersProcessed();                  // tip sync: nothing missing

    // Node already has bodies 3 and 5 (e.g. partial prior backfill).
    scheduler.SetHasBlockBodyCallback([](const uint256& /*hash*/, uint32_t height) {
        return height == 3 || height == 5;
    });

    std::vector<uint32_t> requested_heights;
    scheduler.SetSendGetDataCallback([&](const uint256& /*hash*/, uint32_t height) {
        requested_heights.push_back(height);
    });

    if (!scheduler.IsFullySynchronized()) return Fail("pre: should be synced at tip");

    scheduler.EnableBackfill(1, 8);                  // heights 1..base
    auto prog = scheduler.GetBackfillProgress();
    if (!prog.enabled) return Fail("backfill not enabled");
    if (prog.total != 6) return Fail("expected 6 missing (1,2,4,6,7,8)");
    if (prog.completed != 0) return Fail("none completed yet");

    if (!scheduler.IsFullySynchronized()) return Fail("backfill must not flip IsFullySynchronized");

    // Idempotent re-enable must not duplicate.
    scheduler.EnableBackfill(1, 8);
    if (scheduler.GetBackfillProgress().total != 6) return Fail("re-enable duplicated queue");

    scheduler.DisableBackfill();
    if (scheduler.GetBackfillProgress().enabled) return Fail("disable failed");
    return true;
}
```

- [ ] **Step 3: RED** — build the test target (find its name: `grep -n block_download_scheduler tests/CMakeLists.txt`), compile fails (methods missing). Quote.

- [ ] **Step 4: Implement.** Header additions (inside the class, near the existing callback setters and #241 state):

```cpp
    // ── AssumeUTXO pre-base body backfill (spec Release Gate item 2) ──
    // Separate low-priority queue: never touches missing_blocks_ or
    // IsFullySynchronized (fast-bootstrap nodes stay "synced" while history
    // backfills). Serviced by Tick() only when tip sync has no pending work.
    struct BackfillProgress {
        bool enabled = false;
        uint32_t start_height = 0;
        uint32_t end_height = 0;
        uint64_t total = 0;        // bodies missing at Enable time
        uint64_t completed = 0;    // bodies received+stored since Enable
        uint64_t in_flight = 0;
    };
    // has_body(hash, height): true if the body is already stored locally.
    void SetHasBlockBodyCallback(std::function<bool(const uint256&, uint32_t)> cb);
    // Queue the canonical heights [start, end] whose bodies are missing.
    // Idempotent while enabled with the same range.
    void EnableBackfill(uint32_t start_height, uint32_t end_height);
    void DisableBackfill();
    BackfillProgress GetBackfillProgress() const;
```

Private state (next to `missing_blocks_`):

```cpp
    // Backfill queue entries reuse BlockFetchState (hash, height, status,
    // request_time) but live in their own vector with their own cursor.
    std::vector<BlockFetchState> backfill_blocks_;
    size_t next_backfill_idx_ = 0;
    std::unordered_set<uint256> backfill_expected_;   // routing in OnBlockReceived
    BackfillProgress backfill_progress_;
    std::function<bool(const uint256&, uint32_t)> has_block_body_;
```

Implementation (cpp; all under `mutex_`, matching the class's locking style — read how existing public methods lock):

```cpp
void BlockDownloadScheduler::SetHasBlockBodyCallback(
        std::function<bool(const uint256&, uint32_t)> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    has_block_body_ = std::move(cb);
}

void BlockDownloadScheduler::EnableBackfill(uint32_t start_height, uint32_t end_height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backfill_progress_.enabled &&
        backfill_progress_.start_height == start_height &&
        backfill_progress_.end_height == end_height) {
        return;  // idempotent
    }
    backfill_blocks_.clear();
    backfill_expected_.clear();
    next_backfill_idx_ = 0;
    backfill_progress_ = BackfillProgress{};
    backfill_progress_.enabled = true;
    backfill_progress_.start_height = start_height;
    backfill_progress_.end_height = end_height;

    for (uint32_t h = start_height; h <= end_height; ++h) {
        const HeaderIndexEntry* entry = header_chain_ ? header_chain_->GetHeaderAtHeight(h)
                                                      : nullptr;
        if (!entry) continue;  // header gap: nothing to request (headers-first owns it)
        if (has_block_body_ && has_block_body_(entry->hash, h)) continue;
        backfill_blocks_.push_back(BlockFetchState(entry->hash, h));
        backfill_expected_.insert(entry->hash);
    }
    backfill_progress_.total = backfill_blocks_.size();
}

void BlockDownloadScheduler::DisableBackfill() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& fs : backfill_blocks_) {
        in_flight_blocks_.erase(fs.block_hash);   // release any in-flight accounting
    }
    backfill_blocks_.clear();
    backfill_expected_.clear();
    next_backfill_idx_ = 0;
    backfill_progress_ = BackfillProgress{};
}

BlockDownloadScheduler::BackfillProgress
BlockDownloadScheduler::GetBackfillProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backfill_progress_;
}
```

(Adapt: `BlockFetchState`'s real constructor; `header_chain_` member name; whether `GetHeaderAtHeight` walks from best — it does, #241-fixed linear; for a 33k-range Enable that's 33k × O(1)-amortized via one backward walk — if `GetHeaderAtHeight` per call walks from best (O(n) each → O(n²) total, the EXACT #241 bug class), instead do ONE backward walk from `GetHeaderAtHeight(end_height)` following parent pointers down to start_height, collecting entries — read header_chain.cpp:350-358 and DO THE SINGLE-WALK VERSION if per-call is linear. State which you implemented.)

- [ ] **Step 5: GREEN** — case 11 passes; ALL existing cases still pass (the binary runs every case). Quote the run.

- [ ] **Step 6: Commit** — `git add -A && git commit -m "consensus(scheduler): assumeutxo backfill queue with own accounting"`

---

### Task 2: Backfill servicing — low priority, skip-peers reuse, receive routing (TDD)

**Files:**
- Modify: `src/consensus/block_download_scheduler.cpp` (Tick servicing + OnBlockReceived + OnBlockNotFound)
- Modify: `tests/consensus/test_block_download_scheduler.cpp` (cases 12-14)

- [ ] **Step 1: Write failing cases** (style per the file):

```cpp
// Case 12: backfill is serviced ONLY when tip sync is idle, and respects
// the in-flight cap.
static bool TestCase12_BackfillPriority() {
    dcs::HeaderChainSelector selector;
    BuildLinearHeaders(selector, 8);
    dcs::BlockDownloadScheduler scheduler(&selector, nullptr);

    std::vector<uint32_t> requested;
    scheduler.SetSendGetDataCallback([&](const uint256&, uint32_t h) { requested.push_back(h); });

    // Tip sync busy: local tip 4, headers to 8 → missing 5..8 queued.
    scheduler.SetLocalTipHeight(4);
    scheduler.OnHeadersProcessed();
    scheduler.EnableBackfill(1, 4);

    scheduler.Tick();
    // Every request so far must be tip-sync (heights >4); no backfill yet.
    for (uint32_t h : requested) {
        if (h <= 4) return Fail("backfill serviced while tip sync pending");
    }

    // Drain tip sync: mark all tip blocks received (use the fixture's
    // existing receive helper — build Block bodies for headers 5..8 the way
    // earlier cases do) then Tick again: now backfill heights appear.
    /* adapt: receive 5..8 via scheduler.OnBlockReceived(MakeBlockForHeader(...)) */
    requested.clear();
    scheduler.Tick();
    bool any_backfill = false;
    for (uint32_t h : requested) {
        if (h <= 4) any_backfill = true;
    }
    if (!any_backfill) return Fail("backfill not serviced after tip idle");
    return true;
}

// Case 13: a received backfill block is stored-only: progress increments,
// it never enters connect bookkeeping, and completion empties the queue.
static bool TestCase13_BackfillReceive() {
    /* setup as case 11 with tip at 4=base, EnableBackfill(1,4), Tick to request */
    /* deliver each backfill block via OnBlockReceived(MakeBlockForHeader(h)) */
    /* assert: returns true; GetBackfillProgress().completed increments to total;
       IsFullySynchronized() stays true throughout;
       after all done: progress.enabled stays true, in_flight 0, total==completed */
}

// Case 14: NOTFOUND from a snapshot peer demotes it for backfill heights too
// (reuses peer_lacks_body_at_or_below_): after OnBlockNotFound(hash@3, "peerA"),
// the skip set for a height-2 backfill request contains peerA.
static bool TestCase14_BackfillNotFoundSkip() {
    /* request a backfill block at height 3; OnBlockNotFound(that hash, "peerA");
       drive the next backfill request at height <=3 and assert peerA is in the
       skip set (use whatever the fixture/case 9 uses to observe skip peers —
       read how case 9/#241 tests assert skip behavior and mirror it) */
}
```

Fill the `/* adapt */` bodies by READING the existing cases that build blocks for headers and observe skip-peers (case 9 and the fork cases do both) — mirror their helpers exactly. The assertions stated in comments are the contract; do not weaken them.

- [ ] **Step 2: RED** — cases fail (servicing/routing not implemented). Quote each failure.

- [ ] **Step 3: Implement.**

(a) **Tick servicing** — in `Tick()` after the existing tip-sync work (after `RequestNextBlock()`/`TryConnectStoredBlocks()` — read the exact structure), add:

```cpp
    // Backfill is strictly lower priority: only when tip sync has nothing
    // MISSING or REQUESTED do we spend request slots on history.
    ServiceBackfillLocked();
```

with:

```cpp
void BlockDownloadScheduler::ServiceBackfillLocked() {
    if (!backfill_progress_.enabled || backfill_blocks_.empty()) return;
    if (defer_check_ && defer_check_()) return;

    // Tip sync busy? (any MISSING/REQUESTED tip block) → yield.
    for (const auto& fs : missing_blocks_) {
        if (fs.status == BlockFetchStatus::MISSING ||
            fs.status == BlockFetchStatus::REQUESTED) {
            return;
        }
    }

    // Reuse the global in-flight cap.
    const size_t max_in_flight = GetMaxInFlight();   // adapt to real accessor
    size_t fired = 0;
    const size_t n = backfill_blocks_.size();
    for (size_t i = 0; i < n && in_flight_blocks_.size() < max_in_flight; ++i) {
        size_t idx = (next_backfill_idx_ + i) % n;
        auto& fs = backfill_blocks_[idx];
        if (fs.status != BlockFetchStatus::MISSING) continue;
        // Stale-request retry: reuse the same timeout the tip path uses
        // (read how missing_blocks_ REQUESTED entries get retried and mirror;
        // if retries are driven elsewhere, mirror that site for backfill too).
        fs.status = BlockFetchStatus::REQUESTED;
        fs.request_time = std::chrono::steady_clock::now();
        in_flight_blocks_.insert(fs.block_hash);
        SetRequestSkipPeersLocked(fs.height);        // #241 reuse, verbatim
        backfill_progress_.in_flight++;
        send_getdata_callback_(fs.block_hash, fs.height);
        fired++;
        next_backfill_idx_ = (idx + 1) % n;
    }
    (void)fired;
}
```

CRITICAL ADAPTATION: the send callback is invoked under `mutex_` in this sketch — #241's whole point (commit a4176e3af) was sends OUTSIDE the lock via `StageGetdataLocked` + deferred dispatch. READ how `RequestNextBlock`/Tick actually stage-and-dispatch on this branch and ROUTE BACKFILL SENDS THROUGH THE SAME STAGING MECHANISM. Do not reintroduce the deadlock class #241 fixed. Quote the staging call you used.

(b) **OnBlockReceived routing** — at its top, after computing `block_hash` (read the real flow): if the hash is in `backfill_expected_`, run the same `ValidateBlockAgainstHeader` + `StoreBlock` path, then mark the backfill entry RECEIVED, erase from `in_flight_blocks_`/`backfill_expected_`, `backfill_progress_.completed++`, `backfill_progress_.in_flight--`, and return true WITHOUT entering tip-connect bookkeeping (`received_blocks_`/TryConnectStoredBlocks inputs). Share code with the tip path where natural (a small `StoreVerifiedBlockLocked` helper) rather than duplicating.

(c) **OnBlockNotFound**: extend its `missing_blocks_` search to ALSO search `backfill_blocks_` so the #241 high-water demotion works for backfill heights (the map itself is shared — only the hash→height lookup needs the second vector). Also flip that entry back to MISSING (so another peer is tried) and decrement `in_flight`/erase from `in_flight_blocks_` — mirror exactly what the tip path does on NOTFOUND (read it first).

(d) **Stale-request retry**: find where REQUESTED tip entries time out back to MISSING (#216 lineage) and apply the same timeout sweep to `backfill_blocks_` in the same place.

- [ ] **Step 4: GREEN** — cases 11-14 pass + ALL prior cases. Quote.

- [ ] **Step 5: NEUTER** — disable the yield check in `ServiceBackfillLocked` (service backfill even when tip busy) → case 12 MUST fail. Restore, green. Quote.

- [ ] **Step 6: Commit** — `git add -A && git commit -m "consensus(scheduler): service backfill at low priority with #241 skip-peer + staging reuse"`

---

### Task 3: Worker header-chain fallback lookup

**Files:**
- Modify: `src/daemon/services/chainstate_service.cpp` (worker loop ~11960-11990)

- [ ] **Step 1: Implement the fallback.** In `BackgroundValidationWorker`'s per-height body lookup, replace the bare height-index call:

```cpp
                auto hash_result = chain_db_->getBlockHashByHeight(height);
                uint256 canonical_hash;
                bool have_hash = false;
                if (hash_result.ok()) {
                    canonical_hash = hash_result.value();
                    have_hash = true;
                } else if (header_chain_selector_) {
                    // Backfilled bodies are stored WITHOUT height-index writes
                    // (only ConnectTip writes the canonical index). The header
                    // chain is canonical below the snapshot base by
                    // construction — use it as the lookup of record here. The
                    // body-hash check below still guards integrity.
                    const auto* entry = header_chain_selector_->GetHeaderAtHeight(height);
                    if (entry) {
                        canonical_hash = entry->hash;
                        have_hash = true;
                    }
                }
                if (!have_hash) { blocks_skipped++; continue; }
                auto block_result = getBlockByHash(canonical_hash);
```

…and update the subsequent body-hash check and `ConnectAndAdvance` call sites to use `canonical_hash` (read the current code: it uses `hash_result.value()` in both places). ADAPT: `header_chain_selector_` member name + null-guard idiom; `GetHeaderAtHeight` per-call cost — this runs once per height per pass; with the #241 linear-walk implementation that's the same O(n)-per-pass class the scan already has — if it is O(n) PER CALL from best-header, hoist a single backward walk... simplest correct option: try the index first (hits after any reindex/connect), fall back per-height, and note the cost is bounded by passes that have missing indices only.

CRITICAL HONESTY CHECK: `GetHeaderAtHeight` walks the BEST header chain. Below the snapshot base this is canonical by construction (LoadSnapshot verified the base header is ON the header chain, and the base's ancestors are unique). State this argument in the code comment.

- [ ] **Step 2: Verification reality.** No service fixture; gates are: full dinerod build clean + all four unit suites green + the e2e (Task 5 makes it the real gate; until then the EXISTING e2e still passes because submitblock+reindex populate the height index — the fallback is additive). `git diff --check` clean.

- [ ] **Step 3: Commit** — `git add -A && git commit -m "daemon(assumeutxo): validation worker falls back to header-chain lookup for unindexed backfilled bodies"`

---

### Task 4: Daemon wiring — enable/disable + has-body callback + RPC progress

**Files:**
- Modify: `src/daemon/daemon_app.cpp` (wiring near SetDeferCheck :2444 and the post-LoadSnapshot/TryDeferredSnapshotBootstrap sites :5275)
- Modify: `src/daemon/services/chainstate_service.cpp` / `.h` (small accessors if needed)
- Modify: `src/rpc/methods_blockchain_context.cpp` (progress fields)

- [ ] **Step 1: has-body callback** (where the scheduler's other callbacks are wired in daemon_app.cpp — read that block):

```cpp
    block_download->SetHasBlockBodyCallback(
        [chainstate](const uint256& hash, uint32_t /*height*/) -> bool {
            return chainstate && chainstate->hasBlockByHash(hash);
        });
```

(ADAPT: the real cheap has-body accessor — grep `hasBlockByHash` / `hasBlock` on ChainstateService; it must check BODY presence (flatfile metadata), not just header. If only `getBlockByHash` exists, a `hasBlockBody(hash)` thin accessor on ChainstateService is in scope — declare next to getBlockByHash, implement as a metadata-presence check WITHOUT reading the body; read how getBlockByHash resolves positions and check the same metadata.)

- [ ] **Step 2: Enable/disable conditions.** A small periodic hook where `TryDeferredSnapshotBootstrap` is already called periodically (:5275 region — same cadence is fine):

```cpp
    // AssumeUTXO body backfill: while the lifecycle is validating history,
    // keep the backfill queue armed for heights 1..base; retire it once the
    // history is fully validated (or on fatal/reset).
    if (chainstate && block_download) {
        if (auto* lc = chainstate->GetAssumeUtxoLifecycle()) {
            const auto st = lc->GetStatus(std::chrono::steady_clock::now());
            const bool validating =
                st.assumeutxo_active && !st.history_fully_validated && !st.fatal;
            if (validating && st.snapshot_base_height > 0) {
                block_download->EnableBackfill(1, st.snapshot_base_height);
            } else {
                block_download->DisableBackfill();
            }
        }
    }
```

EnableBackfill's idempotence makes the periodic call cheap (same-range early return). PLACEMENT ADAPTATION: read what `this`/captures are available at the chosen site; if the periodic site lacks the needed pointers, wire it where SetDeferCheck is set up and drive from the existing periodic lambda. DisableBackfill on every non-validating state covers retirement, fatal, AND reset without extra plumbing.

- [ ] **Step 3: RPC fields.** In `buildSnapshotBootstrapDiagnostics` (methods_blockchain_context.cpp — the lifecycle block from #269/#270): ChainstateService doesn't own the scheduler, the daemon does — check whether the RPC `ExecutionContext` can reach the scheduler (grep how other RPCs access `block_download` / daemon context). If reachable:

```cpp
    if (auto* sched = /* resolve scheduler from ctx — adapt */) {
        const auto bp = sched->GetBackfillProgress();
        snapshot["backfill_enabled"] = bp.enabled;
        if (bp.enabled) {
            snapshot["backfill_total"] = static_cast<Json::UInt64>(bp.total);
            snapshot["backfill_completed"] = static_cast<Json::UInt64>(bp.completed);
            snapshot["backfill_in_flight"] = static_cast<Json::UInt64>(bp.in_flight);
        }
    }
```

If the scheduler is NOT reachable from RPC context without invasive plumbing, expose the progress through ChainstateService instead (daemon pushes `GetBackfillProgress()` into a chainstate setter on the same periodic hook) — choose the LESS invasive route and document which.

- [ ] **Step 4: Verify** — full dinerod build clean; four unit suites green; existing e2e still green (workarounds still in place). Commit: `git add -A && git commit -m "daemon(assumeutxo): arm body backfill while validating; expose progress over RPC"`

---

### Task 5: The keystone — e2e rides real backfill (delete the workarounds)

**Files:**
- Modify: `tests/integration/test_assumeutxo_replay_e2e.sh`

- [ ] **Step 1: Delete the crutches.** Remove `deliver_bodies` (submitblock loop, ~:155-169) and the `--reindex` restart (~:262-264) from scenario A's flow (and scenario D's heal leg if it reuses them — read the script). Scenario A becomes: load snapshot on the consumer, connect to the source peer, wait for `history_fully_validated` — bodies must arrive via REAL backfill getdata.

- [ ] **Step 2: Assert the new surface.** While waiting in scenario A, also poll once for `backfill_enabled == true` and later assert `backfill_completed == backfill_total` (or that the keys exist and progressed — adapt to the exact JSON shape from Task 4). On the fully-validated node (post-retirement), assert `backfill_enabled == false` (disabled after retirement).

- [ ] **Step 3: Timing.** Backfill of ~102 regtest blocks over localhost should take seconds; keep scenario A's wait at 240s. If it stalls, the failure dump (daemon log tail) is the debugging entry point — look for backfill getdata in the source's log and NOTFOUND demotions.

- [ ] **Step 4: Run** — `ctest --test-dir build -R AssumeUtxoReplayE2E --output-on-failure` → ALL SCENARIOS PASSED through real P2P backfill.

- [ ] **Step 5: KEYSTONE NEUTER** — comment out the `EnableBackfill` call in daemon_app.cpp, rebuild dinerod, re-run scenario A region → MUST FAIL (stalls: no bodies ever requested; the failure dump should show stall state with backfill_enabled false/absent). Quote. Restore via git checkout, rebuild, full e2e green, git status clean.

- [ ] **Step 6: Commit** — `git add tests/integration/test_assumeutxo_replay_e2e.sh && git commit -m "test(assumeutxo): e2e rides real P2P body backfill — submitblock/--reindex crutches removed"`

---

### Task 6: Final verification + release-gate accounting

- [ ] **Step 1:** Full build + full ctest (background; gate: no NEW failures vs the attributed pre-existing set from #269/#270 — 19 failures expected: shielded-fee cluster, CSN soaks, P2PHandshake, InvalidityImport, ReleaseSuite env, ColdStart harness defect, ConsensusFuzzer; IBDTorture passes when idle and path lacks "fatal").
- [ ] **Step 2:** Stale-object touch check on the scheduler test target ONLY — NEVER concurrently with the background full build (documented race: archive rewrite). Run it BEFORE launching the full suite, or after it completes.
- [ ] **Step 3:** Release Gate accounting for the PR: item 2's blocker is now removed — a fresh node with a snapshot + healthy peers reaches `fully_validated` unattended (e2e-proven); the fleet run itself remains an operational task. Items 1/3 already satisfied; 4/5 release-process.
- [ ] **Step 4:** Hygiene (`git diff --check`, no TODOs, accurate messages), final whole-implementation reviewer (read-only constraints if suite is running), then superpowers:finishing-a-development-branch. No push without the human's go.

## Follow-ups that remain after this plan

- Mainnet fleet from-genesis validation run (Release Gate item 2 execution — now unblocked).
- Wallet-send safe-mode gating (spec Fatal 3/4 completion).
- Backfill bandwidth shaping for mainnet scale (current: yields to tip sync + global in-flight cap; revisit with fleet data — YAGNI now).
- The pre-existing dinero-main issues (shielded fee, harness defects) — still unfiled.

## Register addition (Task 5 review finding 1 — mandatory)

- **Connect backfilled history / ChainDB catch-up so legacy assumeutxo mode exits.** Backfilled bodies are stored, never connected: the canonical height→hash index stays missing for 1..base, ChainDB never reaches base, and the legacy operational mode (and RPC `assumeutxo_active`, via the #269 defensive OR) never exits on the unattended path — deviating from spec Required Test 4's `assumeutxo_active == false` (definition at spec :269: "true while the node still depends on assumed state"; after a matched replay the state is proven, not assumed). Safe-direction deviation (over-claims dependence). The exit gate at `chainstate_service.cpp:12371` is one-shot inside `OnBackgroundValidationComplete` and also needs a re-fire path (ConnectTip hook or periodic check) once ChainDB can catch up.
