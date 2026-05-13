# Fleet HAVE_UNDO Repair Pass (May 2026 H.3 Regression)

- **Date:** 2026-05-02
- **Severity:** P2 — chain integrity at tip on 4/5 nodes; live mining unaffected
- **Resolution commit:** `fd1098937` (HeaderSyncManager fix) + `b67d005da` (clearHeaderStatusBits symmetric API)
- **Bug-introducing commit:** `55f0bc6bf` (2025-12-18, "Phase H.3 — Header persistence (crash-correct IBD)")
- **Window of impact:** 2025-12-18 → 2026-05-02 (~5 months silent)
- **Discovered by:** `dinero-cli health` (Phase D.4) during Phase E.2 install validation on MO
- **Fleet state after pass:** 4/5 nodes clean tip; Dell partially repaired pending separate metadata-recovery work; LA carries pre-existing chaindb-orphan-tip damage tracked separately

## Tl;dr

For 5 months, every block that gossiped via P2P after being mined locally
had its `BLOCK_HAVE_UNDO` status_flag silently stripped from chaindb by
`HeaderSyncManager::MarkBlockReceived`. The bug went undetected until
`dinero-cli health` (shipped May 1 in Phase D.4) added a
`tip_undo_present` check. Phase E.2's install validation on MO
exercised that check post-install and exposed the regression
fleet-wide.

The fix uses a new chaindb API (`setHeaderStatusBits`/`clearHeaderStatusBits`)
that OR-merges/AND-NOT-merges bits without touching unrelated fields.
HeaderSyncManager migrated to it. Fleet upgraded. Already-damaged
heights re-stamped where possible via the offline rebuilder.

## Root cause

`HeaderSyncManager::MarkBlockReceived` (added in `55f0bc6bf` 5 months
ago) called:

```cpp
chain_db_->updateHeaderStatus(token, hash, node->status, ...);
```

`updateHeaderStatus` OVERWRITES status_flags wholesale. `node->status`
is HeaderSyncManager's view of the block, which only tracks the bits
*it* cares about (BLOCK_HAVE_DATA, BLOCK_FAILED). Consensus-layer
bits like BLOCK_HAVE_UNDO that ConnectTip had set via the
chaindb-`updateBlockIndex` path were silently stripped on every
gossiped block.

Triggering sequence per block:

1. Local node mines block → ConnectTip writes undo bytes to
   `rev*.dat`, sets `BLOCK_HAVE_UNDO` on the in-memory CBlockIndex,
   persists via `updateBlockIndex` ⇒ chaindb metadata has HAVE_UNDO ✓
2. Block broadcast via BlockRelayManager
3. Same block arrives back at the miner (and other peers) via P2P
   relay
4. `HeaderSyncManager::MarkBlockReceived` fires →
   `updateHeaderStatus(node->status)` clobbers chaindb metadata.
   `node->status` lacks HAVE_UNDO ⇒ chaindb metadata loses HAVE_UNDO ✗

Heights 1..10783 were unaffected on every fleet node ONLY because the
May 1 hole-repair (commits `5771646b7..27a819b96`) re-wrote those
heights' status_flags via the offline rebuilder *after* every prior
MarkBlockReceived call had fired. Heights mined after May 1 each
acquired the regression on first gossip.

Different transition heights per node:

| Node | First HAVE_UNDO-stripped height (observed) |
|---|---|
| MO   | h ≥ 11000  (post-May-1 mine) |
| CN   | h ≥ ~12000 |
| Dell | h ≥ ~11500 |
| LA, VA | tip remained healthy (specific gossip timing did not strip the tip) |

## Fix

### Code (commits `fd1098937` + `b67d005da`)

- Added `ChainDB::setHeaderStatusBits(token, hash, bits_to_set, batch)`
  — OR-merge that never strips unrelated bits.
- Added `ChainDB::clearHeaderStatusBits(token, hash, bits_to_clear, batch)`
  — AND-NOT-merge that never strips unrelated bits.
- Migrated `HeaderSyncManager::MarkBlockReceived` and `MarkBlockFailed`
  to `setHeaderStatusBits(BLOCK_HAVE_DATA)` /
  `setHeaderStatusBits(BLOCK_FAILED)`.
- Loud header-comment on the existing `updateHeaderStatus` warning
  callers that it strips bits the caller didn't include. Production
  code must only use it for full-status overwrite cases.
- Regression test `tests/storage/test_header_status_bits.cpp` (6/6
  properties) pins:
  1. setHeaderStatusBits OR-merges; HAVE_UNDO survives a
     MarkBlockReceived-class call (the regression test)
  2. setHeaderStatusBits adds new bits
  3. clearHeaderStatusBits clears only requested bits
  4. setHeaderStatusBits is idempotent no-op when bits already set
  5. setHeaderStatusBits returns NotFound on missing metadata
  6. updateHeaderStatus still overwrites (intentional behavior
     preserved for the cleared_status reorg-rollback path)

### Fleet repair (per-node, sequential, ≥1h soak between)

For each node: pull source → build dinerod → capture rollback
artifacts (wallet backup + binary capture) → stop daemon → swap
binary → run `--rebuild-undo-range=10784:tip --rebuild-undo-write`
on live datadir → restart → verify health.

| Node | Pre-state | Action | Result |
|---|---|---|---|
| **CN** | h=12000+ sf=159 (no HAVE_UNDO) | binary swap to fd1098937 + rebuild | rebuilt=224, verify_failed=0; tip sf=415 ✓ |
| **LA** | tip sf=415 (already healthy) | binary swap to fd1098937; rebuild not needed | tip_undo_present=true ✓ |
| **VA** | tip sf=415 (already healthy) | binary swap to fd1098937; rebuild not needed | tip_undo_present=true ✓ |
| **MO** | h=12145 sf=159 (no HAVE_UNDO) | binary swap to fd1098937 + rebuild | rebuilt=1358, verify_failed=0; tip sf=399 ✓ |
| **Dell** | h=12145 sf=159 (no HAVE_UNDO) | binary swap to b67d005da; rebuild **REFUSED** by preflight | tip_undo_present=false ⚠ |

### Dell partial repair

Dell's rebuild preflight refused with `missing_metadata=63` —
63 heights in the 10784..12145 range have no chaindb metadata at
all (not just stripped bits; entirely absent). This is a *different*
bug class from H.3 regression — likely a putHeaderMetadata path
that skipped writes for some heights. Same pattern as LA's much
larger missing-metadata damage (see "LA discovery" below).

The new binary (`b67d005da`) prevents the regression from continuing.
Dell's existing damage at the tip stays as-is until a separate
metadata-recovery pass diagnoses the 63 missing-metadata heights.
Dell's reorg-safety is degraded relative to the rest of the fleet
(can't disconnect a tip-spanning reorg cleanly) but normal serving
+ mining is unaffected.

## LA discovery (separate, pre-existing)

LA's repair pass found a different state: chaindb has the tip
pointer but per-block metadata is **absent for 1362 heights** in
the rebuild range. Tip queries return correctly (tip pointer
intact) but `getblockheader` for any historical block returns
empty.

Diagnosis: orphan-tip — the `setTip` write for the current tip
exists, but a wide range of `putHeaderMetadata` writes never
landed (or were lost in a prior incident). Pre-existing damage,
*not* caused by today's repair pass.

LA's binary was upgraded to fd1098937 (regression stopped) and
the daemon restored to its pre-pass state. LA continues to serve
as a peer at tip 12145; per-block historical queries fail until
a separate metadata-recovery pass.

## Operational scar tissue

### `git stash --include-untracked` is destructive on this fleet

The `data-main → /root/.dinero` symlink on LA and VA is an
untracked file in the source tree. `git stash --include-untracked`
stashed and removed it during my pull-source step, breaking
systemd's `ConditionPathExists` and triggering fresh-IBD on
restart. Recovered via `git stash pop`. Should not happen again
because **`git stash --include-untracked` is now banned on this
fleet**. Use plain `git stash` or `git pull` with conflict
resolution.

### `dh_compress` strips `dinero.conf.example` to `.gz`

E.2-discovered: the .deb postinst's `cp` from
`/usr/share/doc/dinero/dinero.conf.example` doesn't find the
file because dh_compress gzipped it. Manually `zcat`'d during
E.2; needs Phase E.3 fix (add `dh_compress -X` exclusion or
update postinst to handle .gz).

## What this commit does NOT cover

- **LA's chaindb-orphan-tip damage** — separate metadata-recovery
  pass needed. The blocks/rev*.dat data is on disk (not consulted
  via getblockheader). Possible recovery path: rebuild from
  blk*.dat by reading the block stream and re-running
  putHeaderMetadata on each. Not implemented; tracked separately.
- **Dell's 63 missing-metadata heights** — same class as LA's,
  smaller scale.
- **Anchor-rooted rebuild-undo-range** that skips unaffected
  heights — current rebuilder runs full forward-replay from
  genesis (or specified anchor); for the 10784..12145 case it
  builds a complete temp DB through h=12145, which on
  disk-pressured nodes (MO at 4.7 GB free) hits disk-full at
  ~h=2984. Worked around by freeing 35 GB of stale rollback
  artifacts. Better long-term: `--rebuild-undo-range` should
  accept an anchor much closer to the lowest hole height and
  avoid re-replaying unaffected heights.

## Verdict

- **H.3 regression vector: CLOSED on the entire fleet.** All five
  nodes run binaries with `setHeaderStatusBits` migration. No
  newly-mined block can lose HAVE_UNDO via the gossip path.
- **Already-damaged heights: REPAIRED on 3/5 nodes** (CN/MO via
  rebuilder; LA/VA's tips were never damaged). **Dell partial:**
  binary prevents recurrence; existing damage stays.
- **Phase E.3 unblocked.** Health command is doing its job — it
  detected a 5-month-old silent bug. The bug's cause is fixed;
  the residual already-damaged-heights work is a separate track.

## Decision log

- **2026-05-02 (this entry)** — Fleet HAVE_UNDO repair pass executed
  per operator's "pause E.3, repair fleet first" directive. 4/5
  nodes fully clean at tip; Dell + LA carry pre-existing
  metadata-absence damage tracked separately. Phase E.3 unblocked.
