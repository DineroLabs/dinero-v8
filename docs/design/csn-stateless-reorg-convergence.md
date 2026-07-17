# CSN / stateless competing-branch reorg — convergence bug (design note)

**Status:** diagnosed 2026-07-17, NOT fixed. Pre-existing on clean
`dinero-main` (reproduces at interval=1, so independent of the forest
checkpoint delta campaign). CI-excluded (label `integration`/`canonicality`).
Consensus-critical stateless reorg path — surface to the consensus/reorg
owner before implementing.

**Failing tests (all one root cause):** `CsnSpendReorgReconciliation` (#348),
`CSNRecoveryShieldedApply` (#374), `CSNShieldedReorgInvertibility` (#375).
Scenario: a CSN/stateless node must follow a **longer competing reorg
branch** after the bridge invalidates a spend block and re-mines a heavier
branch without it. The CSN node livelocks and never converges.

## Confirmed mechanism (interval=1 isolation, pure signal)

Evidence from a kept run (`--utreexo-stateless=1 --utreexo.checkpoint_interval=1`):
- `Failed to restore fork-point checkpoint` = 0 (restore works at interval=1).
- `Restored stateless pre-state to fork checkpoint` = 3769 — the CSN worker
  **does** consume the reorg-reset and rewind the shared forest to the fork
  point, thousands of times.
- `Competing-branch utxoblk … resetting cursor` = 3769 (one rewind per reset).
- Worker validates competing block 121 — then cannot proceed.
- `HEADER CHAIN IS BETTER!` = 180, immediately followed by
  `EARLY RETURN: best_candidate == active_tip_ (122)` = 130.
- ABC-CSN stateless reorg path (`chainstate_service.cpp:7588`) fires **0**
  times.

The livelock, precisely:

1. Competing block new-121 (best-header hash, not on active chain) arrives →
   `OnUtxoBlock` posts `csn_reorg_reset_to` (`daemon_app.cpp:4892`).
2. Worker rewinds the shared forest to fork height 120 and validates new-121
   (forest → new-121 state).
3. **new-122 / new-123 never download.** The scheduler's
   `stateless_reorg_barrier` (`block_download_scheduler.cpp:739-748`) requests
   ONLY the frontier block (new-121) and `break`s — its comment: "descendants
   are only valid after the replacement block … becomes active. Hold the
   frontier here until chainstate catches up."
4. But new-121 can never "become active": at height 121 it has **less**
   cumulative work than the old tip at 122, so `best_candidate` stays old-122
   and `ActivateBestChain` early-returns without reorging. The reorg to the
   better branch requires new-123 (higher work than old-122), which step 3
   refuses to download.
5. new-121 re-arrives (scheduler re-requests the stuck frontier) → back to
   step 1. `IsBlockConnected(new-121)` is false (a block is only CONNECTED
   once its hash is on the active chain — post-reorg), so the reset re-fires
   forever.

Chicken-and-egg: the barrier withholds the very blocks needed to make the
reorg win.

## Why it is not a minimal patch — two forest owners

Loosening the barrier to download the whole competing branch is necessary but
NOT sufficient, and exposes the real design conflict:

- The **CSN worker** owns the forest during validation: rewind to fork →
  sequential replay of new-121, new-122, new-123 (via `ValidateUtreexoProof` /
  `ReplayBlock` → `ApplyAccumulatorDelta`). After it validates new-123 the
  shared forest is at new-123 while `active_tip_` is still old-122.
- Each validated block is `ProcessIncomingBlockHex`'d → `AddCandidate`. Once
  new-123 is a candidate with higher work, `ActivateBestChain` sets
  `needs_activation=true` (traced: `chainstate_service.cpp:7405-7412`) and
  proceeds into the **ABC-CSN stateless reorg path (7588), which ALSO rewinds
  the forest to the fork and replays** — double-managing the forest the worker
  already advanced, in the forest=new-123 / active=old-122 window.

So a correct fix must decide **who owns the forest during a stateless
competing-branch reorg** and make the other path defer. That is an
architecture decision on consensus-critical code, not a local tweak.

## Candidate approaches (for the owner to choose)

1. **Worker-owned reorg, ActivateBestChain pointer-only.** Loosen the barrier
   to download the full competing branch; the worker rewinds once and replays
   the branch forward; `ActivateBestChain`, on detecting the best candidate is
   a competing tip whose forest is already advanced, moves the tip/candidate
   pointers WITHOUT invoking the ABC-CSN forest rewind. Needs: a guard so the
   worker stops re-rewinding once the reorg is in progress (track in-progress
   reorg target), and a way for ActivateBestChain to know the forest is
   already at the candidate. Risk: the forest=candidate / active=old window
   must be provably consistent for concurrent readers (bridge proof serving,
   getutxoproof).
2. **ActivateBestChain-owned reorg, worker validates into a scratch forest.**
   The worker validates competing blocks into a throwaway forest clone (proof
   check only), never mutating the shared forest; ActivateBestChain's ABC-CSN
   path is the sole shared-forest mutator on reorg. Cleaner ownership, but the
   worker's `ReplayBlock` currently mutates the shared forest — larger change.
3. **Assemble-then-decide barrier.** Keep the barrier as flow-control but
   change its lift condition from "replacement block is active" to "the full
   competing branch (fork+1 … best_header) is downloaded and buffered," then
   let one owner replay+reorg atomically. Smallest scheduler change; still
   needs the owner-conflict resolved.

## Scope / interactions

- Independent of the forest checkpoint delta campaign (pre-existing at
  interval=1). **But** entangled at deploy time: the campaign's every-N
  default breaks the CSN reorg's exact-height restore reads
  (`RestoreUtreexoCheckpoint:10443`, ABC-CSN `:7605`) — a WIP read-conversion
  to `storage::RestoreHistoricalForest` exists (stashed) and additionally
  surfaced that CSN worker checkpoints (`daemon_app.cpp:4566`) are written
  without a consistent canonical-roots flag (stateless forest never calls
  `setCanonicalEmptyRoots`) → `restore-checkpoint-root-mismatch`. That flag
  consistency + sidecar writing on the stateless path is the separate CSN
  every-N work (campaign phase, "task #17").
- Not in production: fleet nodes are full/archival, not stateless. Only the
  phone (CSN) hits this path; the DineroDPI xcframework rebuild is HELD until
  this is fixed. A phone that livelocks on a deep reorg must not ship.

## Chosen design (owner decision 2026-07-17): ActivateBestChain-owned canonical reorg

Combines the earlier approaches #2 + #3. **ActivateBestChain is the sole
owner of all canonical state** — active tip, Utreexo forest, coins, undo,
shielded state, height index, persistence markers, notifications. The CSN
worker becomes a **speculative validator only** and never mutates shared
state during a reorg.

Rejected: worker-owned pointer-only activation. The ABC path does far more
than move a pointer — forest replay + shielded/canonical bookkeeping
(`chainstate_service.cpp:7578`+). A matching forest root cannot prove those
other state transitions committed.

### Flow

1. Scheduler downloads the COMPLETE competing branch into a bounded buffer
   (the barrier becomes an **assembly barrier**, not a per-block stop).
2. Worker restores the fork into a **private scratch forest** via
   `storage::RestoreHistoricalForest` (`include/storage/forest_restore.h:44`).
3. Worker validates the branch sequentially **against that scratch forest**
   — never the shared forest.
4. Worker publishes an immutable, **hash-anchored `CsnReorgPlan`**.
5. ActivateBestChain locks activation, verifies the plan is still current,
   preloads all replay data, and ALONE performs the canonical
   disconnect/replay/bookkeeping. Active tip published LAST. Stale plans are
   discarded without touching live state.

Supported naturally because StatelessNode already binds to an arbitrary
`UtreexoForest*` (ctor `include/network/stateless_node.h:162`); today its
validation commits to whichever forest it holds
(`stateless_node.cpp:604` — `*utreexo_forest_ = std::move(working_forest)`).

### Grounded code seams (for execution)

- **Speculative validation seam** — `ValidateUtreexoProof`
  (`stateless_node.cpp:438-622`) does: stump-resync from member forest
  (463-475) → proof checks vs `local_stump_`/`local_commitment_` (477-564) →
  apply delta to a working copy (566-586) → verify `root_after` (588-601) →
  **commit to member forest** (604). Extract 477-601 into a core that takes
  explicit `(UtreexoForest& forest, UtreexoStump& stump)`; the existing
  method calls it with member forest/stump then commits; a new
  `ValidateProofIntoForest(block, proof_msg, UtreexoForest& scratch, peer_id)`
  calls it with a caller scratch forest + a stump built from that scratch,
  and does NOT commit to the member.
- **Assembly barrier** — `block_download_scheduler.cpp:739-748`: change the
  lift condition from "replacement block became active" to "the full
  competing branch (fork+1 … best_header) is buffered", so the whole branch
  downloads before handoff.
- **Reorg-reset livelock** — `daemon_app.cpp:4870-4894` posts a reset per
  competing-block arrival; under the new design the worker validates into
  scratch and posts ONE `CsnReorgPlan`, so this per-arrival re-rewind goes
  away.
- **ABC handoff** — `ActivateBestChain` competing-candidate path reaches the
  ABC-CSN stateless reorg at `chainstate_service.cpp:7588`; that becomes the
  sole canonical mutator, consuming the worker's plan instead of
  re-deriving.

### Staged landing (owner-specified)

1. Ownership + scratch-validation tests at `--utreexo.checkpoint_interval=1`.
2. `CsnReorgPlan` + isolated (scratch) forest validation.
3. Full-branch scheduler assembly + ABC handoff.
4. Interleaving, stale-plan, invalid-proof, crash-recovery tests.
5. SEPARATELY: convert the exact-height checkpoint reads for every-N
   (the stashed read-conversion WIP + the CSN checkpoint canonical-flag
   consistency fix).

Hold the DineroDPI CSN xcframework until the 3 failing reorg suites AND the
existing passing CSN regression suite are green.

## Guards for whoever implements

- Keep the passing CSN suite green: `BridgeCsnHistoricalSpendRelay`,
  `CsnContaminatedCheckpointRecovery`, `CSNShieldedSpendSync`.
- Do not regress linear IBD (the barrier protects ordered stateless sync).
- Reproduce cleanly with `--utreexo.checkpoint_interval=1` to isolate this
  from the campaign's every-N restore work.
