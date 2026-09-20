# Recovering historical CSN replay metadata

## Problem and scope

Older CSN replay sidecars contain only ordered Utreexo leaf hashes. These do
not reveal the input values needed to check a block's exact transaction fees.
The exact-fee gate correctly refuses to guess. That is an operational replay
dependency; it does not invalidate previously accepted blocks.

Ordinary forward block download can already replace some missing metadata.
It does not cover every stored competing branch: its historical-response and
cursor rules can discard the old proof needed before ActivateBestChain may
safely rewind. Recovery therefore has a separate, bounded request queue.

This change does not alter transaction/block encoding, consensus activation,
subsidy, difficulty, Utreexo commitments, shielded rules, or storage layout.
It upgrades an existing per-block replay sidecar to its existing CSN2 encoding.
It neither deletes checkpoints nor resets a datadir.

## Recovery contract

1. Decode the whole existing sidecar. A malformed or already metadata-bearing
   record is not silently replaced. Only genuine legacy hash-only records with
   transparent inputs need this operation; zero-input shielded transactions do
   not acquire invented transparent inputs.
2. First try complete metadata in the stored proof payload, then the archival
   undo record. Match external inputs by outpoint and derive ephemeral inputs
   from earlier transactions in the stored block. A failed stored payload does
   not hide a usable undo record. Transient local failures may retry after the
   bounded backoff.
3. Otherwise request the exact stored block hash from a connected peer using
   the existing Utreexo-block protocol. This request does not participate in
   normal block-download inflight/window/cursor accounting.
4. Authenticate against local history: block hash, height, parent header,
   before/after header commitments, original ordered deletion targets, exact
   input accounting, reward, and existing maturity rules. A peer response must
   additionally pass cryptographic batch-proof verification. A local undo
   source still has to reproduce the authenticated forest transition.
5. Work on an owned scratch forest. Use the current forest only when its root
   already equals the requested parent. Otherwise restore the common ancestor
   and replay retained, hash-anchored deltas along the requested ancestry.
   Height-keyed checkpoints from a different branch cannot define that history.
6. Under the sidecar publication lock, compare the original record again and
   synchronously commit only its verified CSN2 replacement. Both existing
   normal CSN sidecar writers share this lock. No activation or network call
   runs while that innermost lock is held.
7. Retry ordinary branch activation. Its whole-branch preflight still runs
   before any canonical rewind. Replay and shielded bookkeeping must consume
   the same verified metadata even if an old embedded payload is empty.

A rejected response never marks the block invalid. No peer can choose the
local ancestry, replace the original targets, or move the live forest during
repair. v1 leaves retain their existing deferred-maturity semantics; recovery
does not claim that old hashes authenticate fields they never committed to.

## Scheduling, restart and bounds

The queue keeps at most four needs, each with bounded original-record and
proof storage. Proofs retain the existing 1 MiB wire limit; copied payload
objects and contents have a separate bound derived from the largest supported
wire-to-memory expansion (approximately 6 MiB per response on 64-bit builds).
This counts payload storage, not total process RSS or allocator overhead.
The separate limits allow valid proofs with many small input records; wire size
alone and decoded size are not interchangeable. There is at most one outstanding
attempt per hash, a 30-second
request timeout/backoff, peer rotation, and explicit NOTFOUND handling. Heavy
proof validation is done by the scheduler worker, not the receive callback.
While a hash is pending, wrong-peer, wrong-height, duplicate and stale replies
cannot advance the normal CSN download cursor. Changed records and resolved
abandoned/invalid candidates release
queue capacity. TCP and relay peer identities use the same transport key.

The queue is disposable process state. Unrepaired legacy records remain on
disk. Startup revisits stored candidates after StatelessNode has been wired,
including equal-height branches with greater work; successful repairs survive
restart as CSN2. Cancellation never commits unverified metadata.

This is not general damaged-database recovery. Startup with inconsistent
UTXO/shielded/forest state may fail before P2P starts and must retain its current
fail-closed behavior. Missing/corrupt checkpoint or delta history cannot be
replaced by guessing. A source must retain sufficient spent-output information;
hashes alone cannot reconstruct it. Those availability conditions remain part
of qualification on stopped copies of the rollout databases.

## Qualification

- Shared codec: legacy and CSN2 v4/v5/v6 byte compatibility; malformed lengths,
  counts, versions, trailing bytes and reused output objects.
- Queue: duplicate/concurrent needs, wrong peer/hash/height, timeout/backoff,
  response ownership, stale completion, capacity and restart re-enqueue.
- Scratch validator: exact-fee acceptance and overpay rejection; original
  parent/targets/roots; missing and damaged proofs; two input records for one
  external target in parent/child blocks; v1/v2 maturity behavior.
- Behavioral negative control: bypassing the required peer proof must fail the
  missing-proof test even though metadata and final root remain correct.
- Real PoW full-node/CSN test: establish both branches; stop the CSN and turn
  genuine records into legacy records while removing local proof/undo metadata;
  restart; show unchanged active tip/root while the source is offline; bring
  the source back without a second reconsider/reindex; require automatic repair,
  byte-stable target/transaction identities, durable CSN2, and valid Utreexo
  proofs after restart. A second variant retains only local undo metadata and
  must recover while the source peer remains stopped. Parent/child relay is checked after repair separately
  from the existing disconnect-cache issue discovered during fixture design.
- Every new registered test has an executing CI lane. Local diagnostic links
  against cached dependencies are development evidence, not clean release or
  cross-platform qualification. Linux CI and rollout-database coverage remain
  release gates.
