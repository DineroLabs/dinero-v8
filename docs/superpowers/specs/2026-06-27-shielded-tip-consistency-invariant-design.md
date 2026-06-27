# Shielded Tip-Consistency Invariant (Approach A)

**Date:** 2026-06-27
**Status:** Design approved, pending spec review → plan
**Branch:** `harden/shielded-tip-consistency` (off `dinero-main` @ `e3feaca06` = `v8.0.6`)
**Sequences after:** PR #330 (v4 snapshot shielded restore + nullifier restore)

## Problem

The shielded pool maintains a second cryptographic state machine alongside the UTXO
chain — the commitment **tree** (frontier), the global **nullifier set**, and the
rolling **anchor-history** window — with its own persistence. Every cross-cutting
mechanism (AssumeUTXO snapshot bootstrap, restart recovery, reorg) must keep that
state in lockstep with the chain, and historically several have failed to:

- AssumeUTXO snapshot restore carried no shielded state → empty tree at a populated
  height → first post-snapshot shielded spend fails `AnchorInvalid` and the chain
  wedges (mainnet block 50038).
- Restart after a snapshot bootstrap → shielded-tip misalignment → blunt safe mode
  that *also* blocked transparent `scanutxos` (operator saw a 0 balance).
- The snapshot-restore fix (#330) itself initially restored tree+anchors but not
  nullifiers — the same class, re-created.

**Root cause of the *class*:** the "is in-memory shielded state consistent with the
persisted tip?" check already exists, but it is **copy-pasted across ~6 call sites**
(startup ~1481, catch-up recovery ~2189, snapshot ~8966, connect ~9357/9801/10571)
with **inconsistent responses** (some `return false`, some `EnterSafeMode`, some
silently persist-and-continue). There is no single authoritative invariant, so any
*new* state-transition path (e.g. AssumeUTXO restore) simply omits it and desync
slips through silently — surfacing later as a wedge or a wrong balance.

This is explicitly **not** a consensus change: it adds no block data and no validation
rule for other nodes. It only changes how *this* node detects and responds to its own
internal shielded desync. It is strictly safer than today, where desynced paths proceed
while wrong.

## Goals

1. A single authoritative shielded tip-consistency invariant, enforced uniformly at
   **every** shielded state-transition boundary.
2. On desync: **fail loud, scoped, and actionable** — degrade *shielded* only;
   transparent functionality (sync, `scanutxos`, transparent balance/spend) keeps
   working; emit a diagnostic that names the exact cause and the repair.
3. A **narrow, logged, bounded, re-checked** conservative auto-heal for the cheaply
   repairable cases, so operators rarely need to act manually.
4. No behavior change for a correctly-synced node (always `Aligned`).

## Non-Goals

- The consensus coinbase-commitment rule (Approach B) — separate, future, protocol-
  upgrade project.
- Rewriting shielded persistence (the flat-file→ChainDB consolidation is a separate
  hardening item).
- Fixing snapshot *restore* itself — that is #330; this composes with it.

## Existing primitives (reused, not rebuilt)

- `CurrentShieldedStateSnapshot() → {root, tree_size, nullifier_count}` — the in-memory
  triple. (`chainstate_service.cpp:1102`)
- `ChainDB::getShieldedTipMarker() → {height, block_hash, shielded_root, tree_size,
  nullifier_count}` — the persisted truth. (`include/storage/chain_db.h:339`)
- `EnterSafeMode(reason)` / `ExitSafeMode()` / `IsInSafeMode()` — existing fail-loud
  mechanism. (`chainstate_service.h:270-275`)
- `RangeHasShieldedActivity(start, end)` — cheap "did any block in this range carry
  shielded activity?" (`chainstate_service.cpp:1450`)
- `PersistShieldedState()` / `PersistShieldedTipMarker(hash, height)` — atomic persist.
- Reorg/replay primitives already used by the catch-up recovery path (~1962, ~2189).

## Design

### 1. The invariant function

```cpp
enum class ShieldedConsistency {
    Aligned,                       // in-memory triple == marker, marker tip == active tip
    MarkerMissingNoActivity,       // no marker, but no shielded activity ≤ tip → benign, persist
    MarkerMissingButActivityExists,// no marker, but activity exists ≤ tip → desync
    TipHeightMismatch,             // marker.height/hash != active tip
    RootMismatch,                  // tree.Root() != marker.shielded_root
    SizeMismatch,                  // tree.Size() != marker.tree_size
    NullifierCountMismatch         // nullifiers.Size() != marker.nullifier_count
};

struct ShieldedConsistencyReport {
    ShieldedConsistency status;
    std::string         detail;    // operator-facing, names exact values + cause + repair
    // observed vs expected fields for logging/RPC
    uint64_t obs_tree_size, exp_tree_size;
    uint64_t obs_nullifiers, exp_nullifiers;
    uint256  obs_root, exp_root;
    uint32_t marker_height, active_height;
};

// Pure comparison: reads CurrentShieldedStateSnapshot() + getShieldedTipMarker(),
// classifies, builds the detail string. No side effects.
ShieldedConsistencyReport CheckShieldedTipConsistency(uint32_t expected_height,
                                                      const uint256& expected_hash) const;
```

This *replaces* the inline comparisons at the ~6 existing call sites — they all call
this one function. One place to get right; impossible for a new path to "forget" the
fields because the classifier owns all of them.

### 2. Enforcement boundaries

The invariant is evaluated (and responded to) at exactly the state-transition points:

| Boundary | Site | Expected tip |
|---|---|---|
| Startup, after `LoadShieldedState()` | `chainstate_service.cpp:1652` | persisted chain tip |
| After AssumeUTXO `LoadSnapshot` restore | `LoadSnapshot` (post-#330 restore block) | snapshot base height/hash |
| After reorg / rewind / catch-up recovery | ~1962 / ~2189 | new active tip |

A correctly-synced node returns `Aligned` at all of them → no behavior change.

### 3. Response policy (degraded mode + diagnostic)

On any non-`Aligned`, non-benign result, after auto-heal (§4) has been attempted and
re-checked:

- **Shielded-degraded mode** (new, distinct from the existing global safe mode):
  - Transparent stays fully functional: header/block sync, `scanutxos`, transparent
    balance and spend, RPC — all unaffected.
  - The node **refuses to connect a block that carries shielded transactions** (and
    marks shielded RPC/state `untrusted`) until repaired — so it neither wedges
    silently nor validates shielded against wrong state.
  - Surfaced as a flag in node status RPC (e.g. `shielded_state: "degraded"` with the
    report `detail`), independent of `safemode_active`.
- **Actionable diagnostic**, e.g.:
  `"[Shielded] DEGRADED: tree_size=0 but tip marker expects 10 at height 52066 — `
  `AssumeUTXO restore did not populate shielded state. Transparent funds unaffected `
  `and scannable (scanutxos). Repair: run 'reconcileshielded' or resync."`
- The existing blunt global `EnterSafeMode` for shielded desync is **replaced** by this
  scoped degraded mode at these sites (global safe mode remains for genuinely
  chain-wide problems like UTXO/forest misalignment).

### 4. Conservative auto-heal (narrow, logged, bounded, re-checked)

Attempted **before** declaring degraded mode, and only for deterministically-repairable
classes. Hard constraints (per approval):

- **Narrow** — only these cases auto-heal; everything else goes straight to degraded:
  - `MarkerMissingNoActivity` → persist a marker for the current (empty) state. (benign)
  - `MarkerMissingButActivityExists` **or** `Size/Root/NullifierCountMismatch` where the
    blocks needed to rebuild are **locally available** → replay shielded state forward
    from the last known-good shielded checkpoint to the active tip.
  - `TipHeightMismatch` where marker is *behind* the tip by ≤ bound and the gap blocks
    are available → roll the shielded state forward over the gap (the existing
    catch-up replay).
- **Bounded** — replay spans at most `MAX_SHIELDED_HEAL_BLOCKS` (config, default e.g.
  2000) **and** only over heights where the required block bodies are present. If the
  gap exceeds the bound or any block is missing, **abort heal → degraded mode** (no
  partial heal left in place).
- **Logged** — every heal logs: trigger classification, the exact height range replayed,
  blocks touched, before/after triple, and outcome. No silent repair.
- **Re-checked** — after heal, `CheckShieldedTipConsistency` is run **again**; only
  `Aligned` exits the heal as success. Any remaining mismatch → degraded mode with the
  post-heal report (heal never "downgrades" a mismatch to silent-proceed).
- **Atomic** — heal mutations + the resulting marker are persisted in one ChainDB batch
  (tree frontier + anchors + nullifiers + marker together), so a crash mid-heal cannot
  leave a new inconsistency.

### 5. Atomicity principle (carry-through)

Every place that persists shielded state writes the **three parts + marker together**
in one batch (already the pattern in `CommitBookkeeping`; the heal path and the
snapshot-restore path must follow it). The invariant treats the triple as one unit —
partial handling is exactly what created the class.

## Data flow

```
boundary reached (startup / snapshot-restore / reorg)
        │
        ▼
CheckShieldedTipConsistency(expected_height, expected_hash)
        │
   ┌────┴─────────────┐
Aligned/benign     mismatch
   │                  │
 persist          auto-heal eligible? ──no──► degraded mode + diagnostic
 marker if            │ yes
 missing              ▼
 continue        bounded, logged replay (atomic) ── blocks missing / over bound ─► degraded
                      │
                      ▼
                 re-run CheckShieldedTipConsistency
                      │
                 ┌────┴────┐
                Aligned   still mismatch
                 │            │
              continue    degraded mode + post-heal diagnostic
```

## Testing

**Unit** (`CheckShieldedTipConsistency` classifier): each input (empty-tree-vs-populated-
marker, root mismatch, size mismatch, nullifier-count mismatch, marker-missing±activity,
tip-height mismatch, fully aligned) → asserted classification + that `detail` names the
observed/expected values.

**Integration (regtest) — the teeth-tests:**
- **(a) Desync caught + transparent survives:** boot a node whose shielded state was
  wiped to empty under a populated marker (no healable blocks) → asserts it enters
  **shielded-degraded** with the precise diagnostic, refuses a shielded-bearing block,
  **and** `scanutxos` / transparent balance still work. (Fails without this change:
  today it silently proceeds or blunt-safe-modes.)
- **(b) No false positives:** a correctly-synced node returns `Aligned`, connects
  shielded blocks normally, RPC shows `shielded_state: "ok"`.
- **(c) Auto-heal happy path:** marker-missing-but-blocks-available (within bound) →
  heal replays the bounded range, logs it, re-check returns `Aligned`, node continues.
- **(d) Auto-heal bound respected:** gap > `MAX_SHIELDED_HEAL_BLOCKS` or a block body
  missing → heal aborts, node enters degraded (no partial state persisted).
- **(e) Composition with #330:** after a v4 snapshot restore (tree+anchors+nullifiers),
  the snapshot boundary returns `Aligned` and a post-snapshot shielded spend connects.

Every test must fail on the pre-change tree (neutered) to prove it gates.

## Risks / mitigations

- **False-positive degrade on a healthy node** → the invariant must derive observed and
  expected from the same canonical sources the connect path uses; covered by test (b)
  plus running the full existing ctest suite (no regressions in shielded reorg-
  invertibility tests).
- **Auto-heal masking an upstream bug** → bounded + logged + re-checked; it never
  silently downgrades a mismatch, and the range/trigger are always logged for forensics.
- **Interaction with #330** → this branch is based on the v8.0.6 tag and is designed to
  land *after* #330; the snapshot-restore boundary check assumes #330's restore (incl.
  nullifier restore) is present. If #330 is still open at integration time, rebase onto
  it.

## Rollout

1. Land after #330 (rebase onto it if needed).
2. Ship in the next patch release alongside the v4 snapshot work; ordinary node-local
   change, no activation/fork coordination.
3. Follow-up (separate): Approach B (consensus coinbase commitment) for network-wide
   tamper-evidence.
