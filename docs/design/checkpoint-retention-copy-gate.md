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

1. Freeze the database tip and require its validated tip to match.
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
6. Recheck the frozen storage/validated tips and actual A/B checkpoint
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

The initial local run passed all 23 retention storage tests and all 9 existing
forest restore tests. Four independent temporary mutations disabled ancestry
index checking, validated-tip identity checking, actual legacy anchor-byte
binding, and automatic import-anchor preservation. Each corresponding test
failed. The correct implementation was restored, rebuilt, and all 23 retention
tests passed again. Test and build logs are retained with the September 14
investigation artifacts.

The copy harness also passed 16 CLI rejection tests and a real subprocess
gate on a synthetic database. A second process held its actual RocksDB lock;
the harness refused to open it without replacing the lock file. After a forced
`_Exit(86)` following the first synced three-height deletion batch, a new
process restored all 33 historical states and every live-leaf proof
byte-for-byte. A resumed apply and explicit compaction preserved those results.
Source file bytes, inodes, modes, sizes, and modification times were unchanged.
Separate subprocess cases protect the height-only wallet recovery marker and
reject invalid or above-tip metadata. These fixtures exercise storage and
proof reconstruction; they do not establish daemon branch activation or live
network proof service.

The final four-case subprocess suite also removes the delta at height 8.
Applying retention with compaction requested exits 3, reports the skipped
interval `[0,10]`, preserves every checkpoint in that interval, and performs
no compaction. Later complete intervals still prune. Every historical forest
and live-leaf proof remains reproducible, and the source fingerprint is
unchanged. The 16 CLI tests and all 4 subprocess tests pass together.

## Required gates before live integration

- Storage tests: sparse/recent/import preservation, invalid replay refusal,
  header/height identity, anchor changes with and without checksums, paired
  atomic deletes, bounded work, and every-height state/proof equality after
  interruption and reopen.
- Neuter verification: temporarily remove safety conditions and confirm
  their corresponding tests fail, then restore and rerun the suite.
- A real process crash after a synced deletion batch, followed by recovery.
- Restart a daemon mid-sync after processing its stopped copied data.
- Activate a genuinely competing branch crossing a deleted checkpoint
  region; verify resulting state, restart, and subsequent block processing.
- Serve and verify historical bridge proofs at deleted checkpoint heights.
- Run the first real reclamation on a copy of a seed datadir. Record
  source/copy provenance, protected anchors, skipped ranges, before/after
  SST allocation, and historical proof latency after compaction.
- Review the live maintenance synchronization and latency design separately.

Passing the storage suite is not a claim that the daemon, crash, historical
bridge, or seed-copy gates have passed. No live-seed operation is authorized
by the mere existence of this code or its test harness.
