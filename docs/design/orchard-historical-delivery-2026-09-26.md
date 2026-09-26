# Historical delivery after an Orchard origin

A reorg can cross below Orchard activation and then return to the same tip.
Neither the final tip nor successful-return callback counts identify all the
committed transitions. The whole-reorg intent retains planned bodies, but does
not establish which historical callbacks completed before a crash.

## Atomic recording

The actual stateful historical `ConnectTip` and `DisconnectTip` now hold the
activation mutex throughout preparation and transition. If the indexed Orchard
outbox already has an origin, they prepare a historical record before the
historical validator changes memory. Preparation checks selected domain, height,
body identity and Merkle root, existing head and the current canonical tip.
It performs no write. Ordinary historical databases without an origin continue
without delivery records; an orphaned first record is an error.

The prepared object is thread-affine and noncopyable. Its record and new head
join the existing synchronous canonical batch immediately before that batch is
committed. The head and tip must still equal the captured values. Stage failures
terminate because the historical validator may already have changed memory;
returning to normal execution would be unsafe. Abandoning a prepared or staged
but uncommitted batch changes neither tip nor delivery history. This is not a
general firewall against reentrant canonical writers, nor a repair of every
historical validation failure path.

DNOE01 keeps exact Orchard bytes. DNOE02 carries canonical historical `Block`
serialization, checked against its header and transaction Merkle root. Height
and the selected activation schedule determine which representation is allowed.
DNOH01 heads and existing sequence/digest cursors are unchanged. These are local
consistency checks, not consensus validity or malicious-database authentication.
Historical serialization is not a claim to reproduce every old accepted wire
encoding. Retained records include a down/up cycle even if the final tip repeats.

Runtime-disabled and unsupported CSN connection/rollback paths refuse a retained
delivery history. CSN bookkeeping refuses too; it is not an Orchard replay
implementation. Bootstrap shortcuts cannot advance a store with delivery history.
Mainnet activation remains unset. No notification provider is installed here.

## Qualification scope

The dedicated outbox lane uses temporary ChainDB stores and a generated mixed
fixture. It checks existing DNOE01 replay, historical connect/disconnect/connect,
abandonment, reopen, cursor pagination, repeated-tip transitions, missing heads,
profile mismatch, bad body identity and a stale canonical tip. Its historical
body is an identity fixture, not a historically validated block. Source build
and existing service lanes cover linkage/routing compatibility; these are not a
whole running daemon crossing activation with live wallet consumers.

Production durable consumers/checkpoints, coordinated wallet recovery, validated
activation history, full startup/replay/reindex, CSN/pruned support and load/disk
limits remain required. Existing records are retained without acknowledgment or
deletion. No queue submission constitutes durable wallet success.
