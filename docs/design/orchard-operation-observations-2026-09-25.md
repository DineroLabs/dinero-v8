# Reversible Orchard wallet operation observations

Account advancement now records confirmation or conflict alongside the scanner,
address counters and pending-operation bytes in the same encrypted account
snapshot. This is staged wallet integration, not live RPC or node admission.

## Meaning and evidence

Confirmation means the frozen Ready transaction's txid occurs in the selected
block. The original signed bytes remain unchanged. A different valid transparent
witness can have the same txid; it does not change the confirmed effects.
Conflict means a different selected transaction consumes a reserved transparent
outpoint or action nullifier. The first conflicting transaction in block order
is recorded, with block hash and height. Ordinary transparent transactions count
as conflicts too. Reserved plans can be conflicted before proving finishes;
`SetReady` cannot publish a newly completed result while that conflict stands.

The account calls this logic only after its scanner has checked block identity,
ordered Orchard authorization coverage and the prepared state. The host must
still supply a fully validated selected block under its chain/wallet locks;
the scanner and observation code are not full block validation. Input/nullifier
lookup maps are constructed once per advancing block, avoiding a Cartesian scan
of every reserved input against every input in the block.

Snapshot version DNORAC02 adds bounded, ordered observation records, at most one
per existing operation. DNORAC01 remains readable without invented observations.
Restoring observations requires an authenticated selected-block callback and
recomputes the result from that block. The archival ChainDB account adapter
supplies this callback. Missing data, wrong branches and malformed observations
are errors; restored statuses are not trusted solely because SQLite had them.

## Reorg and persistence

Rewind requires a retained authenticated common ancestor (or the identical
checkpoint); same-height replacement must first rewind to the common ancestor.
Observations above that height are dropped. Earlier observations, issued-address
counters, reservations and all frozen Ready bytes stay. Full rescan drops derived
observations and rebuilds them while preserving operation identities. Dropping a
confirmation or conflict never establishes that a transaction is admissible or
should be relayed. Fresh node admission is mandatory.

As for the account scanner, the host stages the replacement account snapshot
with companion wallet updates inside one SQLite transaction, then publishes only
after commit. Account tests cover encrypted close/reopen, rollback of a proposed
rewind, committed rewind/reopen, unchanged signed bytes and address counters.

## Deliberate remaining limits

These observations do not release reservations, erase transactions, prune history
or automatically rebroadcast. The pending queue still has its existing 128-entry
bound. A separate durable archival/retention policy is required before a live
wallet can retire completed entries and operate beyond that bound; this slice
must not be presented as completing that work. Service integration, restart
coordination, ownership/selection, admission and production reorg qualification
remain required.

Tests include confirmation, transparent-input conflict, real honest note-spend
plans sharing a nullifier, conflict before proving completes, rewind followed by
confirmation on a replacement branch, old snapshot decoding, missing/wrong
selected-block evidence, malformed outcomes and encrypted persistence. These
use synthetic selected-block fixtures; historical transparent script validity
is a caller precondition in this component suite.
