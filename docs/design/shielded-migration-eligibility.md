# External state eligibility and Utreexo relocation qualification

This extends the noninstalled stopped-datadir wrapper. It does not add an
operator command, daemon startup migration, NodeCore entry point, reset API or
activation setting. Normal consensus and forest restoration code are unchanged.

## Read-only SQLite eligibility

After native datadir ownership and byte-identical companion inventories are
established, inspect the original `blockchain/utxo` and optional
`blockchain/shielded_nullifiers.db`. The matching candidate bytes are pinned by
the existing companion contract. SQLite uses an escaped `file:` URI, read-only
and immutable flags, defensive mode and untrusted-schema mode. No recovery,
schema update, cache stamp, checkpoint or sidecar creation is performed.
Immutable reads are appropriate only within this stopped, clean-sidecar cohort;
they are not a substitute for the outer ownership checks.

Explicit budgets cover metadata row count, field size, VM steps per SQLite
connection and ancestry headers. VM accounting aborts at a 1,000-instruction
boundary before exceeding the requested budget. Quick-check, schema inspection
and queries share that connection's budget. SQLite/RocksDB cache allocation and
OS caching are additional resource costs, requiring real-copy measurements.

The initial supported UTXO SQLite schema is version 1 with an ordinary
`utxo_metadata` table. Wrong types, embedded NULs, duplicate/empty keys,
unsupported schema versions, unknown `assumeutxo_` keys and query/budget errors
refuse. Pending reorg/recovery, fatal state, active imports and incomplete or
unknown lifecycle states also refuse. This is an operational eligibility policy,
not a new consensus rule or a diagnosis that a live database is damaged.

An absent/disabled lifecycle is acceptable only without orphaned lifecycle/base
records. A fully-validated lifecycle requires its completion flag, canonical
base height/hash, consistent retained base markers and sufficient recorded
progress when present. Expected-commitment fields are syntax-checked here;
this parser does not rerun historical AssumeUTXO content validation.

Nullifier SQLite version 1 is an explicitly nonauthoritative cache. Version 0
is acceptable only when empty; populated unstamped stores refuse for separate
reconciliation. Unknown versions or unreadable schema/provenance refuse.
An absent cache is allowed because canonical shielded authority is separately
verified by the migration engine. No absent cache is created during inspection.

## Cross-database protected-base checks

Before opening the candidate writable, while holding the original RocksDB lock:

- Require the exact completed ChainDB promotion marker for a fully-validated
  SQLite base; SQLite's flag alone cannot establish promotion completion.
- Discover `wallet_snapshot_recovery_base_height` separately, and decode
  ChainDB's `prebase_coins/M:base` with its exact supported encoding. Orphaned
  pre-base records without a marker refuse.
- Reject future or conflicting base identities. Walk backward from the actual
  active tip through stored headers, checking header hashes and recorded heights
  against the walk. Match each declared base on that ancestry.
- Ignore persisted height-index answers for ancestry selection. A stale index
  does not override the active tip's hash-linked history.

These checks establish base identity, not checkpoint/replay availability. They
do not delete, normalize or move any protected checkpoint or replay record.

## Utreexo qualification added

Generated chains now contain real serialized forests, hash-linked headers,
checkpoints and per-block deltas, alongside nonempty shielded state. After
relocation, both original and candidate reopen through ChainDB. The production
`RestoreHistoricalForest` path reconstructs heights 2, 5, 7, 10 and 12; serialized
states and proof bytes match the continuously constructed forest. Membership
proofs verify, and a spent leaf loses current membership while its earlier
state remains reconstructible. Missing/corrupt delta cases refuse on both sides.

These are storage/reconstruction tests. They do not mine real blocks, validate
PoW/ASERT, execute live connect/disconnect or prove a deployed reorg safe. The
generated header chain is not a network identity fixture. Corrupt-delta tests
exercise the proof consumer's refusal; the wrapper does not yet perform a
complete forest reconstruction audit as a pre-migration eligibility gate.

## Verification and remaining gates

The executing `ShieldedMigrationCohort` suite now contains 85 cases in the normal
Tests lane. Local test-first evidence includes 31 new SQLite refusal failures
against opaque-file-only inspection, then six additional protected-base failures
after parsing alone. A missing namespace brace was a compile/setup failure,
not a RED assertion. Three initial forest-parity failures came from the generated
builder omitting the canonical-roots activation transition; the builder was
corrected to perform the real transition and exact byte assertions were retained.

Five compiling behavioral negative controls remove import refusal, legacy-cache
provenance, VM budget enforcement, the protected-base contract, or base identity
comparison. Each fails its targeted test. The existing 55-case inner engine
regression suite remains green. Linux qualification is required separately.

Remaining operator/release gates include network/profile binding, a required
checkpoint/forest reconstruction audit before migration, configured inputs
outside the companion inventory, binary/datadir rollback enforcement, disk and
real-copy resource qualification, and the actual migrated-daemon compact-proof
plus 60-second restart/reorg/mining run. Mobile ownership, resources and SR-1
remain separate. None of these are satisfied by a naked ChainDB READY marker.
