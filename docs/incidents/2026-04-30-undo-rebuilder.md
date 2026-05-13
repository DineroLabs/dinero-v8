# Undo Rebuilder — LA Range-Dependent Failure

- **Date:** 2026-04-30
- **Severity:** P2 (no chain corruption; an explicitly-authorized recovery
  tool failed to repair holes; live tips were never at risk)
- **Resolution commit:** `3056607b9`
- **Regression guard:** `72269138a` (Property #5 in
  `tests/integration/test_rebuild_undo_range.sh`)
- **Scope:** Affected only `--rebuild-undo-range` write-mode runs whose
  preflight discovered ≥1 hole. The live consensus path is independent
  of this code and was never invoked.

## Tl;dr

The Apr 30 fleet audit found 753 missing-undo holes across MO/LA/VA.
The new offline rebuilder (commit chain `5771646b7..27a819b96`) repaired
CN cleanly (333 holes). LA's run aborted at `h=7187` with
`reindex-missing-utxo`, but only when the request range contained any
holes — the same range minus the hole heights succeeded.

Root cause was an early-return in the hole-only optimization
(`9d892f2fb`): `processBlock`'s Step 5b `return Status::Ok` for
already_ok heights skipped Step 6's temp-DB `putCoin`. With a non-empty
hole whitelist, the temp ChainDB never accumulated UTXOs from already_ok
heights, so subsequent in-window holes failed to look up their prevouts.

Fix `3056607b9` replaces the early return with a fall-through `else`
that gates only the LIVE writes; Step 6 always runs.

## Timeline

| Time (UTC, 2026-04-30) | Event |
|---|---|
| audit run | `dinerod doctor` flagged 753 missing-undo holes across MO (≈204), LA (518), VA (276), CN (333). All four servers were on `efbc5b63a` (Apr 30 fleet baseline). |
| early afternoon | Authorized "real reconstruction pass". Rebuilder design constraints set: extend existing `BlockReindexer` (no fork), use `ChainstateCommitBatch` only at LIVE writes through verified path, mandate DisconnectBlock-roundtrip verification before any LIVE write. |
| `5771646b7..fca83937a` | Reindexer extended with `Mode::WINDOWED_UNDO_ONLY`, scratch DisconnectBlock-roundtrip verification, anchored seed, tolerant ReadDiskBlocks. Daemon CLI flag `--rebuild-undo-range=A:B [--rebuild-undo-write] [--rebuild-undo-anchor-height H]` + orchestrator wiring. Crash oracles + integration suite shipped. |
| `9d892f2fb` | "Hole-only optimization": restrict LIVE writes to heights the orchestrator preflight tagged as `Hole`. **This commit is the source of the bug.** |
| `27a819b96` | Fix A: truthful per-height write attestation (`live_undo_write_success_heights`). Caught a separate manifest-truthfulness gap unrelated to this incident. |
| `272630e67` | Fix B: anchored canonical chain selection (walk backward from known LIVE tip). Required to repair LA whose 52,683 stored block records included orphans the legacy chainwork search couldn't filter. |
| CN run | Range `1:13242 --rebuild-undo-write` succeeded. 333 rebuilt, 0 verify_failed. Live CN restarted on rebuilt copy, soaked, advanced normally. |
| LA run #1 | Range `1:10783 --rebuild-undo-write` aborted at `h=7187` with `reindex-missing-utxo` looking up prevout `002d901aaa…d0c4:0`. Manifest reported `final_status=incomplete`, `skipped=1`, `rebuilt=0`. |
| LA diagnostic | Range `1:7187 --rebuild-undo-write` succeeded. Same canonical chain prefix, but a *different result*. Three diagnostic commits added to narrow the divergence: `33124ef15` (per-height forest trace), `167ba749e` (verifier-phase forest trace), `54f29ff35` (env-gated chain dump). |
| late evening | Forest trace + verifier trace + chain dump diff confirmed: forward forest replay byte-identical, verifier output identical, canonical chain prefix `[1..7187]` identical. The only remaining variable was Step 5b control flow. |
| `3056607b9` | Fix shipped. Replaces `return Status::Ok` with fall-through `else`. ctest `undo-rebuild|reindex` 10/10 green. |
| LA run #2 | Range `1:10783 --rebuild-undo-write` succeeded on the bug-fixed binary. 518 rebuilt, 0 verify_failed. |
| VA run | 276 rebuilt, 0 verify_failed. |
| `72269138a` | Property #5 regression guard committed. Empirically reproduces the LA failure when the fix is reverted (verified by running the test against `54f29ff35`-content reindexer.cpp — fails with byte-identical manifest signature `final_status=incomplete, skipped=1, rebuilt=0`). |

MO was excluded from this rebuild — its height-index drift is a
different failure mode (different fix path) and is tracked separately.

## Root cause

`src/consensus/reindexer.cpp::processBlock` in `WINDOWED_UNDO_ONLY` mode
runs roughly:

1. Read block bytes from disk.
2. Verify forest before connect.
3. Roundtrip-verify rebuilt undo against scratch DisconnectBlock.
4. Append rebuilt undo to LIVE rev*.dat.
5. **Step 5b — hole-only optimization gate**.
6. Call `chain_db_->putCoin(...)` to populate the temp DB so subsequent
   blocks can look up their prevouts.

`9d892f2fb` introduced Step 5b as:

```cpp
if (!window.hole_heights_to_rebuild.empty() &&
    window.hole_heights_to_rebuild.find(height) ==
        window.hole_heights_to_rebuild.end()) {
    g_logger.info("...verified clean; skipping LIVE writes...");
    return Status::Ok;     // <-- bug: returns from processBlock
}
// LIVE writes (writeUndo + putHeaderMetadata)
// Step 6 putCoin loop
```

The intent of Step 5b was "for this in-window already_ok height, don't
re-write the LIVE rev*.dat or LIVE header metadata — its bytes are
already correct." But `return Status::Ok` returned from the entire
function, skipping Step 6's temp-DB UTXO mutation.

This was harmless when the whitelist was empty (the gate never fired
and Step 6 always ran), but on every range with at least one hole, the
already_ok heights left the temp DB unpopulated. The first in-window
hole or in-window non-coinbase block whose prevout was created by an
in-window already_ok height then failed:

```
h=7187 reindex-missing-utxo prevout=002d901aaa…d0c4:0
```

That prevout was the coinbase of an earlier already_ok block — never
inserted into the temp DB because Step 5b's `return` aborted before
Step 6.

## Why it survived the existing test suite

`test_rebuild_undo_range.sh` Property #4 verified the LIVE-side
contract: rebuild a window with a single hole and one already_ok block
on each side; assert the already_ok blocks' undo metadata is
byte-identical pre and post. That property held because Step 5b's
LIVE-skip logic was correct — and Property #4's 14-block fixture was
coinbase-only, so no in-window block ever needed to look up an
in-window prevout. The temp-DB invariant was never exercised.

Property #5 (added in `72269138a`) closes the gap: it mines past
coinbase maturity, executes a wallet `sendtoaddress`, mines the
confirming block (which has a non-coinbase tx whose prevout is an
in-window coinbase), and punches a hole only at the spending height.
With the bug, the rebuild aborts at the spending height with the same
manifest signature LA produced. With the fix, all heights process
cleanly.

## Diagnostic chain (what worked)

The diagnosis hinged on a range-dependent failure: same canonical
chain prefix, different result depending on whether the request range
included other holes. That ruled out:

1. **Forest divergence** — `33124ef15` instrumented per-height forest
   roots in the forward replay. Identical between the two ranges.
2. **Verifier divergence** — `167ba749e` extended the forest trace
   into the DisconnectBlock-roundtrip verifier phase. Identical.
3. **Canonical chain divergence** — `54f29ff35` dumped the resolved
   canonical chain prefix. Byte-identical for `[1..7187]`.

With the three obvious sources of divergence ruled out, the only
remaining variable in `processBlock` was Step 5b's control flow — the
one piece of code whose behavior depends on
`window.hole_heights_to_rebuild` being empty vs non-empty. Walking
through Step 5b with that lens immediately surfaced the bug.

User feedback during the diagnostic: "Claude didn't quite give up,
but he stopped one layer too early." The forest-trace + verifier-trace
+ chain-dump triad came in after that nudge.

## Fix

Commit `3056607b9` replaces the early return with a fall-through:

```cpp
const bool skip_live_writes_due_to_whitelist =
    !window.hole_heights_to_rebuild.empty() &&
    window.hole_heights_to_rebuild.find(height) ==
        window.hole_heights_to_rebuild.end();
if (skip_live_writes_due_to_whitelist) {
    g_logger.info("...verified clean; skipping LIVE writes...");
    // Fall through — Step 6+ MUST still run to populate
    // the temp DB for subsequent blocks' prevout lookups.
} else {
    // LIVE writes (writeUndo + putHeaderMetadata)
    stats_.live_undo_writes_committed++;
    stats_.live_undo_write_success_heights.push_back(height);
}
// Step 6 putCoin always runs
```

The whitelist now gates ONLY the LIVE write block. Step 6 always runs.
The `live_undo_write_success_heights` cross-check (`27a819b96`) means
the orchestrator can no longer report "rebuilt" for a height that
didn't actually receive a LIVE write — even if processBlock later
proceeds through the temp-DB path.

## Outcome

| Server | Holes | Rebuilt | verify_failed | Notes |
|---|---|---|---|---|
| CN | 333 | 333 | 0 | Repaired via legacy path before bug was found. |
| LA | 518 | 518 | 0 | Repaired via fixed binary (run #2). |
| VA | 276 | 276 | 0 | Repaired via fixed binary. |
| MO | (separate) | — | — | Height-index drift; different fix path. Excluded. |

All three repaired servers soaked ≥1h between rebuilds and re-mined
normally.

## Lessons

1. **A new code path needs a test that exercises every internal
   invariant, not just the contract the path is documented to satisfy.**
   Property #4 verified the LIVE-byte invariant. The temp-DB invariant
   was implicit and untested. Property #5 makes it explicit.

2. **Range-dependent failures point at control-flow conditional on
   inputs that vary across ranges.** When the same canonical chain
   prefix produces different results, the divergence is in code whose
   behavior depends on something else in the call. In this case:
   `hole_heights_to_rebuild`.

3. **Diagnostic instrumentation is cheap; commit it.** The three
   diagnostic commits (forest trace, verifier trace, chain dump) are
   env-gated and have zero overhead when off. They cost nothing to
   leave in the tree and will pay off on the next reindexer
   investigation.

4. **`return` vs `continue` vs fall-through is a class of bug.** Step 5b
   wanted "skip LIVE writes for this height"; it wrote "return from
   processBlock". The structure of `processBlock` (LIVE writes followed
   by temp-DB writes) made the impact non-local. A linter or comment
   pinning each early return to "this skips X, Y, Z that follow" would
   have surfaced this at review.
