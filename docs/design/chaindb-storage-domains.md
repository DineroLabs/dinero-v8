# ChainDB storage domains and layout migration

Status: C++ implementation plan. The opening-safety change in this branch does
not add column families or move records. Existing databases retain their format.

## Target layout

Keep the nine existing families and append two explicit storage domains:

| Family | Ownership |
| --- | --- |
| `shielded_state_v1` (new) | Canonical nullifiers, commitment-tree frontier, anchor history and its legacy-import sentinel |
| `utreexo_checkpoints_v1` (new) | Exact checkpoint/checksum records only |
| `utreexo` (existing) | Retained replay targets, transition proofs, journal and unrelated retained metadata |
| `meta` (existing) | Cross-state tips, lifecycle records, schema and migration control |
| `utxo`, `prebase_coins` (existing) | Transparent coin state and frozen snapshot coins |
| `default`, `blocks`, `headers`, `height`, `txindex` (existing) | Existing raw records, history and indexes |

This makes eleven families. Nullifiers, frontier and anchors share the initial
shielded family because they jointly represent shielded consensus state. Further
splits require a demonstrated retention, access or tuning benefit. Taproot and PQ
validation features do not by themselves require database families.

Shielded records move through named, restricted storage APIs. General Utreexo
metadata APIs must no longer be the route for shielded state. Keep persisted byte
encodings unchanged. Checkpoints and checksums keep their exact five-byte keys;
unknown/malformed keys are preserved and reported, never swept into a domain by
a broad prefix guess.

## Atomicity and deletion authority

A connected/disconnected block must keep the same atomic WriteBatch across
shielded state, checkpoint state, coins and tip markers. Storage separation must
not split one committed state transition into sequential writes. Snapshot import,
reindex, reconciliation, shielded epoch transitions and recovery need the same
explicit domain routing as normal block connection.

**Rebuildable does not mean disposable.** A checkpoint-only family grants no
permission to drop the family or clear every checkpoint. Required import bases,
retained anchors, deltas and verified reconstruction rules still apply. Physical
relocation removes an old copy only after verifying its durable replacement; it
must not remove any logical history or checkpoint as a side effect.

A shielded family is independent of checkpoint maintenance, not independent of
chainstate consistency. Chain identity, roots, heights and replay inputs remain
linked. This is a storage change, not a consensus-rule or proof-format change.

## Stage 1: safe opening (this change)

Before writable RocksDB::Open, while holding the actual database LOCK:

- Recognize only the existing eight-family legacy layout or complete nine-family
  layout by name. `prebase_coins` is the only permitted automatic append.
- Accept the existing native-uint32 schema value 4. A missing legacy schema may
  be initialized only after layout validation; malformed, older/unimplemented
  and future values are refused rather than overwritten.
- Reserve `meta/storage_layout_v1` as the future layout/migration fence. Any
  present value is unsupported by this preparatory reader, including READY or
  an empty value. No code in this stage writes that key.
- Resolve returned handles by name into internal slots; discovery order and
  family count alone never establish identity.
- Reject missing CURRENT in an otherwise populated directory. Never turn a
  damaged store into a fresh one implicitly.
- Keep DB/handles local until all opening work succeeds. A failed init leaves
  no readable/writable ChainDB object, including append/schema-write failures.

The preflight lock is held continuously through read-only inspection, writable
open and the lifetime of the DB. Path canonicalization closes same-process
symlink/relative-path aliases in RocksDB's pathname-based lock registry. This
uses the configured Env's locking guarantees; an Env that deliberately disables
locking does not gain exclusion from this wrapper.

Compatibility refusal leaves logical records, family membership, RocksDB sequence
and CURRENT/MANIFEST/SST/WAL contents unchanged. Diagnostic LOG/OPTIONS activity
is not part of that byte-invariance claim. I/O failure after an approved writable
open can change engine files; the guarantee there is closed ownership and safe
retry, not rollback of RocksDB's own recovery work.

## Stage 2: domain APIs and offline migration

The migration reader must explicitly recognize the target layout and its journal
states. Start with an opt-in tool on stopped copies. Normal daemon startup never
starts a migration, never services partially moved state and never guesses that
missing target records mean legacy fallback.

Required sequence: durable intent before CF creation; bounded byte-preserving
copy; destination read-back verification; synchronous source-delete/journal
advance; complete inventory and state-equivalence verification; READY publication.
Interrupt every boundary and prove resumability. Preserve the source/rollback
copy until qualification completes. Creation of a CF is not an atomic batch with
ordinary record writes and needs explicit interrupted-state handling.

Keep new-layout creation opt-in initially. Already-shipped binaries may open
unknown layouts unsafely, so metadata alone cannot guard them: rollback selects
the original datadir with its matching binary. Do not point an unguarded older
binary at migrated storage.

## Qualification

Opening regressions execute as `ChainDBOpenSafety` in the normal Tests workflow
(`storage;smoke`, no baseline exemption). Cases include named legacy/current
layouts, unsupported metadata and families, immutable rejection, injected read,
manifest and WAL errors, competing owners, path aliases and move/close ownership.

Migration qualification additionally requires:

- Exact inventory/bytes before and after, preserving unrelated records and every
  required checkpoint/import anchor; bounded memory and disk-full behavior.
- Every reconstructible Utreexo height retains identical forest serialization,
  leaf positions, roots and live-leaf proofs; spent proofs remain invalid.
- Nonempty shielded snapshot import, duplicate-spend rejection, valid spends,
  restart, reorg, epoch reset and real full/CSN daemon operation.
- Crash/fault tests of migration and cross-family atomic block writes.
- Explicit rollback behavior and resource/performance measurements.

Opening tests do not constitute migration or rollout qualification.
