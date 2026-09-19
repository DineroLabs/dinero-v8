# Offline shielded-state relocation engine

This component implements the ChainDB-copy portion of the approved
`shielded_state_v1` migration. It is compiled into a noninstalled qualification
executable only. There is no daemon, NodeCore, RPC or operator CLI entry point.
It does not establish that a complete node datadir is safe to migrate or launch.

## Inputs and authority

The engine takes two existing, disjoint ChainDB directories: a preserved
original and a candidate containing identical logical records. It requires the
supported nine-family schema-4 original, canonical persisted shielded data,
consistent chain/forest/shielded marker height and hash, and a recomputed
shielded root/count. It rejects malformed nullifier keys or values, duplicate
nullifiers across heights, and absent or invalid required state. Unknown records
outside the selected domain are retained and compared, not inferred disposable.

Explicit byte, row, record-size and nullifier-count budgets bound copying and
shielded inventory. RocksDB caches/memtables and the existing accumulator's
temporary allocations are additional memory costs; these budgets are not an
iPhone memory qualification or a free-space reservation.

Original and candidate paths reject symlinks, nesting, aliasing and hardlinked
files. Real RocksDB locks cover both databases throughout inspection, read-only
to writable handoff, copying, readiness publication and handle destruction.
Directory and lock device/inode identities are rechecked at mutation boundaries.
The public entry point always uses the native Env, never iOS's no-op lock Env.
The engine refuses iOS use. Privileged filesystem mutation or unguarded tools
that ignore ownership are outside this exclusion contract.

## Exact move set

Only these records move from `utreexo` to `shielded_state_v1`:

- `N || height_be32 || nullifier32`, exactly 37 bytes with an empty value.
- `Mshielded_frontier`.
- `Mshielded_anchor_history`, retaining the full original persistence bytes.
- Optional `Mshielded_anchor_history_migrated_v1`; absence is preserved.

`meta/shielded_tip`, U/C checkpoints/checksums, replay targets, transition proofs,
undo/deltas, coins, headers, blocks and every other existing logical record stay
byte-identical. No checkpoint deletion, retention pass, reset or repair is part
of this operation. Rebuildable does not mean freely disposable.

## Durable progression

The operation is bound to both canonical paths and directory identities plus a
streaming inventory digest. The digest has explicit family/row boundaries and
length-delimited fields. Candidate comparisons additionally walk both directions
and compare actual key/value bytes, rather than relying on equal counts.

`meta/shielded_migration_v1` records the operation binding, source digest,
selected count, retired prefix length and phase. `meta/storage_layout_v1`
records the corresponding layout fence. Both control records are published
in the same synchronous WAL-backed batch.

1. **PREPARING:** persist intent before creating the destination CF.
2. **MOVING:** copy bounded batches to the destination, sync, read back every
   value, then retire only the verified old physical copies. Retirement and
   journal advancement share one synchronous batch.
3. **VERIFYING:** exhaustively compare the relocated and untouched records.
4. **READY:** atomically publish the completed journal and reader-compatible
   layout fence. This means the ChainDB relocation is complete, not that the
   complete datadir or release is qualified for cutover.

After interruption, identical duplicates are resumable. Conflicting copies,
missing data, unexplained destination records, a changed original, a substituted
candidate directory or inconsistent progress refuse without repair. Retired
records must match the exact prefix recorded in the journal. A READY operation
is idempotent only while the databases still match that operation; it is not
intended to be rerun after normal service has advanced the candidate.

The original is opened read-only. No original rows are retired. Generated-store
tests compare all original storage-file bytes before and after; diagnostic LOG
files and the lock file are explicitly outside that byte comparison.

## Executing qualification

`ShieldedStateMigration` runs in the normal Tests lane (`storage;smoke`). It uses
generated stores and real RocksDB. Child recovery uses fork followed immediately
by exec, avoiding reuse of inherited RocksDB thread pools after fork.

Coverage includes interrupted phases, byte-limited and row-limited batches,
nonempty and zero-nullifier states, absent optional marker, state corruption,
conflicting duplicate refusal, original/candidate identity changes, same-process
and cross-process lock exclusion, and reopening through the supported ChainDB
reader. I/O cases inject real WAL append/sync errors and a lost sync acknowledgement
at copying/retirement/completion transitions, then resume in a fresh process.
The injected Env is available only in the qualification build; the public engine
cannot select it. Process exits and injected errors are not a power-loss model.

## Outer datadir qualification layer

The [companion wrapper](shielded-migration-cohort.md) now holds the native
daemon locks and binds frozen external chain-file inventories into migration
resumption. It is also noninstalled. It preserves the distinction between
ChainDB READY and a qualified complete datadir; semantic external-state and
release eligibility checks below remain outstanding.

## Remaining gates before an operator tool or release

- Semantic qualification of the frozen cohort: external SQLite provenance,
  snapshot/lifecycle metadata, block/rev consistency and configured inputs
  outside the companion wrapper's inventory.
- Network/genesis binding, unfinished promotion/import/reindex/recovery refusal,
  protected-base discovery and Utreexo reconstruction/proof equivalence against
  the original. Matching forest-tip identity alone does not prove these.
- Disk-headroom policy, bounded resource measurements on real stopped copies,
  additional filesystem fault coverage and Linux qualification.
- Supported launcher enforcement of binary/datadir pairing. Rollback selects
  the preserved original with its matching binary; an older unguarded binary
  must never open a partially migrated or READY candidate.
- Actual daemon import, connect/disconnect, reindex, restart and reorg against a
  migrated nonempty store, then the combined compact-proof/60-second candidate.
- Physical-device resource/recovery qualification and SR-1's separate core
  recovery API/FFI. A new CF does not survive a whole-directory deletion.

These are remaining deliverables, not conditions the caller can bypass by
supplying a boolean assertion that a store is safe. Compact and timing activation
are unchanged by this component; the owner-approved target remains one release
containing all three changes after their combined qualification.
