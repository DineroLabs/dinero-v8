# Utreexo checkpoint retention: offline copy gate

This is the first implementation stage. There is no daemon option, live
maintenance thread, or automatic compaction hook. All mutation experiments
must use an exclusively owned offline copy. Merging or using this on a live
seed still requires the owner's explicit approval and the gates below.

## Why

The September 14 seed inventory found 192.23 GiB (SJ) and 191.55 GiB (NA)
in the Utreexo column family, about 99.93% of SST bytes. Old consecutive
height-keyed full-forest snapshots coexist with newer 500-block snapshots.
Normal compaction preserves the distinct live keys. Reduced checkpoint
frequency has not reclaimed the dense history.

## Retention policy

Provisional copy-test defaults:

- Keep the earliest checkpoint, including a nonzero import base.
- Keep every existing checkpoint in the last 2,000 blocks, plus the latest
  checkpoint at or below that window boundary.
- In older history, keep the latest existing checkpoint at or below each
  5,000-block rung. A missing exact rung preserves its predecessor.
- Keep known snapshot/import/lifecycle anchors or their existing
  predecessors, even between regular rungs. The engine discovers the
  ChainDB pre-base marker; the copy harness also reads the separate SQLite
  AssumeUTXO active/lifecycle bases and the persistent wallet snapshot
  recovery base. Additional explicitly protected heights are supported.
- Keep all per-block deltas, transition proofs, spend positions, tip
  markers, UTXOs, headers, undo, and other families.
- Keep checkpoints at heights above the frozen tip.

This bounds checkpoint density and typical replay distance, not total
archive size independently of chain age. Historical proof latency at a
5,000-block replay distance is an open measurement gate, not an accepted
production default. A phone that does not serve historical proofs may
eventually use a different policy; this patch does not change phone storage.

## Deletion eligibility

For two consecutive retained checkpoints A and B:

1. Freeze the database tip. Require the forest-tip marker that ConnectTip
   commits with chain state to match its height, hash, and header root. If a
   legacy validated-tip marker exists, it must also agree; its presence is
   pinned for the pass. Current nodes do not write that legacy marker.
2. Walk header ancestry backwards from that tip. Require the persisted
   height index to agree at every relevant height; roots alone cannot
   establish ancestry. Imported histories can begin at their earliest
   available checkpoint.
3. Load A, validate its optional checksum and its forest commitment.
4. Replay every delta in A+1 through B, checking parent linkage, height
   identity, delta application, and each header's root. Do not skip a
   missing delta merely because an intermediate full checkpoint exists.
5. Compare the resulting canonical serialized state to the independently
   restored checkpoint B. This also checks the state needed for leaf
   position lookup and proof generation.
6. Recheck the frozen storage/forest tips, optional legacy marker, and actual A/B checkpoint
   bytes and checksums before each deletion batch.
7. Delete only `U+height` and `C+height` for A < height < B, paired in a
   synced RocksDB batch. Default maximum is 128 height slots per batch.

An invalid or missing replay interval is preserved in its entirety and
reported as skipped. Other complete intervals may still be processed.
A changed tip or retained anchor stops the pass. These checks supplement
the exclusive-copy requirement; they do not constitute a live-writer
concurrency protocol.

## Bounded work and restart behavior

The state machine checks up to 16 ancestry/replay heights per step, or
stages up to 128 deletion pairs. It discovers rungs with individual seeks;
it does not enumerate every giant checkpoint value. Deleting known integer
height slots may also create tombstones for absent keys. Reported slot
counts therefore are not counts of removed snapshots or reclaimed bytes.

A checkpoint load, forest deserialization, checksum, or database error
recovery can still take appreciable time. Count bounds do not guarantee a
wall-clock limit, which is why this engine is not called from ConnectTip.

Progress is in memory. After a process interruption, a new pass rechecks
the database. Any completed deletion batch leaves A, B, and all replay
material intact; intermediate deleted heights remain reconstructible.
No progress marker can accidentally skip verification after a crash.

Tombstones are logical deletion. Physical reclamation requires a separate,
explicit compaction on the offline copy after correctness checks. The
engine never triggers compaction implicitly.

## Copy harness

Build the test-only `utreexo_retention_copy` target. The source and copy must
already exist as separate, non-nested datadir roots. The harness does not
create a copy or open the source database. It rejects symlinks and hardlinks
inside the copy's database paths and uses the normal RocksDB exclusive lock.
The entire copy, including SQLite metadata, must remain offline throughout.

Example audit, with operator-supplied paths:

```sh
./build-retention/utreexo_retention_copy \
  --source-datadir /path/to/source-datadir \
  --copy-datadir /path/to/offline-copy \
  --network mainnet
```

Audit performs replay checks without deleting checkpoints. Opening the copy
through ChainDB may create logs, recover its WAL, or update its schema; audit
is not a byte-for-byte read-only open of the copy. The source is only checked
for path isolation.

After reviewing audit output, append `--apply` to delete verified redundant
checkpoint/checksum pairs on the copy. `--compact` additionally flushes and
compacts its Utreexo column family, only after a fully successful pass with
zero skipped intervals. Both options operate on the copy and require space
for RocksDB's normal compaction output.

Exit codes are 0 for complete, 2 for rejection/failure, 3 for skipped intervals,
and 86 for the explicit test-only `--crash-after-delete-batches N` injection.
An apply pass can commit safe earlier intervals before a later interval is
skipped or fails. Always inspect the reported status, skipped ranges, and
height-slot counts; process exit alone is not a disk-savings measurement.

## Local validation

The final storage run passed **29 retention tests**, **16 CLI tests**, and
**4 subprocess tests**. All **9 existing forest restore tests** also passed.
Six independent temporary mutations disabled ancestry-index checking,
legacy validated-tip identity checking, actual legacy anchor-byte binding,
automatic import-anchor preservation, production forest-tip agreement, and
pinned forest-root checking. Each corresponding test failed. The correct
implementation was restored, rebuilt, and all 29 retention tests passed again.

The first actual daemon run caught a fixture assumption: `getValidatedTip`
has no current production writer, so requiring that old marker rejected a
valid stopped database before any deletion. The final engine requires the
production `ForestTipMarker` and validates its header commitment throughout
the pass. An existing legacy marker must still agree. Missing production
markers are refused, never repaired or silently created by retention.

The real subprocess tests use synthetic RocksDB data. They verify actual
lock contention without replacing the lock file, then force `_Exit(86)`
after a synced three-height deletion batch. A new process restores all 33
historical states and every live-leaf proof byte-for-byte. Resumed apply and
explicit compaction preserve those results. Source bytes, inodes, modes,
sizes, and modification times remain unchanged. Other cases protect the
height-only wallet recovery marker and reject invalid metadata.

A missing delta at height 8 causes exit 3, reports skipped interval `[0,10]`,
preserves every checkpoint there, and suppresses requested compaction. Later
complete intervals still prune, with all historical states/proofs reproducible.

### Actual daemon gate

`CheckpointRetentionDaemon` passed in **95.18 seconds** using the locally
built daemon, loopback regtest peers, and a stopped independent datadir copy:

- Mine a real spend at 111 and extend to 150 with dense checkpoints.
- Apply retention on the copy; explicitly prove `U110` and `C110` were deleted.
- Restart the pruned bridge and compare its entire internal forest
  serialization plus full UTXO summary to the original.
- Interrupt a fresh CSN at height **21 of 150**, perform two offline restarts,
  and resume it against the pruned bridge.
- Require log evidence for that exact historical spend block being served
  and its proof validated; confirm client tip, roots, and commitment match.
- Disconnect **40 blocks** to fork height 110; compare full forest and UTXOs
  to their original values before the spend.
- Mine a longer **41-block competing branch** to 151, converge the CSN, and
  restart the bridge with identical full forest/UTXO state.

These are local regtest results with complete replay material. They do not
measure old production delta coverage or a 5,000-block proof-serving latency.
Evidence logs and binary hashes are retained with the September 14 investigation.

## Remaining gates before live integration

- Run the first real reclamation on an offline copy of a seed datadir. Record
  source/copy provenance, protected anchors, skipped ranges, before/after
  SST allocation, and historical proof latency after compaction.
- Select the production retention spacing from those measurements. Missing
  historical deltas may require a separate block-replay migration; this pass
  preserves such intervals and cannot promise to reclaim all 190 GiB.
- Design and review live maintenance synchronization and latency separately.
  This patch exposes no live hook and does not bound wall-clock latency.
- Obtain the owner's explicit approval before merging or touching live seeds.

No real seed data was pruned or compacted during these local gates.
