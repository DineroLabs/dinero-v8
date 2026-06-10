# AssumeUTXO Mode Exit (Promotion) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After the validation worker's final complete replay pass proves blocks 1..base, promote the proven history into ChainDB (height index, last-1024 undo, bulk coin reconcile, tip-anchored markers, tip at base) so the existing exit gate fires and legacy assumeutxo mode exits — restoring spec Required Test 4's `assumeutxo_active == false` (issue #280) — and add the spec-mandated fork-below-base fatal guard (today a below-base reorg would crash `DisconnectTip` on missing undo).

**Architecture:** A new `ChainstateService::PromoteValidatedHistory(...)` runs inside the worker immediately after `OnReplayComplete` succeeds, sourcing everything from state already in hand: per-block `putHeightIndex` for 1..base; undo flatfile + locator for only the last `kStartupUndoAuditWindow` (1024) heights (captured during the final pass — the audit walks back exactly that far); ONE bulk coin-CF reconcile from the engine's proven UTXO set (end-state identical to per-block churn; ~98k coins vs 33k blocks of mutations); tip-anchored writes once at base (ForestTipMarker, ShieldedTipMarker + shielded frontier/nullifiers/anchors from the engine's state, journal row if `atomic_persist`); finally a durable `setTip(base)`. Promotion is idempotent (worker re-runs replay+promotion on restart if the gate didn't fire). The fork guard sits immediately after `FindFork` in `ActivateBestChain`: fork-point below the base while assumeutxo state exists → safe mode + persisted lifecycle fatal, never a disconnect attempt.

**Tech Stack:** C++17, CMake, GoogleTest + the bespoke scheduler harness, regtest shell e2e (`AssumeUtxoReplayE2E` gains the restored spec-Test-4 assertions).

**Branch:** `worktree-assumeutxo-mode-exit` off post-#282 `dinero-main` (05d791dbc). First commit: this plan.

---

## Scope

**In scope:** the promotion path, the fork-below-base fatal guard, worker/exit-gate integration, e2e assertions restoring spec Test 4 (`assumeutxo_active == false`, height index present, restart-clean startup audits).

**Explicit non-goals:**
- TxIndex backfill for 1..base — `getrawtransaction` stays degraded for pre-base txids (register as documented limitation; mempool/consensus don't read txindex). A follow-up can rebuild via the existing `RebuildTxIndex` (`chainstate_service.cpp:7719`).
- Per-block utreexo checkpoints below base (LoadSnapshot already wrote the base checkpoint at :8531; startup restores AT tip).
- The mainnet fleet validation run / release-process items.

## Ground truth (verified 2026-06-10/11, post-#282 dinero-main; line refs from that tree)

- **Exit gate** `chainstate_service.cpp:12358-12374`: `chaindb_caught_up` = `chain_db_->getTip().height >= assumeutxo_base_height_`; with lifecycle FullyValidated → `ClearAssumeUTXOState(true)`. Promotion makes the EXISTING gate fire — no gate changes.
- **The tolerance being retired**: `IsCanonicalStateAligned` `:9265-9274` returns true for chaindb-behind ONLY while `assumeutxo_active_`. After promotion+clear, alignment must hold on its own — which it does once tip==base and the tip-anchored markers match.
- **Audit window**: `kStartupUndoAuditWindow = 1024` (`:2447`); `VerifyActiveChainUndoCoverage` (`:4425-4540`) walks back from the persisted tip exactly that far; within the window `BLOCK_HAVE_UNDO` + `undo_size > 0` + readable undo are unconditional (`CheckBlockDisconnectMaterialDurable :4038-4048`; also demands the utreexo delta sidecar `UD:<hash>` when utreexo-active, `:4106-4125`). Below the window: not audited.
- **Coin CF**: LoadSnapshot populates ONLY the consensus set (`BulkLoad :8454`); ChainDB's coin CF is NOT loaded. Readers: mempool input validation (`mempool.cpp:100-300` via `getCoinWithConfidentialFallback`) and `gettxout` (`grpc/blockchain_service.cpp:248,313`). Promotion must leave the coin CF equal to the engine's proven base-state. (Task 2 Step 1 verifies the fallback semantics — if a consensus-set fallback already serves these readers, the bulk reconcile is still required for post-clear consistency, but the verification decides whether a pre-promotion bug also exists to note.)
- **Tip-anchored startup reads**: forest checkpoint AT tip (`RestoreUtreexoCheckpoint :9190`, called `:7364`); `IsCanonicalStateAligned` requires ShieldedTipMarker matching active tip (`:9281-9304`); `VerifyConsensusJournalAtActiveTip :1251-1337` (journal row keyed by tip, gated on `atomic_persist`, gate `:1277`).
- **ConnectTip per-block batch** (the maximal reference): `:10658-11104` — coins, txindex, utreexo checkpoint+marker, undo locator (`updateUndoLocator :10783`), shielded frontier/anchors/marker/nullifiers, setTip, putHeightIndex, journal, UD sidecar (`:11103`). `CommitConnectedBlockBookkeeping :11686-11794` is the stateless-replay variant.
- **Reindexer precedent** (`src/consensus/reindexer.cpp:2800-2879`): rebuilds ONLY the coin CF via per-block delete/put + `writeBatch` — confirms coin-CF-only rebuilds are an accepted pattern; promotion's bulk reconcile is the same end-state computed differently.
- **Fork guard placement**: `ActivateBestChain`, `FindFork` at `:6773`, disconnect path built `:6791-6797`. Guard goes between them. `EnterSafeMode` callable there (used at `:7404` etc.). Lifecycle fatal persistence via `assumeutxo_lifecycle_->OnReplayComplete(true,false,...)` (the established convergence idiom from the worker's poison path `:12149` region) — `EnsureAssumeUtxoLifecycle()` first.
- **Worker final pass**: `BackgroundValidationWorker` — engine `ConnectAndAdvance` per height (undo populated by ConnectBlock, currently discarded inside the engine, `assumeutxo_replay.cpp:66-81`); completion block calls `OnReplayComplete(true, match, ...)` then `OnBackgroundValidationComplete(true,"")` whose success branch evaluates the exit gate. Promotion must run between a successful `OnReplayComplete` and `OnBackgroundValidationComplete`.
- **Engine state at completion**: full proven UTXO map (`GetUTXOs()` — wait, engine wraps the set privately; it exposes `UtxoCount/RecordsDigestHex/UtreexoRootHex`; Task 1 adds the accessors promotion needs), forest, shielded trio.
- **getTip/setTip**: `chain_db.cpp:783-837` (KEY_TIP in meta CF; `setTip` batchable, `sync=true` when direct).
- ChainDB write idioms: `ChainWriteToken token` + `rocksdb::WriteBatch` + `writeBatch` (reindexer `:2868` shows the pattern outside ConnectTip).

## File map

| File | Action | Responsibility |
|---|---|---|
| `include/daemon/services/assumeutxo_replay.h` / `src/.../assumeutxo_replay.cpp` | Modify | Expose promotion inputs: undo capture for a tail window, set/forest/shielded accessors |
| `src/daemon/services/chainstate_service.cpp` | Modify | `PromoteValidatedHistory` (new private method), worker integration, fork-below-base guard |
| `include/daemon/services/chainstate_service.h` | Modify | Declaration + small members |
| `tests/daemon/test_assumeutxo_replay.cpp` | Modify | Undo-capture + accessor unit tests |
| `tests/integration/test_assumeutxo_replay_e2e.sh` | Modify | Scenario A restores spec-Test-4 assertions; restart-clean audit leg |
| `docs/design/assumeutxo-fatal-state-machine.md` | none | (spec unchanged; Test 4 becomes fully satisfied) |

Build/test commands per task:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release    # once; submodules first
cmake --build build -j8 --target <target>
ctest --test-dir build -R <Name> --output-on-failure
```

---

### Task 1: Engine exposes promotion inputs (TDD)

**Files:**
- Modify: `include/daemon/services/assumeutxo_replay.h`, `src/daemon/services/assumeutxo_replay.cpp`
- Modify: `tests/daemon/test_assumeutxo_replay.cpp`

The worker needs, at completion: (a) per-block undo for the LAST `tail_window` heights; (b) read access to the proven UTXO map; (c) the forest + shielded state for tip-anchored writes.

- [ ] **Step 1: Write the failing tests** (append to the existing suite; reuse its `BuildDeterministicChain` builder and style):

```cpp
// Promotion inputs: the engine captures undo for the requested tail window only.
TEST(AssumeUtxoReplay, CapturesUndoTailWindow) {
    const auto chain = BuildDeterministicChain(10);
    assumeutxo::AssumeUtxoReplayEngine engine;
    engine.SetUndoTailWindow(3);   // capture undo for the last 3 connected heights
    std::string err;
    // (genesis pre-applied per engine contract; replay 1..9)
    for (uint32_t h = 1; h < chain.size(); ++h) {
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[h], h, chain[h].GetHash(), err)) << err;
    }
    const auto& tail = engine.UndoTail();          // ordered ascending by height
    ASSERT_EQ(tail.size(), 3u);
    EXPECT_EQ(tail.front().height, 7u);
    EXPECT_EQ(tail.back().height, 9u);
    // Each captured undo must be non-trivial for blocks that spend (our builder's
    // coinbase-only blocks have empty spent sets — assert the struct is present
    // and heights are right; spend-bearing undo content is e2e territory).
}

// Promotion inputs: proven-set access for the bulk coin reconcile.
TEST(AssumeUtxoReplay, ExposesProvenSetAndStateRefs) {
    const auto chain = BuildDeterministicChain(5);
    assumeutxo::AssumeUtxoReplayEngine engine;
    std::string err;
    for (uint32_t h = 1; h < chain.size(); ++h) {
        ASSERT_TRUE(engine.ConnectAndAdvance(chain[h], h, chain[h].GetHash(), err)) << err;
    }
    EXPECT_EQ(engine.ProvenUtxos().size(), engine.UtxoCount());
    EXPECT_NE(engine.Forest(), nullptr);
    EXPECT_NE(engine.ShieldedTree(), nullptr);
    EXPECT_NE(engine.ShieldedNullifiers(), nullptr);
    EXPECT_NE(engine.ShieldedAnchors(), nullptr);
}
```

- [ ] **Step 2: RED** — build `test_assumeutxo_replay` → compile failure (methods missing). Quote.

- [ ] **Step 3: Implement.** Header additions (public, after the existing accessors):

```cpp
    struct CapturedUndo {
        uint32_t height = 0;
        uint256 block_hash;
        consensus::BlockUndo undo;
    };
    // Capture per-block undo for the LAST `window` connected heights (ring
    // semantics: older entries are dropped as new heights connect). 0 = off.
    // Sized for the startup undo-audit window (kStartupUndoAuditWindow=1024):
    // only the audited tail needs durable undo; below-base disconnects are
    // fatal-guarded so deeper undo is never read.
    void SetUndoTailWindow(uint32_t window);
    const std::deque<CapturedUndo>& UndoTail() const { return undo_tail_; }

    // Promotion inputs (proven state at the base after a complete replay):
    const std::unordered_map<consensus::OutPoint, consensus::UTXOEntry>& ProvenUtxos() const;
    const consensus::UtreexoForest* Forest() const;          // adapt return type to GetForest()'s real type
    const consensus::shielded::CommitmentTree* ShieldedTree() const;
    const consensus::shielded::NullifierSet* ShieldedNullifiers() const;
    const consensus::shielded::AnchorHistory* ShieldedAnchors() const;
```

cpp: in `ConnectAndAdvance`, after a successful `ConnectBlock` (the local `undo` is fully populated there — currently discarded):

```cpp
    if (undo_tail_window_ > 0) {
        undo_tail_.push_back(CapturedUndo{height, block_hash, std::move(undo)});
        while (undo_tail_.size() > undo_tail_window_) undo_tail_.pop_front();
    }
```

Accessors delegate to `set_->GetUTXOs()`, `&set_->GetForest()`, and the engine's shielded members. Members: `uint32_t undo_tail_window_ = 0; std::deque<CapturedUndo> undo_tail_;` + `#include <deque>`. ADAPT: `BlockUndo` movability (if move is deleted, copy — sizes are small per block); the forest/shielded concrete types from the engine's existing members; const-correctness against `GetForest()`'s signature.

- [ ] **Step 4: GREEN** — `ctest --test-dir build -R AssumeUtxoReplay --output-on-failure` (now 5 tests) + the other unit suites stay green.

- [ ] **Step 5: Commit** — `git add -A && git commit -m "daemon(assumeutxo): replay engine captures undo tail and exposes proven state for promotion"`

---

### Task 2: PromoteValidatedHistory

**Files:**
- Modify: `include/daemon/services/chainstate_service.h` (private method decl)
- Modify: `src/daemon/services/chainstate_service.cpp` (implementation near the worker, ~12100 region)

- [ ] **Step 1: Verify the coin-CF fallback question FIRST.** Read `getCoinWithConfidentialFallback` (grep its definition) and determine what the "fallback" consults. Report in the commit body: (a) what mempool/gettxout actually see for pre-base coins on a snapshot node TODAY (if they see nothing, that's a pre-existing bug to note in the PR — promotion fixes it as a side effect); (b) confirm the bulk reconcile target (ChainDB coin CF must equal the engine's proven set after promotion).

- [ ] **Step 2: Implement the method.** Declaration (chainstate_service.h, private, near the worker decls):

```cpp
    // Promote replay-proven history 1..base into ChainDB so the assumeutxo
    // exit gate (chaindb tip >= base) can fire: height index per block, undo
    // for the audited tail window, bulk coin-CF reconcile from the proven set,
    // tip-anchored markers from the engine state, then a durable tip at base.
    // Idempotent: safe to re-run after a crash (worker re-runs replay first).
    // Returns false (with error populated) on any write failure — caller
    // treats that as OPERATIONAL (retry next pass), never as snapshot-fatal.
    bool PromoteValidatedHistory(const assumeutxo::AssumeUtxoReplayEngine& engine,
                                 const std::vector<uint256>& canonical_hashes,
                                 std::string& error);
```

Implementation skeleton (adapt every ChainDB call to the real signatures — the reindexer `:2800-2879` and ConnectTip `:10658-11104` are the idiom sources; use `ChainWriteToken` + batched writes, chunked to bound batch size):

```cpp
bool ChainstateService::PromoteValidatedHistory(
        const assumeutxo::AssumeUtxoReplayEngine& engine,
        const std::vector<uint256>& canonical_hashes,
        std::string& error) {
    if (!chain_db_) { error = "chaindb unavailable"; return false; }
    const uint32_t base = assumeutxo_base_height_;

    logger_->info("[Promotion] promoting replay-proven history 1.." + std::to_string(base) +
                  " into ChainDB (height index, undo tail, coin reconcile, tip)");

    // 1) Height index 1..base (idempotent puts), chunked batches of ~4096.
    //    canonical_hashes[h] is the hash-anchored table the worker already built.
    // 2) Undo tail: for each CapturedUndo in engine.UndoTail():
    //      serialize undo -> block_storage_->writeUndo(hash, bytes)
    //      -> chain_db_->updateUndoLocator(token, hash, file/pos/size, &batch)
    //      (sets BLOCK_HAVE_UNDO; mirror ConnectTip :10768-10814's serialization
    //       + locator idiom EXACTLY — read it first)
    //      ALSO the UD:<hash> utreexo delta sidecar for those heights if
    //      utreexo-active (mirror :11103's write; the engine connected these
    //      blocks so the delta source is the same place ConnectTip gets it —
    //      read what :11103 serializes and from where; if the delta is only
    //      available DURING ConnectBlock, capture it in Task 1's CapturedUndo
    //      alongside the undo — verify and adapt).
    // 3) Bulk coin reconcile: clear-and-rewrite is simplest-correct:
    //      iterate engine.ProvenUtxos() -> putCoin each (chunked batches).
    //      PLUS delete stale rows: if the coin CF can contain coins NOT in the
    //      proven set (genesis-era rows the snapshot-spent), enumerate and
    //      delete them (read what the CF holds post-genesis-init; if a
    //      clearAllCoins/iterator API exists, clear-then-write; else delete by
    //      iterating the CF — find the reindexer's approach to stale rows).
    // 4) Tip-anchored writes at base (single batch):
    //      ForestTipMarker(base hash/height) from engine.Forest()
    //      Shielded frontier/anchors/nullifiers + ShieldedTipMarker from the
    //        engine's shielded state (mirror ConnectTip :10816-10984 — but
    //        ONCE, at base, with the engine's final state)
    //      journal row at base if atomic_persist (mirror :11049 gate)
    // 5) Durable setTip(base hash, base height, work-from-header-chain) LAST.
    //
    // Failure anywhere -> error + false. Partial promotion is safe: tip moves
    // only in step 5, so all earlier writes are invisible-until-tip and the
    // re-run overwrites them idempotently.
    ...
}
```

THE NON-NEGOTIABLE ORDERING INVARIANT (state it in a comment + honor it): `setTip` is the LAST write and the only one that makes the rest observable to startup audits/alignment. Everything before it must be re-runnable.

- [ ] **Step 3: Build clean.** No unit fixture exists for ChainstateService — the e2e (Task 4) is the gate. The commit body carries the fallback-question findings + a written walk of the ordering invariant.

- [ ] **Step 4: Commit** — `git add -A && git commit -m "daemon(assumeutxo): PromoteValidatedHistory — replay-proven history becomes canonical ChainDB state"`

---

### Task 3: Worker integration + fork-below-base fatal guard

**Files:**
- Modify: `src/daemon/services/chainstate_service.cpp` (worker completion block ~12140-12230; ActivateBestChain ~6773)

- [ ] **Step 1: Worker wiring.** At worker start: `replay->SetUndoTailWindow(std::min<uint32_t>(kStartupUndoAuditWindow, target_height));` — wait, the engine is re-emplaced per rescan pass: set the window immediately after every `replay.emplace()` (find both/all emplace sites from #272's loop). In the completion block, AFTER `OnReplayComplete(...)` returns true (FullyValidated) and BEFORE `OnBackgroundValidationComplete(true, "")`:

```cpp
        if (lifecycle_promoted_to_fully_validated) {   // the OnReplayComplete(...) == true result
            std::string promote_err;
            if (!PromoteValidatedHistory(*replay, canonical_hashes_fallback, promote_err)) {
                // OPERATIONAL failure (disk, db): retry next pass — never fatal.
                logger_->warning("[BackgroundValidation] promotion failed (" + promote_err +
                                 ") — will retry; assumeutxo mode remains active");
                OnBackgroundValidationComplete(true, "");   // gate simply won't fire yet
                return;
            }
            logger_->info("[Promotion] complete — ChainDB tip at base; exit gate eligible");
        }
        OnBackgroundValidationComplete(true, "");
```

ADAPT: capture `OnReplayComplete`'s return into a named bool (it currently isn't captured — read the exact completion block from #272/#270); `canonical_hashes_fallback` is the hash-anchored table already built per pass — confirm it's populated (anchor-resolved) on the completing pass and pass it through; if the table was empty because the height INDEX served every lookup (post-promotion re-run), promotion is already done — make the promotion call conditional on `chain_db_ tip < base` (cheap idempotence guard).

- [ ] **Step 2: The fork guard.** In `ActivateBestChain`, immediately after `FindFork` (`:6773`), before the disconnect path is built (`:6791`):

```cpp
    // Spec (assumeutxo-fatal-state-machine.md, Fatal Mismatch Semantics): a
    // higher-work chain diverging BELOW the snapshot base must go fatal — not
    // a silent reorg. Mechanically: undo below the base may not exist
    // (promotion persists only the audited tail), so a disconnect below base
    // would fail anyway; classify it as the proof failure it is.
    if (fork_point && !assumeutxo_base_block_.IsNull() &&
        fork_point->height < static_cast<int>(assumeutxo_base_height_)) {
        const std::string reason =
            "reorg below assumeutxo base: fork height " + std::to_string(fork_point->height) +
            " < base " + std::to_string(assumeutxo_base_height_) +
            " — higher-work divergence below the snapshot base (spec: fatal, not reorg)";
        logger_->error("[ActivateBestChain] FATAL: " + reason);
        EnsureAssumeUtxoLifecycle();
        // ForceFatal (Task 3 Step 3): OnReplayComplete's mismatch branch is
        // refused from FullyValidated (state guard), and a post-promotion
        // below-base fork must STILL persist fatal — ForceFatal is the direct
        // entry usable from any non-fatal state.
        assumeutxo_lifecycle_->ForceFatal(reason);
        EnterSafeMode("assumeutxo fatal: " + reason);
        return;   // adapt to the function's actual error-return idiom
    }
```

CONDITION CHOICE (read + decide + document): gate on `!assumeutxo_base_block_.IsNull()` (base ever set this run) rather than `assumeutxo_active_` — after promotion+clear, `assumeutxo_active_` is false but a below-base fork is STILL spec-fatal (the spec's rule isn't mode-scoped; and undo below the audited tail still doesn't exist). Verify `assumeutxo_base_block_`/`assumeutxo_base_height_` survive `ClearAssumeUTXOState` — they DO NOT (ClearState nulls them, assumeutxo_state.h) — so a cleared node loses the guard. Fix: capture the base height into a new never-cleared member `promoted_base_height_` (set at promotion success; also restored at startup from the lifecycle's persisted base keys when state is fully_validated). Gate the guard on `max(assumeutxo_active_ ? assumeutxo_base_height_ : 0, promoted_base_height_)`. ALSO check OnReplayComplete's state guard: from FullyValidated it returns false without entering fatal (Task-4 #270 guard: mismatch path requires Validating/Stalled) — VERIFY by reading assumeutxo_lifecycle.cpp: the `!commitment_match` branch fires BEFORE the state guard? It fires inside the state-guard-passing region only (state must be Validating/Stalled at :99). From FullyValidated the call is REFUSED — so for a post-promotion below-base fork the lifecycle needs a direct fatal entry: add a small public `AssumeUtxoLifecycle::ForceFatal(const std::string& reason)` (locks, EnterFatal, persists; refuse only if already fatal) with a unit test (TDD: test first — FullyValidated → ForceFatal → FatalMismatch persisted, restart preserves). Use ForceFatal in the guard instead of OnReplayComplete.

- [ ] **Step 3: Lifecycle ForceFatal (TDD)** — test in tests/daemon/test_assumeutxo_lifecycle.cpp:

```cpp
// A below-base divergence discovered AFTER retirement must still go fatal.
TEST_F(AssumeUtxoLifecycleTest, ForceFatalWorksFromFullyValidated) {
    auto lc = MakeLifecycle();
    ASSERT_TRUE(lc->OnSnapshotLoaded(base_block_, kBaseHeight));
    ASSERT_TRUE(lc->OnValidationStarted(t0_));
    lc->OnBlockValidated(kBaseHeight, t0_ + 10s);
    ASSERT_TRUE(lc->OnReplayComplete(true, true, "aa", "aa", 0, t0_ + 20s));
    ASSERT_EQ(lc->GetState(), State::FullyValidated);

    lc->ForceFatal("reorg below assumeutxo base: test");
    EXPECT_EQ(lc->GetState(), State::FatalMismatch);
    EXPECT_NE(lc->GetStatus(t0_).fatal_reason.find("below assumeutxo base"), std::string::npos);
    // Restart preserves (fatal beats the stale fully_validated marker).
    auto lc2 = MakeLifecycle();
    lc2->RestoreFromPersistence(true);
    EXPECT_EQ(lc2->GetState(), State::FatalMismatch);
}
```

RED first (method missing) → implement (locks mu_, no-op if already FatalMismatch, else EnterFatal(reason, now)) → GREEN. Verify Persist() ordering: EnterFatal persists fatal AND the fully_validated key must not shadow it on restore — read RestoreFromPersistence's branch order (fatal_mismatch branch is checked FIRST — confirm) and state it in the test comment.

- [ ] **Step 4: Build + all unit suites green. Commit** — `git add -A && git commit -m "daemon(assumeutxo): promote on completion; below-base fork goes fatal, never reorg"`

---

### Task 4: E2E — restore spec Required Test 4 in full

**Files:**
- Modify: `tests/integration/test_assumeutxo_replay_e2e.sh` (scenario A region)

- [ ] **Step 1: Restore + extend scenario A's assertions.** After the existing `history_fully_validated` wait, add (adapt jq/key shapes to the script's idiom):

```bash
# Spec Required Test 4 restored by promotion (#280): legacy mode exits.
wait_status "$A_RPC" "$A_DIR" '.assumeutxo_active == false' 60 \
    "A-EXIT: assumeutxo_active false after promotion (spec Test 4)"

# Promotion side effects: canonical height index now serves pre-base heights.
H1=$(rpc "$A_RPC" "$A_DIR" getblockhash '[1]')
jq -e '.result | test("^[0-9a-f]{64}$")' <<<"$H1" >/dev/null \
    || fail "A-EXIT: getblockhash 1 not served from promoted height index: $H1"

# Restart-clean: startup audits (strict archival + undo tail + alignment)
# must pass with tip at base and NO assumeutxo tolerance active.
```

The restart leg already exists — extend its post-restart assertion to `'.history_fully_validated == true and .assumeutxo_active == false and .fatal == false'` AND grep the daemon log for the strict-archival pass line ("Strict archival audit passed") + absence of safe-mode entry. (Find the exact log strings from chainstate_service.cpp:3957 region and EnterSafeMode's banner.)

- [ ] **Step 2: Run the full e2e** → ALL SCENARIOS PASSED. The poisoned scenario B and stall scenario D must be UNAFFECTED (they never reach promotion).

- [ ] **Step 3: KEYSTONE NEUTER** — comment out the `PromoteValidatedHistory` call in the worker, rebuild dinerod, re-run scenario A → A-EXIT MUST FAIL (assumeutxo_active stays true; getblockhash 1 fails). Quote. Restore via git checkout, rebuild, full e2e green, clean status.

- [ ] **Step 4: Commit** — `git add tests/integration/test_assumeutxo_replay_e2e.sh && git commit -m "test(assumeutxo): e2e asserts full mode exit — spec Required Test 4 restored (#280)"`

---

### Task 5: Final verification + spec/issue accounting

- [ ] **Step 1:** Stale-object touch check on a unit target FIRST (never concurrent with builds), then full build + full ctest in background. Gate: no NEW failures vs the attributed 19.
- [ ] **Step 2:** Spec sweep: Required Test 4 now FULLY satisfied (all bullets incl. `assumeutxo_active == false`); the Fatal Mismatch "higher-work divergence below base" rule now has an enforcing guard (previously: documented-but-crash). Account for Fatal item 4 (display provisional) — still the remaining partial, unchanged.
- [ ] **Step 3:** Register updates for the PR: txindex degradation for pre-base txids (documented limitation + RebuildTxIndex follow-up pointer); the coin-CF fallback findings from Task 2 Step 1 (pre-existing mempool/gettxout behavior on snapshot nodes — fixed or noted); close-the-loop note for issue #280 ("Fixes #280" in the PR body) and #218 cross-reference (the strict-archival audit now passes naturally on promoted nodes — verify whether #218's pruned-node complaint is also resolved or separate).
- [ ] **Step 4:** Hygiene + final whole-implementation reviewer (read-only constraints if the suite is running) + superpowers:finishing-a-development-branch. No push/merge without the human's go.

## Follow-ups that remain after this plan

- TxIndex backfill for pre-base txids (optional UX; RebuildTxIndex exists).
- Mainnet fleet from-genesis validation run (Release Gate item 2 execution).
- Release notes + signed SHA256SUMS (gates 4/5) — then the stable-default decision.
- Fatal item 4 completion (replay-aware wallet recompute / display gating) — last spec partial.
