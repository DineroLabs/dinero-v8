# Orchard parent-state transition and ChainDB staging

This is an integration component, not enabled block admission. No activation
height or historical validation rule changes.

## Boundary

`PrepareOrchardStateTransition` accepts only sealed combined authorizations,
an explicit network/domain and selected parent, and membership callbacks for
that parent's active history. It validates the persisted frontier encoding,
recomputes root and size, and requires exact height/hash linkage. The activation
block starts with no Orchard state and zero balance. Every later block requires
a parent, including blocks with no Orchard transactions.

In canonical transaction order it checks context, duplicate transaction/input
identities, anchor eligibility and nullifiers, derives transparent value flows
and checked fees, and appends every action commitment through the pinned tree.
Its immutable result owns the before/after state, nullifiers, flows and fees.
The draft anchor rule accepts the empty root or an active historical root;
roots newly produced inside this block are not added to the lookup set. All
nullifiers, including dummy-action nullifiers, are tracked. Storage resource
ceilings supplement rather than replace the block byte/weight rules.

`StageOrchardBlockUnderChainstateLock` checks ChainDB's committed tip, reads the
parent and membership records, and stages the sealed result into the caller's
RocksDB batch. Missing membership means false. Failed reads and inconsistent
persisted parent state remain local storage errors, not consensus-invalid peer
blocks. It never commits independently. Failed staging preserves caller bytes.

The host must hold the selected chain/coin lock from coin resolution and
combined authorization through commit. A write token, matching height or sealed
result is not evidence of a lock. Include all Orchard transactions exactly once,
validate interleaved transparent transactions and UTXO changes, and apply the
coinbase bound with these same validated fees. Coins, forest, tip, height/index
and ordinary undo must share the outer batch. Publish live state only after a
successful commit. Those production callers remain to be implemented.

## Executed component checks

- The funding fixture's derived root equals the independently saved anchor of
  its honest cross-address spend. Funding adds 5,000 una; the next spend leaves
  4,500 una, with two then four commitments and 666 una fees per transaction.
- Parent root/size/bytes/height/hash mismatch, missing post-activation state,
  premature existing state, unknown anchors, spent nullifiers, duplicate
  transactions, stale contexts and pool underflow are rejected.
- Lookup failures are separately typed. No failure mutates the input state.
- Real ChainDB: abandoned batch, fund/reopen, spend/reopen, disconnect, replacement
  branch, repeated spend, incorrect parent, duplicate staging and corrupt local
  frontier. These are generated temporary stores and synthetic honest proofs.
- Seven standalone and ten root CTest registrations pass locally on macOS;
  CI requires the exact inventories, including both new test executables.

The new transition, staging, ChainDB, Orchard storage and test translation units
also pass ASan/UBSan using their root build compile commands. Other dependencies
are uninstrumented; macOS leak detection is disabled. An initial manual build
with mismatched compiler/platform flags stopped during RocksDB fixture setup;
that failed log is retained and is not counted as qualification.

No full-node connect/reindex/reorg, wallet, miner, mobile, Linux or release
qualification is claimed by these component results.
