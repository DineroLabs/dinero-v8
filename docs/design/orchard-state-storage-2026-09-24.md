# Orchard block storage transaction

This is a **staging API in the real ChainDB**, not a connected Orchard block
validator. No production caller invokes it yet. The next integration must
produce its state through the pinned Orchard tree and validated transactions,
then use the existing ConnectTip/DisconnectTip outer write batch.

## Boundary

`stageOrchardConnect` and `stageOrchardDisconnect` require a ChainWriteToken and
a non-optional caller-owned RocksDB WriteBatch. They never commit independently.
The host must hold its chainstate writer lock from authenticated validation
through synchronous commit. It must stage UTXOs, forest, ordinary undo, indexes
and tip in this same batch, and publish in-memory Orchard state only after the
batch succeeds. A failed/uncertain write is not permission to advance memory.

Exactly one Orchard block transition can be staged in a batch. The API rejects
prior writes in its namespace instead of ignoring an earlier staged state while
reading committed storage. Other caller entries and savepoints survive rejected
operations. Current-state comparison is byte-exact, including frontier, pool,
height and block identity; it does not replace the writer lock. Readers needing
multiple coherent records also need the chainstate read lock/snapshot.

## Records and undo

All rows use the `O1` prefix inside the named `shielded_state_v1` column family:

- `O1S`: current state, version `DOS1`, height, block hash, anchor, pool value,
  leaf count and bounded opaque frontier bytes.
- `O1N || nullifier`: owning connected block hash. Lookup is keyed by the
  nullifier itself, independently of the historical pool's height-keyed rows.
- `O1A || anchor`: positive reference count across currently connected blocks.
  Repeated roots survive disconnection of one occurrence.
- `O1U || block hash`: version `DOU1`, complete previous/current state and a
  canonically sorted unique list of that block's new nullifiers.

Integers use explicit little endian and exact lengths. Trailing bytes, unknown
versions, out-of-bound fields and malformed records are corruption errors.
The 4,096-byte frontier and 16,384-nullifier limits bound storage records; they
do not define transaction or block consensus limits. Orchard leaf capacity is
bounded at 2^32. Frontier bytes are opaque here: root/frontier correctness is
the tree/consensus layer's responsibility, not established by storage parsing.

Connect rejects duplicate nullifiers within the block and already stored ones,
stale expected state, non-successor height, shrinking trees, and changed root
or frontier without a leaf-count change. Initialization refuses orphan Orchard
rows when its state record is missing. Disconnect requires exact tip/undo
agreement and matching nullifier owners before staging any removal. It restores
the entire previous state and removes only this block's additions. Disconnecting
the first Orchard block restores the absence of Orchard state.

The flow list is now mandatory: connect recomputes the pool counter from
authenticated transparent inputs, outputs and fees, starting at zero for an
absent Orchard state. The proposed stored balance must match. See
[the pool guard](orchard-pool-balance-2026-09-24.md) for its draft ordering rule,
independent arithmetic tests and remaining runtime caller obligations.

Only a verified separated shielded layout is accepted. There is no implicit
migration, no write into the legacy column family and no change to legacy rows.
This uses the existing named-comparator protection against older layouts; full
release upgrade/downgrade and matched wallet restore remain qualification work.

## Executed storage checks

`OrchardStateStorage` uses generated temporary RocksDB databases and opaque
frontier fixtures. It covers abandoned staging, commit/reopen, exact state
undo with companion coin/tip writes (the tip's write timestamp is separately
bounded to the restore operation), repeated roots, branch replacement,
nullifier removal/reuse on the replacement branch, stale/bounded-input
rejection, malformed state/undo/owner/reference records, and legacy-layout
refusal. An injected WAL append failure leaves all old records intact.
Separate subprocesses exit without destructors before or after synchronous
commit; reopening sees the respective complete old or new state. This is
process-loss evidence, not a power-loss or full-daemon crash qualification.

C++ ASan/UBSan passes with the new test, Orchard storage implementation and
ChainDB compiled under instrumentation. Linked RocksDB, consensus/crypto and
other supporting archives are uninstrumented; macOS leak detection is disabled.
A test-only source copy with nullifier removal omitted from disconnect fails
the exact-restore assertion. The production source is not changed for that
negative control.

The two existing shielded storage/scan tests remain green. Root Orchard CI now
requires the storage test as an eighth active lane, including in builds where
the cryptographic backend is disabled via the ordinary full test suite.
The nested RocksDB build exposes a positive job-count setting; this local build
and the root Orchard workflow set it to two, rather than allowing its old
hardcoded eight jobs to override the outer compiler budget.

## Still to implement

- Pinned Orchard frontier append/serialization and authenticated anchor rules.
- Validated block flow collection and proof-derived nullifier/commitment lists.
- Actual ConnectTip/DisconnectTip and replay/reindex integration, including all
  alternate storage funnels and startup consistency checks.
- Runtime parsing/admission, mempool/relay/assembly and activation rules.
- Wallet, witness maintenance and full lifecycle/platform/load qualification.

Anchor-reference existence alone is not an anchor eligibility rule. Pruning,
anchor-window policy, initialization of the empty pool and activation-boundary
undo must be specified and tested at the consensus layer. This API accepts
trusted state records; it is not a substitute for those checks.
