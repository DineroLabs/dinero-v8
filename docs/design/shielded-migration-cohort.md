# Stopped-datadir ownership and companion binding

This is the next qualification layer around the shielded relocation engine. It
is linked only into a noninstalled test executable. It does not supply a daemon
startup mode, operator command, NodeCore API, selective reset or launch permit.

## Enforced boundary

`MigrateShieldedDatadirCopy` takes two existing disjoint native datadirs. It
acquires the same `dinerod.lock` that `DatadirGuard` uses, on both originals and
candidates, without creating lock files or changing PID files. These outer
leases outlive the inner engine's actual ChainDB locks, handle destruction and
final verification. They also exclude another owner inside the same process.
No C++ mutex is held across a foreign async call; no caller can replace lease
acquisition with a running-status query. iOS is refused.

Paths reject symlinks; directory and lock identities are pinned and rechecked.
Lock descriptors are close-on-exec. An exception or process exit releases the
OS lock, while the inner durable journal retains the incomplete operation.
This excludes cooperating native daemon/migration entry points, not privileged
filesystem mutation or arbitrary programs that ignore the ownership contract.
It does not qualify embedded NodeCore's separate lifecycle/maintenance protocol.

## What the companion inventory covers

The wrapper inventories `blockchain/` except the inner `chaindb/`, plus `blocks/`,
`headers/` and `checkpoints/`. Existing files, directories and absent optional
roots are recorded. Unknown files inside these roots are retained and compared.
Files must be regular, not symlinks or hardlinks. Budgets bound entry count and
total bytes read; hashing streams through a 64 KiB buffer. Path/entry metadata
and RocksDB allocation remain additional resource costs.

Original/candidate inventories must match by relative path, type, size and
SHA-256 content. Each side's identities, timestamps and membership are rechecked
at inner migration boundaries. Full bytes are rehashed before READY and after
the inner handles close. Wallet directories, keys, identities, runtime PID
files, logs and other top-level operational files are not inspected or changed.
External snapshot paths outside these four roots are not covered by this layer.

Presence of `chainstate_recovery.marker`, `blockchain/reindex_promotion.marker`,
an `.reindex.tmp` companion, or the sibling `.nodecore-maintenance-v1` control
directory causes refusal. This intentionally does not reinterpret a maintenance
prototype record as permission to migrate. Nonempty SQLite `-wal`/`-journal`
companions also refuse; the migrator never checkpoints or recovers them itself.
An empty sidecar is retained. The [eligibility extension](shielded-migration-eligibility.md)
now inspects SQLite lifecycle and provenance read-only after this byte inventory.

## Resume and publication

The operation hash additionally binds the two outer paths/directory identities,
lock identities and original companion inventory digest. This makes changing
both copies identically insufficient to resume an interrupted operation. The
inner unbound ChainDB-only API cannot resume a journal created by this wrapper.
Relocation's existing record set, synchronous batches and replay/checkpoint
preservation remain unchanged.

READY still describes **only** the completed ChainDB relocation. A companion
change observed after READY publication reports failure, even though that
durable marker exists. Consumers must not treat a naked READY marker as a
release cutover receipt. Subsequent changed-cohort attempts are refused by the
operation binding. No caller/launcher is added by this change.

## Executing tests

`ShieldedMigrationCohort` is registered in the normal Tests lane as
`storage;smoke`, with a 180-second timeout. Its generated datadirs exercise real
RocksDB, production `DatadirGuard`, same-process and exec-child contention,
mid-migration process exit/resume, modified companion pairs, lock replacement,
recovery barriers, file aliases and resource-budget refusal. Wallet sentinels
and PID bytes remain untouched. Companion payloads are deliberately opaque
fixtures, not real SQLite/headers/blocks or proof-equivalence evidence.

Local test-first evidence: the previous engine passed five controls but failed
25 of these assertions. The wrapper passes all 30; removing the datadir lock,
companion digest binding, recovery barrier or companion recheck causes its
targeted behavioral test to fail. All 55 existing engine cases still pass.

## Eligibility extension and remaining release gates

The [eligibility extension](shielded-migration-eligibility.md) adds bounded
SQLite inspection, protected-base ancestry checks and actual Utreexo
reconstruction/proof comparisons on generated migrated stores. The original
30 cases above remain; the expanded suite has 85. Its limits remain explicit.

- Bind the selected network/profile and canonical ancestry. Mainnet/regtest
  share a genesis hash, so a genesis check alone is not a network proof.
- Extend the implemented protected-base discovery/ancestry checks with a
  pre-migration reconstruction audit of required forest states and proofs.
- Bind external configured snapshot inputs, binary/datadir pairing, rollback
  selection and disk-headroom policy in a qualified operator tool.
- Qualify real stopped copies, Linux and relevant filesystem faults, then
  actual daemon restart/reorg/mining with compact proofs and 60-second rules.
- Complete mobile resource/lifecycle qualification and SR-1's separate
  consistency-preserving recovery API before mobile cutover.

The owner-approved target remains one combined release. This layer advances
that implementation without authorizing retention, whole-CF deletion or a
whole-directory wipe. Required checkpoints/import anchors remain protected.
