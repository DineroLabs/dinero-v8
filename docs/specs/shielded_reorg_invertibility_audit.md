# Shielded-era Reorg Invertibility Audit

**Status:** drafted 2026-04-28 after the LA chainstate-recovery
investigation. Owner: project. Tracks task #27 follow-up.

The chainstate-recovery code that ships today was written before
shielded-pool consensus state existed. As more state containers
were bolted onto Connect/Disconnect, the rules for "what does it
mean to undo a block?" stopped being a single, total relation
and became a fan-out of rules across multiple files, each
maintained semi-independently. The 2026-04-28 LA audit (commits
`e5aa07009`, `a72053a9a`, `5929d0992`) closed three concrete
asymmetries; none of them fully heals the live fleet because
the existing on-disk state was produced under the buggy code.

This document inventories every state container that crosses
the reorg boundary, every code path that mutates one, and every
asymmetry visible from a static read of the source. The goal is
a single, total `Connect/Disconnect` relation we can test with a
property-based harness and that startup-recovery does not
sidestep.

## Framing: the shared danger is not "rings" or "shielded" — it is the pattern

Rings (deleted on 2026-04-17) broke Utreexo because they added
non-UTXO consensus state with awkward rollback semantics: ring
signatures, key images, ring-membership commitments. Each one
had to be invertible across reorgs. They weren't, and the
fragmentation incidents on Apr 13/18 are the receipts.

The pattern is:

> `ConnectBlock` mutates extra consensus state → `DisconnectBlock`
> must exactly undo it → `reindex`/replay must rebuild the exact
> same state.

Rings is just one instance. **Shielded is the same pattern with
different containers**: a Sapling-shape commitment tree, a
nullifier set, an anchor-history window, a Utreexo forest still
tied to block connect/disconnect, plus persisted recovery
markers and a reindex path that has to reproduce all of it.

So even without rings, once shielded exists Utreexo is no longer
the only thing that has to be invertible. The daemon now hosts
**multiple consensus state machines that must move together**.
Every confirmed gap in the table below is an instance of one
state machine drifting from another.

The remaining structural work — phase 3 — is exactly this:
*"when a block connects, every consensus container either all
commits together or none of it does."* That is what prevents
shielded from becoming another rings-style incident, and what
makes any future container (covenants, governance, vault) safe
to add.

---

## State containers

Five mutable state containers cross the reorg boundary:

| # | Container | Type | Persistence |
|---|-----------|------|-------------|
| 1 | `consensus_utxo_set_->utxos_` | `unordered_map<OutPoint, UTXOEntry>` | UTXO column family in ChainDB; per-block checkpoints |
| 2 | `consensus_utxo_set_->forest_` | `UtreexoForest` (nodes_/roots_/numLeaves_/deleted_positions_/`canonical_empty_roots_`) | `putUtreexoCheckpointWithChecksum` per block |
| 3 | `shielded_tree_` | `CommitmentTree` (frontier; depth-32 incremental Merkle) | Frontier serialized into `BlockUndo` and into a periodic `ShieldedTipMarker` |
| 4 | `shielded_nullifiers_` | sqlite-backed set of `(nullifier, height)` | sqlite WAL |
| 5 | `shielded_anchor_history_` | in-memory deque of `(height, root)`, depth=100 | Single on-disk file (`Save`/`Load` at shutdown/startup) |

Plus one *index*, derivable from (1) + (2):

| # | Index | Type | Persistence |
|---|-------|------|-------------|
| 6 | `utxo_position_index_` | `unordered_map<(TxId,vout), uint64_t>` | none — rebuilt at startup |

(6) is not consensus state but `getutxoproof` walks it; if it
desynchronizes from (2), proofs return "missing live UTXO" for
existent UTXOs and the safety fuse trips.

---

## Connect path: every mutation site

The live `Connect` path is `ChainstateService::ConnectTip`, which
calls `BlockValidator::ConnectBlockInternal`, then mutates the
position index itself. Mutation sites in canonical order:

1. **utreexo_set add/remove (PASS 1 + PASS 2)** in
   `block_validation.cpp:1614-1776`. Skips intra-block ephemeral
   UTXOs. Commits clone-then-move at line 1898.
2. **UTXO map AddCoin/SpendCoin** at lines 1864-1891. Deferred
   until after forest validation passes.
3. **Shielded tree append + nullifier insert** via
   `shielded::ApplyBlockShielded` at `block_validation.cpp:2020`.
4. **Anchor history record** — *MISSING* on the live path. Both
   `BlockReindexer` (`reindexer.cpp:1271`) and
   `ChainstateService::ReplayShieldedBlockForward`
   (`chainstate_service.cpp:895`) call
   `shielded_anchor_history_.RecordRoot(height, tree.Root())`
   right after `ApplyBlockShielded`. The live `ConnectBlockInternal`
   does not. **Live-built chains have an empty `anchor_history`;
   reindexed chains have a populated one.** Validation rules
   diverge accordingly: live shielded txs can only reference the
   exact tip root, reindexed/recovered ones can reference any
   anchor in the 100-block window.
5. **Position index update** in `ChainstateService::ConnectTip`
   (`chainstate_service.cpp:8918+`). Skips intra-block ephemeral
   spends and outputs (correct). Asserts `delta.addedLeaves.size()
   == expected_outputs` (correct).

`BlockUndo` is populated to capture the pre-state for (5/2) Disconnect:

- `pre_block_snapshot` — full UTXO map + serialized forest
- `pre_block_shielded_frontier` — serialized frontier
- `utreexo_delta` — added/deleted leaves
- `spent_coins` — for the legacy fallback path
- *MISSING:* anchor-history rollback target (height before block)
- *MISSING:* nullifier-set rollback target (depends on
  `nullifiers->RollbackAbove(height-1)`, which is correct as
  long as height-1 is what's wanted)
- *MISSING:* explicit ephemeral set, so position-index Disconnect
  can re-derive it without re-running the same scan

---

## Disconnect path: every inverse site

`ChainstateService::DisconnectTip` calls
`BlockValidator::DisconnectBlock`, then mutates the position index.

**Snapshot path** (`block_validation.cpp:2069-2125`):
- `consensus_utxo_set_->Restore(snapshot)` — reinstalls UTXOs and
  deserializes forest. **Pre-2026-04-28: silently wiped the forest
  if the source had `canonical_empty_roots_=true`** (gap #3, fixed
  in `a72053a9a` via serialize v3).
- `tree->DeserializeFrontier(pre_block_shielded_frontier)`.
- `nullifiers->RollbackAbove(height-1)`.
- `anchor_history->RollbackAbove(height-1)` — fixed in
  `e5aa07009`. Note: only meaningful once gap #4 (live RecordRoot)
  is also fixed.

**Legacy path** (`block_validation.cpp:2127+`):
- `DeleteCoin` per-output / `AddCoin` per-spent-coin from undo.
- `forest_.removeLastNLeaves(addedLeaves.size())` then
  `restoreDeletedLeaf` in reverse for each deleted. This assumes
  adds went to the *end* of the leaves vector — an assumption the
  canonical-roots fork can violate. Worth a separate look.
- Same shielded handling as snapshot path.

**Position-index undo** (`chainstate_service.cpp:8127-8210`):
- Pre-2026-04-28: did not skip ephemeral UTXOs in either step,
  causing `delta.deletedLeaves` desync and silent drift (gap #1,
  fixed in `e5aa07009`).
- Post-fix: ephemeral filter applied symmetrically; deleted_idx
  consumption asserted.

---

## Bypass paths: mutations *outside* Connect/Disconnect

These exist because startup is allowed to fix up state without
going through the consensus path. Each one is a separate rule
that has to stay in sync with the canonical Connect/Disconnect
inverse, and historically hasn't.

### A. Startup shielded-state recovery (`chainstate_service.cpp`)

| Function | When called | What it mutates |
|----------|------------|-----------------|
| `RecoverShieldedStateFromTipMarker` (`:899`) | startup, when the persisted shielded-tip-marker is reachable | `shielded_tree_` (Deserialize), `shielded_nullifiers_` (RollbackAbove), `shielded_anchor_history_` (RollbackAbove) |
| `RestoreShieldedFrontierFromUndoBlock` (`:816`) | helper for above | `shielded_tree_` (Deserialize) |
| `ReplayShieldedBlockForward` (`:852`) | helper for above | `shielded_tree_` (Append), `shielded_nullifiers_` (Insert), `shielded_anchor_history_` (RecordRoot) — same as reindex |
| `RewindShieldedStateToActiveTipForStartup` (`:1019`) | when persisted tip marker is *ahead* of canonical active tip | `shielded_tree_` (Deserialize), `shielded_nullifiers_` (RollbackAbove) |

`RecoverShieldedStateFromTipMarker` has FIVE distinct branches
based on the relationship between `marker.height`, `tip_height`,
and `marker.block_hash` vs `tip_hash`. Each branch has its own
recovery rule. Each is a place where "the right inverse" can
diverge from `BlockValidator::DisconnectBlock`'s inverse.

### B. Shutdown / startup persistence

| Container | Save | Load |
|-----------|------|------|
| `shielded_anchor_history_` | `Save(path)` at shutdown (`:780`) | `Load(path)` at startup (`:747`) |
| `shielded_nullifiers_` | sqlite WAL on each Insert | sqlite reopen at startup (`:706`) |
| `shielded_tree_` | frontier captured into per-block undo + into `ShieldedTipMarker` periodically (`:764`) | `DeserializeFrontier` at startup (`:733`) |
| `consensus_utxo_set_` | UTXO column family per-write; utreexo checkpoint per-block (`:8967`) | startup loads both |
| `utxo_position_index_` | not persisted | `Rebuild` at startup |

The four containers have **four different durability lifecycles**.
A daemon crash at any point between two writes can leave them in
internally inconsistent states. The current code handles each
case via its own `Recover…` branch (see A above).

### C. Reindex (`BlockReindexer`)

Has its own Connect-equivalent path that mutates all five
containers. Its rules match `ChainstateService::ReplayShieldedBlockForward`
but DIVERGE from live `BlockValidator::ConnectBlockInternal`
specifically on `RecordRoot`.

---

## Confirmed asymmetries

| # | Status | Location | Symptom |
|---|--------|----------|---------|
| 1 | **Fixed** `e5aa07009` | `chainstate_service.cpp` DisconnectTip position-index | LA: 9172 missing live UTXOs |
| 2 | **Fixed** `e5aa07009` | `block_validation.cpp` DisconnectBlock anchor rollback | reorged-out anchors stayed live |
| 3 | **Fixed** `a72053a9a` | `utreexo_accumulator.cpp` serialize v2 → v3 | silent forest wipe on Restore after fork |
| 4 | **Fixed** `5a1593c10` | `block_validation.cpp:2020` ConnectBlockInternal | live `anchor_history` was empty; aligned with reindex/recovery via RecordRoot per the AnchorHistory contract |
| 5 | **Fixed** | `block_validation.cpp:2152-2181` legacy DisconnectBlock | Two-part fix. (1) `removeLastNLeaves` now uses a PASS-1 validate / PASS-2 mutate split — a contract violation discovered mid-range never leaves the forest with positions partially cleared (commit b520a3196 + this commit's PASS-1 expansion). (2) The legacy `DisconnectBlock` caller now snapshots both the consensus UTXO set and the shielded tree frontier at function entry and restores them on any mid-function failure, so a partial-mutation state never escapes the function. The two parts together apply the §1 atomic-unit law at the legacy path's granularity. The path itself is still slated for phase 5 deletion. |
| 6 | **Open** | `RecoverShieldedStateFromTipMarker` 5-branch logic | each branch is its own recovery rule, can diverge from `DisconnectBlock` inverse |
| 7 | **Open** | crash-boundary durability | the 4 containers have 4 different fsync lifecycles; partial-write recovery currently per-branch |
| 8 | **Fixed** | `ApplyShieldedBundle` vs `ApplyBlockShielded` | `ApplyBlockShielded` is now a one-liner that loops `ApplyShieldedBundle`. Single source of truth for "what does it mean to apply a shielded bundle's mutations to consensus state." |
| 9 | **Fixed** | `daemon.shieldedstatehash` v1 nullifier coverage | Phase 3b step 1 (this commit). `NullifierSet::SerializeContent()` emits a stable `(block_height ASC, nullifier ASC)`-sorted byte stream of every entry, prefixed with its own tag/version/count header. `daemon.shieldedstatehash` is bumped to v2 (tag `'DSR2'`) and now folds the sorted nullifier content into the digest instead of just the count. Property tests get a real content-level oracle for nullifier drift, not just count drift. |
| 10 | **Fixed** | LA 2026-04-28 9291-style pre-fix drift | Three layers of coverage: (1) integration `ReindexLegacyV2ForestFixture` plants a v2-format on-disk forest via the `DINERO_FOREST_SERIALIZE_LEGACY_V2=1` debug knob and asserts copy-then-reindex preserves composite state; (2) unit `testCanonicalEmptyRootsLegacyV2Recovery` covers one fully-deleted-root shape; (3) parameterized unit `testCanonicalEmptyRootsLegacyV2RecoveryShapes` (audit row #10c — closed) walks 7 shapes including the smallest reproduction (numLeaves=1 with leaf 0 drained) which actually fired the bug. The deserializer at `utreexo_accumulator.cpp::deserialize` now retries `rebuildRoots()` with `canonical_empty_roots_=true` if a v2-payload mismatch is detected — this catches the silent-wipe path that test 6c had passed by accident due to multi-tree shape interaction. |

---

## Where the design needs to go

The code as-shipped treats `Disconnect` as "approximately the
inverse of Connect". For shielded-era state that doesn't hold —
the relation needs to be exact, by construction. Concretely:

### Principle 1: BlockUndo is authoritative

The only legal way to undo a block is to read `BlockUndo` for
that block. Anything missing from `BlockUndo` is a bug, not a
"recovery code can re-derive it" exception. Today, `BlockUndo`
captures (UTXOs + forest snapshot) + (shielded frontier) +
(utreexo delta), and Disconnect re-derives the rest. Move
everything Disconnect needs into the record:

  - Pre-block anchor-history tail (height of last entry before
    this block), so anchor rollback is exact rather than
    "everything above height-1".
  - Pre-block nullifier-set Size + last-inserted height per
    block, so a partial sqlite write at Connect crash time can
    be detected and rolled back.
  - Block's ephemeral-output set, so position-index Disconnect
    doesn't have to re-derive it (the place where gap #1 hid).

### Principle 2: One Connect, one Disconnect

Today there are three apply paths: live `ConnectBlockInternal`,
`BlockReindexer::applyBlock…`, and `ChainstateService::ReplayShieldedBlockForward`.
Each has its own subset of mutations and its own bugs (`RecordRoot`
in two of three; `canonical_empty_roots_` flag preserved in two of
three; etc.). They should be one function on a `IConsensusState`
interface, called by every code path that needs to advance state.

The recovery paths in `chainstate_service.cpp:899-1014` should
not exist as separate logic. Startup should be: load persisted
state, walk forward via the canonical Connect, walk backward via
the canonical Disconnect, never touch state through any other
hook.

### Principle 3: Atomic persistence — the all-or-nothing rule

In plain English: **when a block connects, every consensus
container either all commits together or none of it does.**

A single `ChainDB::WriteBatch` per Connect/Disconnect that
contains every column-family write needed — tip pointer, UTXO
delta, utreexo checkpoint, shielded frontier, anchor delta,
nullifier journal entries — committed as one atomic transaction.
sqlite needs to be either folded in or relegated to a write-ahead
journal that's rolled forward from the same point as ChainDB.

If the daemon crashes mid-Connect, on restart it sees either
"block N applied" or "block N not applied" — never partial.
That eliminates the entire B + C bypass-path tree above and is
the structural fix that prevents the next consensus container
(covenants, governance, vault) from re-introducing the same
class of bug.

**Phase 3 design lives at**
`docs/specs/atomic_consensus_persistence_phase3.md`. Phase 3 is
greenlit as a design-first patch (this commit), then a narrow
opt-in staging scaffold (phase 3a), then container-by-container
extension (phase 3b). Bypass-path deletion is deferred to phase
5 and only after phase 3b has soaked. The
`ReindexCopiedDatadir` regression is the gate.

### Principle 4: Property test, not unit test

A regtest harness that:

  1. Walks chain forward N blocks (with shielded txs).
  2. At every height, captures a hash of (UTXOs ⊕ forest serial ⊕
     tree frontier ⊕ nullifier set ⊕ anchor history).
  3. Walks chain backward to height 0 via Disconnect.
  4. Walks chain forward again.
  5. At every height, the captured hash must match the original.

This is one test that catches every asymmetry in the table above
plus all the ones we haven't found yet. The fact that we don't
have it is why each new gap had to be discovered by chain-state
forensics on a live fleet.

---

## Implementation plan (multi-session)

| Phase | Scope | Risk |
|-------|-------|------|
| 1 | Add gap #4 fix: live `ConnectBlockInternal` calls `RecordRoot` after `ApplyBlockShielded`. Aligns live with reindex/recovery. Consensus relaxation (more anchors valid), not tightening. | Low — current chain has no shielded txs that depend on this |
| 2 | Property-test harness as Principle 4. Pin current invariants on regtest. | Low — pure new code |
| 3 | Move pre-block-anchor-history + nullifier-rollback-marker into `BlockUndo`. Disconnect uses them instead of `RollbackAbove(height-1)`. | Medium — changes undo schema; needs migration |
| 4 | Atomic persist (Principle 3). | High — touches ChainDB write paths and sqlite |
| 5 | Delete bypass paths (Principle 2). Keep startup-recovery as a "verify and walk back to last checkpoint" only. | High — touches the 5-branch `RecoverShieldedStateFromTipMarker` |
| 6 | Audit `removeLastNLeaves` legacy-path assumption (gap #5). Either remove the legacy path or fix the assumption. | Low |

Phase 1 is shippable today as a one-line addition. Phase 2 is the
gating phase — without it the rest is invisible. Phases 3-5
together are the "rewrite" the task #27 description calls for.

---

## What this audit does NOT cover

- Wallet-side state (separate UTXO index, separate scan paths).
- P2P-layer reorg signaling (orphan handling, BlockAcceptor).
- Mempool eviction on reorg (separate concern; mempool reads
  shielded state but doesn't mutate it, so it's invariant under
  this audit).

---

## Addendum (2026-04-28): what `daemon.shieldedstatehash` v1 does NOT prove

The composite hash from commit `81e5db8ec` makes phase 2 sharper —
the property test now asserts byte-equality directly on the hash,
including every entry of the anchor history. Two limits remain
explicit so they don't get lost as the audit closes phase by phase:

1. **It does not close phases 3-6.** It proves the current
   Connect/Disconnect/Connect cycle returns to the same tracked
   state on a chain the new binary built. It does not make
   persistence atomic, does not unify the stateful and stateless
   write paths, does not delete any of the bypass-recovery
   branches in `RecoverShieldedStateFromTipMarker`, and does not
   audit the legacy `removeLastNLeaves` assumption (gap #5).

2. **Nullifier coverage is by count only.** The `NullifierSet`
   contributes its `Size()` to the hash. Two same-size nullifier
   sets with different members produce the same digest. Count
   drift is caught (an Insert/RollbackAbove asymmetry would
   change Size); content drift is not (a swapped nullifier value
   at the same height would not). Closing this requires a stable
   enumeration of `NullifierSet` (sqlite `ORDER BY block_height
   ASC, nullifier ASC`) plus a content-hash helper, fed into a v2
   of `ComputeShieldedReorgStateHash` and tagged `DSRH v2`. Track
   as gap #9 above.

The digest is tagged `DSRH v1` so a future v2 can extend it
without breaking the v1 contract.

---

## Addendum (2026-04-28): StatelessNode trace

`StatelessNode` is the fourth Connect surface flagged at the end of
the original audit. Tracing it here so phase 3+ can decide whether
to fold it into the unified Connect/Disconnect model or treat it
as a formally separate one.

### Activation

Gated by `GetConfig().utreexo_stateless && utreexo_forest`
(`daemon_app.cpp:2435`). The flag is `true` only under the
`ios_utreexo` sync profile (mobile / DineroDPI). Mainnet fleet
servers run `mac_fullblock` → `utreexo_stateless = false` →
**StatelessNode is not instantiated on the fleet**, so the existing
LA/VA/MO/CN drift is *not* coming from this path. iOS shielded
support, when it ships, will land here.

### Mutation surface

`StatelessNode` holds `consensus::UtreexoForest*
utreexo_forest_` and mutates only the forest plus a local
`UtreexoStump` cache. It does **not** touch the UTXO map, the
shielded tree, the nullifier set, or the anchor history. Four
mutating entry points:

| Method | Source | Forward / Reverse |
|--------|--------|-------------------|
| `ValidateUtreexoProof` (`stateless_node.cpp:275`) | proof message from a bridge peer | forward; mutates `*utreexo_forest_` after batch-proof verification succeeds |
| `ValidateWithTransitionProof` (`:465`) | transition-proof variant | forward |
| `ReplayBlock` (`:854`) | called from `ChainstateService` during a CSN reorg | forward |
| `RewindToCheckpoint` (`:829`) | called from `ChainstateService` during a CSN reorg | reverse — replaces forest with a caller-supplied checkpoint and resets stump |

### Interaction with `BlockValidator`

In stateless mode, `BlockValidator::ConnectBlockInternal` takes the
early-return at line 1519 ("STATELESS MODE: Early return — skip
forest clone/mutation/root verification") because StatelessNode has
already mutated the canonical forest. **Crucially, that early
return runs BEFORE the `BlockUndo` population at line 2032+** —
which means in stateless mode `undo.pre_block_snapshot` and
`undo.utreexo_delta` are never written.

Consequence: `BlockValidator::DisconnectBlock` cannot roll back a
stateless-mode block. Its snapshot path requires
`pre_block_snapshot.has_value()` (false), the legacy path
requires `utreexo_delta.has_value()` (false → "missing-utreexo-
delta-undo-data" error). Stateless reorgs therefore go through a
**completely separate code path**: `ChainstateService` calls
`stateless_node_->RewindToCheckpoint(fork_height, restored_forest)`
where `restored_forest` is loaded from the Utreexo checkpoint
column family (stored per-block by `ConnectTip`), then
`stateless_node_->ReplayBlock(...)` per replayed block.

### Invertibility in CSN reorg mode

`RewindToCheckpoint` is total (overwrites the forest), not delta-
based. Its inverse is the original `*utreexo_forest_` value, which
is implicit in the caller passing the right checkpoint. There is
no `BlockUndo` involved.

`ReplayBlock` is forward-only. It validates the resulting forest
root against `block.header.utreexo_root` (line 883), so a bad
replay fails loud rather than silently drifting. That makes it
*safer* against the kind of drift gap #1/#2/#3 produced on the
fleet — but the safety only holds because the header pre-commits
the forest root. Anything that doesn't (shielded state, anchor
history, nullifiers) is not protected by this check.

### Implications for phases 3-6

1. **Phase 3 (atomic persistence)** has to model two write paths,
   not one: the stateful path (UTXO + forest + shielded + position
   index) and the stateless path (forest + checkpoint pointer).
   They write to different ChainDB column families and have
   different recovery shapes. Folding them into one transaction is
   possible but not free.
2. **Phase 5 (bypass-path removal)** can't delete
   `RewindToCheckpoint`/`ReplayBlock` — they *are* the canonical
   stateless-mode reorg path. The audit's "single Disconnect"
   principle should be restated as "single Disconnect per mode".
3. **Shielded support in stateless mode** is currently absent.
   Whenever DineroDPI iOS gets shielded scanning, this code path
   will need parallel mutations + invertibility coverage for the
   shielded containers — and the property test will need a
   stateless-mode variant.
4. Position-index gap #1, anchor-history gap #2, and serialize
   gap #3 do **not** apply to the stateless path (none of those
   containers exist there). Gap #4 (RecordRoot in live Connect)
   does not apply either — stateless nodes don't run shielded
   validation today.

### Verdict

StatelessNode is a separate Connect surface but **not** a hidden
fourth instance of the same gaps. Its narrower mutation surface
(forest only) plus `ReplayBlock`'s built-in
header-root-equality check make it more defensively built than
the stateful path was. Phase 3+ should treat it as a parallel
state machine to be co-designed with the unified stateful
Connect/Disconnect, not as a path to retrofit into a single
shared abstraction.
