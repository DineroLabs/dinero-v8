# External state eligibility and Utreexo relocation qualification

This extends the noninstalled stopped-datadir wrapper. It does not add an
operator command, daemon startup migration, NodeCore entry point, reset API or
activation setting. Consensus rules are unchanged. Forest restoration exposes a
read-only view; the existing ChainDB API delegates to the same replay implementation.

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

These identity checks are followed by the forest audit described below. They
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
exercise the proof consumer's refusal; the subsequent pre-migration gate is described below.

## Verification and remaining gates

The executing `ShieldedMigrationCohort` suite now contains 111 cases in the normal
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

Remaining operator/release gates include network/profile binding, configured inputs
outside the companion inventory, binary/datadir rollback enforcement, disk and
real-copy resource qualification, and the actual migrated-daemon compact-proof
plus 60-second restart/reorg/mining run. Mobile ownership, resources and SR-1
remain separate. None of these are satisfied by a naked ChainDB READY marker.


## Pre-migration forest reconstruction gate

The bound stopped-datadir wrapper now audits the already-owned read-only
original before any writable candidate open, on inspection, initial migration,
and resume. The unbound ChainDB-only component remains a lower-level test
component; it is not an operator entry or a way to bypass the bound journal.

The audit inventories every U/C key with exact length, bounded count and record
size; malformed, future or orphan checksum rows refuse without deletion. A
present checksum must match SHA256 of its checkpoint. Legacy checkpoints without
checksums remain eligible only through the forest/header/replay checks. v2/v3
checkpoint framing and delta counts are bounded before decoder allocations;
older unsupported encodings refuse for separate review, never normalization.

Header hashes and recorded heights are checked along the active tip's parent
chain down to the earliest retained checkpoint. Persisted height indexes cannot
override that ancestry. The audit restores the earliest checkpoint using the
shared production algorithm, replays every interval, checks every replayed
header commitment, and compares complete normalized serialized forest states at
each later retained checkpoint. It then replays to tip and checks the forest-tip
marker root. An empty genesis checkpoint must have empty framing; protected
base heights may not fall below the audited reconstruction range.

`ForestRestoreView` is a read-only interface to the existing algorithm. The
normal `ChainDB` overloads forward through an adapter; no new replay algorithm,
consensus rule, mutable DB open, recovery action or deletion permission is
introduced. All bytes, including optional legacy checkpoints, stay unchanged.

Budgets explicitly cap checkpoint count, record bytes, leaves, ancestry headers
and replayed blocks. This bounds inputs/work; it is not an RSS guarantee.
Multiple forests/serialized buffers, database caches, and allocator overhead
still require real-copy and device measurements. The audit covers the retained
range from its earliest checkpoint to tip; it does not establish history before
that range or authenticate a network/PoW chain. Identity, configured external
snapshot inputs, rollback enforcement, real stopped-copy measurements and actual
migrated-daemon compact+60-second restart/reorg/mining remain release gates.

The test-first extension has 15 failing refusal assertions and 88 controls on
the preceding implementation. The existing normal ChainDB forest-restore suite
passes all nine cases after the shared-reader extraction. Local and Linux
qualification results for the final extension are recorded separately in its
PR/evidence; earlier CI successes do not qualify these new bytes.

Final native qualification: 111/111 cohort cases in 107.79 seconds under the
unchanged 180-second CTest budget, 55/55 engine cases and 9/9 existing
ForestRestore cases. Five compiling negative controls fail as intended. An
earlier concurrent harness run timed out; the identical binary subsequently
passed in 145.38 seconds, then duplicate fixture construction was removed
without changing assertions or migration durability. No runtime deadline was
extended. Linux qualification remains separate.
