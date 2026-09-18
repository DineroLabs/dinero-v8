# CSN snapshot historical restore qualification

## Failure and storage change

This follows the proof/restart fix in #773. An imported snapshot has a verified
header and forest checkpoint, but deliberately has no pre-base canonical height
index or local base block body. CSN invalidation and reorg restore incorrectly
required those records. Correcting the height lookup exposed a second failure:
stored-body CSN forward/recovery connections did not persist every Utreexo delta.
Sparse-checkpoint reconstruction therefore stopped at the first missing delta.

The field replay also exposed a reconsideration gap: the separate header store
can extend beyond downloaded bodies. Such descendants may have no ChainDB metadata
row to clear, and the last eligible body-backed block is not a leaf of the header
tree. Reconsideration previously aborted at the missing row or failed to offer
that body-backed frontier as a candidate.

The fix:

- Resolves checkpoint/replay hashes through the caller's authoritative block-index
  ancestry for manual invalidation, ABC-CSN reorg and checkpoint rewind. An
  unresolved ancestry lookup fails; it does not fall back to another branch.
- Checks the destination forest against its persisted header. The blocks being
  disconnected still require their own bodies and undo records.
- Captures addition deltas in stateless stored-body validation and deletion/addition
  deltas before proof-based replay. Replay still validates the resulting root.
- Commits those deltas atomically with coins, shielded state and the canonical tip.
  The already-applied worker path reuses its existing decoded delta. The commit
  guard now requires a delta for stateless connections too.
- Reconsideration stages complete known metadata for a header-only descendant
  whose metadata row is absent, without granting body/undo flags. Missing metadata
  for a data-bearing block still fails. Candidates are offered parent-first through
  the unchanged eligibility and invalid-ancestry gates, so unavailable descendants
  do not hide the connectable tip. Status clearing and the decision-generation
  change remain in the same atomic batch under the activation lock.

No consensus rule, proof/hash format, column family, snapshot format, activation
height or checkpoint-retention policy changes. Missing/corrupt reconstruction
material remains an error. This does not backfill historical deltas absent from
an existing damaged database; qualification below uses a fresh snapshot replay.

## Test-first evidence

`CSNSnapshotHistory` extends the existing snapshot proof fixture and is explicitly
selected by the serial daemon lane in `tests.yml`:

```sh
CSN_SNAPSHOT_HISTORY=1 DINEROD=/absolute/path/to/dinerod \
  python3 tests/integration/test_csn_snapshot_proof_restart.py
```

A snapshot at 130 includes a coin spent in block 131. The CSN receives blocks
through 138 with checkpoint interval 1,000, then invalidates to 136 and to 130.
Each cycle restarts while disconnected, reconsiders the branch, and restarts
again. Requirements include exact full-node tip/root agreement, no safe mode,
single and batch proof availability, altered-sibling rejection, resurrection of
the spent base coin on rollback and its rejection after reconnection.
The copied header store extends to 141, while only bodies through 138 are supplied.
The active tip must stay at 138 after reconsideration, despite those extra headers.

Observed RED sequence:

1. Unchanged #773 daemon: `restore-missing-height-index-at-checkpoint-130`.
2. Ancestry/header correction alone: `replay-missing-delta-sidecar-at-131`.
3. Focused unit assertions fail when forward validation omits the delta and when
   the stateless commit guard permits a missing delta.
4. Field reconnect fails at header-only height 112474; the extended synthetic
   fixture reproduces at height 139 before the reconsideration fix.

Native macOS qualification includes `BlockValidationInvariants`,
`ChainstateCommitBatchInterval`, `UtreexoDeltaCodec`,
`ForestDeltaReplayEquivalence`, `ForestRestore`, `CSNSnapshotHistory`,
`CsnManualInvalidation`, `CsnSpendReorgReconciliation` and
`CSNShieldedReorgInvertibility`. The existing restore suites include missing and
corrupt records, wrong header roots, stale indexes and failed ancestry resolution.
The shielded suite includes repeated reorgs, nullifier rejection and epoch reset.
The reconsideration change also runs against `InvalidityRestartSticky`,
`GenerationToctouBarrier`, `ReconsiderRestartBoundary`, `HeaderStatusBits` and
`HeaderStatusOverwriteAllowlist`.

The local execution-map check selects the new test from `tests.yml`. A full local
coverage-gate run is not equivalent to Linux CI: this macOS compact-enabled build
registers additional platform/feature tests and lacks the Linux network-namespace
test. No baseline exemption was added; the Linux run remains the full gate.

## Remaining qualification boundaries

Linux CI, rebuilt iOS NodeCore/device qualification, promoted-snapshot lifecycle
coverage and concurrent proof/reorg stress remain separate gates. The preserved
phone blocks end at 112473, before the first reported mismatch at 112485. A clean
restart test does not establish power-loss safety at every reorg boundary.

These results authorize no phone reset, seed deployment, release activation or
column-family relocation. Required import checkpoints and verified-reconstruction
deletion rules remain intact.
