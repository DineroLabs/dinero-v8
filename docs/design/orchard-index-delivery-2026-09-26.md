# Ordered UTXO-index delivery

## Store-owned effects and progress

`RuntimeIndexDelivery` consumes a checked `RuntimeOutboxEvent` and applies real
transparent index effects together with a progress record in the existing
`UTXOIndex` SQLite database. Historical transactions and Orchard envelopes use
their actual transaction IDs, inputs and transparent outputs. No historical
transaction or block shells are synthesized. Inputs are spent and owned outputs
are inserted in transaction order, including same-block spends. Disconnect
removes outputs created at that height and restores inputs spent there.
Historical confidential outputs currently refuse the complete event before any
write; this does not resolve ordinary CT compatibility or authorize retiring
those funds.

The owner holds the existing index mutex and watched-script mutex, checks
connection ownership, selects and verifies `synchronous=FULL`, and uses the
existing checked `ApplyAtomically` transaction. Required SQL failures and
COMMIT rejection abort both effects and progress. An existing caller transaction
is refused unchanged. The database and registered scripts must outlive the
synchronous operation; callers must not wait for a thread needing these locks.

The reserved `runtime_delivery:` metadata contains one checked `DNUI01` receipt:
wallet identity, a digest of the exact registered scripts and derivation paths,
selected domain, source sequence/digest, coverage origin and applied tip. It is
per-store progress, not another event journal. Existing canonical outbox and
reorg-intent records retain the replay material. No receipt-only setter exists.
The next event must match the predecessor digest, next sequence, profile and
current tip. Exact replay is idempotent. Initial adoption requires source event
one and refuses rows ahead of its pre-transition height. These checks do not
establish the completeness or branch correctness of the pre-origin baseline.

SQL triggers invalidate the receipt in the same transaction as ordinary index
row mutations. Successful source application replaces it only after completing
all effects. Reads verify the installed trigger definitions. Generic metadata
set/delete APIs refuse the reserved namespace. `ClearAll` invalidates even an
empty tracked index and retains the invalidation tombstone; a legacy reset
cannot silently manufacture a fresh coverage origin. Changed script ownership,
missing guards or invalidation require baseline reconciliation. This is local
consistency protection, not authentication against arbitrary database edits.

## Source and ownership obligations

The event remains a POD and must come from the selected service's checked
`getRuntimeDeliveryPage` / outbox reader. This API checks body identity but does
not independently certify source digests, consensus validity or complete prior
history. Obtain source pages before acquiring wallet/index ownership. A future
production owner must bind a stable persistent wallet identity under the wallet
lease, validate the applied cursor against the source, reconcile the pre-origin
baseline and coordinate other stores before exposing readiness. A process-local
wallet session or an arbitrary string is not a persistent identity certificate.

No production notification provider is installed by this change. Ordinary
WalletManager, note and Orchard-account effects still need their own durable
progress and ordered recovery after partial commits. Account receipts, complete
reorg intent and canonical delivery logs already exist and must be reused.
Baseline invalidation has no permissive reset API. Late-account/rescan recovery,
all-consumer readiness, independently validated activation history and full
startup/replay/reindex qualification remain open. Mainnet activation is unset.

## Qualification

The independent mandatory `OrchardIndexDelivery` CTest uses a real SQLite index
and events obtained from the checked source reader over generated indexed
chainstate. It checks effect/receipt rollback on output and receipt errors and
deferred COMMIT rejection, borrowed ownership, exact replay, skipped sequence,
wallet/script changes, missing guards, reopen and ordinary-writer invalidation.
It performs a real indexed boundary rollback/reconnect and consumes historical
disconnect/connect records below activation. Historical fixtures and seeded
index rows establish body/effect identities, not independent historical
consensus or baseline validity. Empty-index reset invalidation is covered too.

A fresh declared CMake target and full daemon build pass. Eleven selected CTest
registrations pass across the new lane and existing wallet transaction,
ownership, queue and covenant recovery lanes. The final historical-boundary
extension was rebuilt and rerun. Both workflow selectors require exactly 45 root
Orchard registrations and explicitly build the new target; not all 45 ran
locally. The default runtime-reader-off service translation unit compiles.

All 72 linked project C++ translation units were freshly instrumented with
ASan/UBSan. Four copied-source controls omit receipt publication, atomic
transaction ownership, ordinary-writer invalidation or source ordering; each
fails its intended regression, and the restored source passes. The final link map contains no project C++ archive members. Rust and
external libraries are uninstrumented and macOS leak detection is off. This
scope does not resolve the separate open ARM dependency sanitizer gate. Local
inherited version labels and prebuilt dependencies are not release-binary
provenance. No fresh-process, physical power-loss, running-node or all-wallet
recovery qualification is claimed. Exact Linux results are recorded by source
commit separately.
