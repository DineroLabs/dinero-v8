# Shielded State Classification

**Date**: 2026-04-19
**Status**: Working architecture plan / high-priority reference
**Scope**: Shielded-pool state (commitment tree, nullifier set, undo data,
wallet note store) under the canonical-state discipline

---

## Purpose

[CANONICAL_STATE_AND_RECOVERY_PLAN.md](./CANONICAL_STATE_AND_RECOVERY_PLAN.md)
defined Class A / B / C state and set a Priority 5 ask:

> Before shielded consensus is declared fully active, explicitly decide
> which shielded structures are Class B and which remain Class C wallet
> state. Do not let wallet convenience state leak into consensus
> assumptions.

This document executes that ask. It audits the live shielded code path,
fixes the classification for every persisted shielded object, names the
rebuild story for each Class B object, and identifies the concrete gaps
that need closing.

---

## Activation Finding

**Shielded validation is consensus-active in v7 right now.**

Evidence: `src/consensus/block_validation.cpp:1838` calls
`shielded::ValidateBlockShielded(bundles, deltas, bctx)` during
`ConnectBlockInternal` whenever any transaction in the block carries
`TX_VERSION_SHIELDED = 5` (see `include/primitives/transaction.h:211`).
`ValidateBlockShielded` enforces:

- nullifier non-reuse against the pre-block nullifier set
- ZK proof verification against the pre-block commitment-tree root
- commitment-tree appends atomic with the block's UTXO mutations
- value conservation (shielded pool delta)

If a block contains a shielded transaction and any of those checks fail,
the block is rejected. Shielded state is therefore **not wallet
convenience**. It is consensus-critical derived state.

This is different from the old ring/CLSAG privacy stack, which was
permanently removed on 2026-04-17 because it was architecturally
incompatible with Utreexo. Shielded uses a Zcash-Sapling-shape primitive
(note commitments + nullifier set + ZK proofs) and is Utreexo-compatible.

No activation height gate was found — shielded validation runs from
genesis in v7 and is triggered by the presence of a v5 transaction, not
by block height.

---

## State Inventory

Applying the [canonical-state class
system](./CANONICAL_STATE_AND_RECOVERY_PLAN.md#state-classes):

| Object | Class | Authoritative Source | Persisted Where | Mutated By | Rebuild Story | May Block Startup? |
|---|---|---|---|---|---|---|
| Shielded commitment tree (full state) | B | Accepted shielded commitments on active chain, in order | in-memory only during runtime | `ConnectBlock` appends, `DisconnectBlock` restores from undo | Replay shielded commitments in block order from genesis (or from a trusted tree checkpoint + replay forward) | If corrupt: yes |
| `blockchain/shielded_frontier.bin` | B | Serialized incremental Merkle tree frontier of the commitment tree at ChainDB tip | datadir flatfile | `ChainstateService::PersistShieldedState()` at tip advance | Deterministic from commitment-tree replay above | If present but mismatched vs. tip: should halt |
| `blockchain/shielded_nullifiers.db` | B | Accepted shielded spends on active chain | RocksDB-backed `NullifierSet` | `ConnectBlock` inserts, `DisconnectBlock` calls `RollbackAbove(height-1)` | Deterministic from replay of all shielded spends in canonical order | If present but mismatched: should halt |
| `BlockUndo::pre_block_shielded_frontier` | A | Frozen snapshot of commitment-tree frontier before block was connected | In the undo flatfile entry for the block | Written at block connect; consumed at disconnect | Regenerated only by disconnecting subsequent blocks; authoritative per-block-undo | Directly — missing undo means no safe disconnect |
| `BlockUndo` shielded nullifier rollback record | A | Implicit via `NullifierSet::RollbackAbove(height-1)` — height is the canonical key | Derived from block-index height | Disconnect rolls nullifiers by height | N/A — state is height-indexed, not individually persisted in undo | Indirectly — if height index is wrong, rollback is wrong |
| Wallet shielded note store (`src/wallet/shielded_note_store.cpp`) | C | Wallet-local detection of received notes | wallet SQLite / local DB | wallet worker on block-connect notifications | Regenerate by rescanning from wallet birth height | Never |
| Wallet shielded runtime (`src/wallet/shielded_wallet_runtime.cpp`) | C | Wallet-local key derivation + spend building | wallet keystore / memory | wallet ops | Regenerate from HD seed | Never |
| Shielded pool monetary supply accounting | B | Per-block shielded pool delta | implicit in the commitment tree + nullifier set | block connect / disconnect | Sum of deltas over active chain | If supply computation diverges: yes |

The boundary between A and B here:

- **Class A** is the undo record carrying enough per-block state to
  disconnect. `pre_block_shielded_frontier` is the authoritative
  per-block rollback data. If it's missing, the block cannot be safely
  disconnected — equivalent to the Utreexo "missing undo at tip" failure
  that triggered the 2026-04-18 fleet fragmentation.

- **Class B** is the persistent on-disk snapshot (`shielded_frontier.bin`
  + `shielded_nullifiers.db`) that startup reads to avoid replaying the
  whole chain. These must be deterministically rebuildable from the
  block archive, because a stale or corrupt copy cannot be healed by
  "wait for the next block" — incoming shielded transactions are
  validated against them immediately.

---

## Rebuild Stories

For each Class B object, the canonical plan requires a named algorithm
and a named code path.

### Commitment tree + `shielded_frontier.bin`

**Canonical inputs:** every shielded-tx note commitment appearing in the
active chain, in block order and within a block in transaction order and
within a transaction in bundle order.

**Algorithm:** walk active chain from genesis to tip; for each
`TX_VERSION_SHIELDED` transaction, deserialize the bundle and append its
note commitments to an in-memory `CommitmentTree`; at tip, serialize the
frontier via `CommitmentTree::SerializeFrontier()` and write to
`shielded_frontier.bin`.

**Code path that performs the rebuild:** currently **none.** The normal
live path (`ConnectBlockInternal`) mutates the tree block-by-block.
Startup (`ChainstateService::LoadShieldedState`) reads the persisted
frontier if it exists; if missing it assumes "start empty" — which is
wrong except at genesis. There is **no code path that rebuilds the tree
from the block archive after corruption or reindex.**

**Invariant that proves rebuild success:** the tree root at the tip
block matches an external oracle. The cleanest oracle is a
consensus-level commitment (e.g., header field or per-block coinbase
commitment); today the tree root is **not** in the block header, so the
proof is indirect — any subsequent block whose shielded transactions
validate against this tree confirms the tree state matches network
consensus.

**Gap:** rebuild algorithm exists conceptually; there is no implemented
rebuild code path.

### Nullifier set + `shielded_nullifiers.db`

**Canonical inputs:** every shielded-tx nullifier appearing in the
active chain, keyed by block height for rollback.

**Algorithm:** walk active chain from genesis to tip; for each shielded
transaction, insert every nullifier into a `NullifierSet` along with the
block height; at tip, the DB is complete.

**Code path that performs the rebuild:** currently **none.** Startup
(`ChainstateService::LoadShieldedState`) opens the existing DB file; if
the file is missing, the DB is newly created empty. There is **no
replay path** that rebuilds nullifiers from the block archive.

**Invariant that proves rebuild success:** the nullifier set contains
exactly the union of nullifiers appearing in shielded transactions on
the active chain. The set's bit-identical equivalence with a fresh
replay is the bar.

**Gap:** same as the commitment tree — no rebuild code path exists.

### `pre_block_shielded_frontier` in `BlockUndo`

**Canonical inputs:** the commitment-tree frontier *before* the block
being connected is applied.

**Algorithm:** snapshot at block-connect time:
`tree.SerializeFrontier()` before the block's commitments are appended.

**Code path that performs the rebuild:** `ConnectBlockInternal`
captures `pre_block_shielded_frontier` into `BlockUndo` during block
connect (src/consensus/block_validation.cpp:520-524). The undo record is
persisted to the undo flatfile as part of the undo-record serialization.
`DisconnectBlock` restores from it at src/consensus/block_validation.cpp:1957.

**Invariant:** if the block is durably recorded as active, the undo
entry for that block must be durable and must contain the pre-block
frontier.

**Current status:** covered by the live replay proof on this branch.
`ShieldedReindexEquivalence` reads archival undo from disk and verifies
that the persisted `pre_block_shielded_frontier` matches the expected
pre-block frontier at every replayed height.

---

## Known Gaps (Priority-Ordered)

### Closed on current branch: Reindexer replay path for shielded state

`--reindex-chainstate` now reconstructs `shielded_tree`,
`shielded_nullifiers`, and archival `pre_block_shielded_frontier`
records from canonical block history. The live proof is
`ShieldedReindexEquivalence`, which compares live-vs-reindex shielded
state at every replayed height and requires:

- byte-equal serialized frontier
- equal tip height and hash
- equal persisted nullifier rows
- equal undo-carried pre-block frontier snapshots

This closes the same structural gap that previously existed for the
Utreexo forest: live sync and reindex now derive the same consensus
shielded state from the same block archive.

### Closed on current branch: ShieldedTipMarker startup consistency

`ChainstateService::Init` now has a shielded-side equivalent to the
Utreexo `ForestTipMarker`: `ShieldedTipMarker` persists height, block
hash, shielded root, tree size, and nullifier count. Startup now does
two distinct things with it:

- validates the persisted shielded frontier/nullifier state against the
  stored ChainDB tip before any replay begins
- rewinds shielded frontier/nullifiers back to the currently validated
  active tip when startup replay needs to run forward again

The live executable proof is
`ShieldedTipMarkerRestartEquivalence`, which verifies:

- aligned marker acceptance
- harmless stale-marker auto-heal across a shielded-inactive gap
- rewind-to-active-tip behavior for startup replay
- rejection of a stale marker across a shielded-active gap

### Gap 1: Executable equivalence proof coverage is partial

Per the canonical-state plan's minimum proof matrix, shielded state
needs three proofs:

| Scenario A | Scenario B | Expected Result |
|---|---|---|
| Fresh sync from genesis | Replay raw block files into empty chainstate | Same commitment-tree root, same nullifier set |
| Live reorg | Restart after interrupted reorg recovery | Same shielded state at final tip |
| Fresh sync | `--reindex-chainstate` on the same block files | Same shielded state |

A `shielded-canonical-recovery` CTest label now exists and is
registered at the top level in `CMakeLists.txt`, with release-suite
execution wired through `tests/test_release_suite.sh`. On the current
branch, the gate runs:

- `ShieldedReindexEquivalence`
- `ShieldedPoolRoundTrip`
- `ShieldedAdversarialHardening`
- `ShieldedTipMarkerRestartEquivalence`

Together those tests prove:

- mined live-vs-reindex frontier/nullifier equivalence
- persisted frontier/nullifier reopen semantics
- adversarial rollback and nullifier-hardening behavior
- shielded tip-marker verify/heal/rewind semantics at the chainstate-artifact level

The remaining proof gap is no longer replay/reindex or basic
tip-marker consistency. The separate
`phase2-shielded-canonical-recovery` label now covers four daemon-valid
regtest proofs with real `dinerod` and real mined shielded blocks:

- `ShieldedDaemonRestartEquivalence`
  - `after_undo_before_tip`
  - restart preserves or repairs state at the first live connect-time
    durability boundary
- `ShieldedTipPersistRestartEquivalence`
  - `after_tip_before_checkpoint`
  - restart converges to the already-committed shielded tip state after
    the later tip-persist boundary
- `ShieldedReorgDisconnectRestartEquivalence`
  - `after_disconnect_tip_before_shielded_flush`
  - restart plus retry-to-success proves shielded rollback state survives
    a live disconnect crash and can re-mine the same spend canonically
  - proof construction also verified that reorg reconciliation restores
    the disconnected shielded spend to mempool, so the remine path is
    real daemon behavior rather than operator-assisted re-submission
- `ShieldedReorgSecondRestartInvalidityEquivalence`
  - second clean restart after a successful rollback retry
  - proves the invalidated shielded block stays dead across a later clean
    restart instead of being revived from stale height-index or catch-up
    state

The remaining gap is now the rest of the full daemon-valid family:
additional crash-mid-connect boundaries on longer shielded histories,
multi-block rollback/reorg cases, and deeper shielded-history restart
families beyond the single-block rollback/second-restart case.

New proofs should follow the same pattern as the
`phase1-canonical-recovery` CTest label bundle added on 2026-04-19.

---

## Proof Backlog

Proposed executable tests to add to the existing
`phase2-shielded-canonical-recovery` label:

1. **Multi-block shielded rollback/reorg equivalence** — mine a
   shielded transfer into block `N`, extend the chain, force a rollback
   past `N`, and prove the frontier/nullifier state matches the clean
   pre-`N` baseline across the whole rollback window rather than only a
   one-block disconnect.
2. **Additional live crash-mid-connect boundaries** — add more
   deterministic crash points on shielded-active connect paths once a
   new durability seam exists that is not already covered by
   `after_undo_before_tip` or `after_tip_before_checkpoint`.
3. **Longer-history second-restart durability proof** — extend the new
   second-clean-restart invalidity proof from a one-block rollback into
   multi-block shielded histories, so restart stickiness is proven across
   deeper unwinds too.

---

## Decisions Captured

- Shielded validation is consensus-active in v7; all associated
  persisted state is Class A or Class B, not Class C.
- Commitment tree, frontier file, nullifier set DB, and undo-record
  shielded fields are consensus-critical — they must be deterministic,
  replay-equivalent across sync/reindex/restart.
- Wallet-owned note detection and spend building remain Class C —
  their loss may degrade the wallet but must never affect block
  acceptance or daemon startup.
- `--reindex-chainstate` now rebuilds shielded state and is covered by
  `ShieldedReindexEquivalence`; startup consistency is now checked via
  `ShieldedTipMarker`, and the first four daemon-valid crash/restart
  boundaries are green under `phase2-shielded-canonical-recovery`.

---

## Non-Goals

- This document does not propose reactivating the removed ring/CLSAG
  stack. That stack is permanently gone; see
  `memory/v7_ring_ct_removal.md`.
- This document does not define new shielded consensus rules. It
  catalogs the existing shielded validation behavior and imposes the
  canonical-state discipline on what is already there.

---

## Bottom Line

Shielded state gets the same treatment as UTXO + Utreexo: consensus
truth lives in blocks, everything derived must be rebuildable, and
every state transition must be equivalent across sync, restart,
reindex, and recovery. The known gap (Gap 1) is structurally identical
to the Utreexo gap we fixed yesterday and should be closed with the
same shape of patch.
