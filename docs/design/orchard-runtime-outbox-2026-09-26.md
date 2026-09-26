# Durable indexed Orchard delivery records

The service's indexed connect/disconnect owner now appends a delivery record in
its private synchronous chainstate batch. A process exit between that commit and
`PublishAfterCommit` leaves exact replay material. Abandoned preparation leaves
neither a chainstate change nor a delivery record. Rollback appends a disconnect;
it does not erase earlier connects. This closes a per-block persistence gap. It
does not install a notification provider or finish wallet/mempool recovery.

## Contents and integrity

The versioned local record contains a monotonically increasing sequence, the
previous record digest, direction, selected network/genesis/branch/activation,
block height/hash/parent and the exact mixed body including Utreexo data. A
SHA-256 digest covers its framing and contents. The versioned head stores the
last sequence/digest and its own checksum. These are local consistency checks,
not a consensus commitment or authentication against an adversarial database.
Records are outside DNRS and never establish proof/script validity.

The indexed owner verifies the previous head and final record, refuses occupied
next keys or sequence exhaustion, and stages the record and head with coins,
forest, Orchard state, undo, indexes and tips. Its final readiness check also
refuses a changed head. Existing selected-lock/no-reentrant-canonical-writer
requirements still apply. Low-level staging and unindexed helper calls do not
append: they are not service delivery entry points.

## Bounded replay

`ReadRuntimeOutboxUnderLock` requires the selected writer lock, expected profile
and a consumer cursor containing the last applied sequence and digest. It
checks framing, bounds, domain, cursor and sequence links, exact block identity,
transaction/witness commitments and the head. It does not repeat consensus
proof/script verification. A page holds at most 128 records and 16 MiB of
charged record bytes; a budget unable to hold even the next record returns an
error, rather than an empty page that could be mistaken for completion.

Consumers must commit their effects and cursor atomically in their own store,
or implement an equivalent durable idempotent protocol. Reading a page never
acknowledges it. No deletion or acknowledgement API is exposed yet. A consumer
must not advance from the current head without applying the preceding events.
The first record identifies the start of coverage, not delivery before it.

## Remaining production obligations

The log starts with indexed Orchard transitions. Once an origin exists, the
stateful historical ConnectTip/DisconnectTip paths retain subsequent historical
transitions in their canonical batches, including rollback below activation.
See orchard-historical-delivery-2026-09-26.md. Whole-reorg intent still records
preparation and must be reconciled against committed events and canonical state. Incomplete progress
counts remain lower bounds, including in-process callback failures. No production
`RuntimeBlockNotifications` implementation is installed by this change.

Wallet, mempool/readmission, proof caches, relay, oracle and longpoll consumers,
per-consumer durable cursors, coordinated startup, reindex policy and backlog
readiness remain required. Queue retention is currently unlimited; disk/load
qualification and any safe reclamation policy remain open. Do not enable
activation based on this log or treat it as a running-node lifecycle result.
