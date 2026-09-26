# Wallet UTXO rollback failure handling

Date: 2026-09-26

The existing `UTXOIndex::RevertBlock` called SQLite BEGIN, DELETE, UPDATE and
COMMIT without consistently checking their return values. A failed statement
could leave only one half of the rollback applied while the function returned
success. A failed nested BEGIN could also allow it to commit its caller's work.
This routine is called by the real WalletWorker disconnect and reorg paths.

The replacement holds the existing database mutex, acquires its own transaction
with checked BEGIN IMMEDIATE, checks preparation/binding/execution of both
statements, and checks COMMIT. Statements finalize through RAII. Failure rolls
back only the transaction it acquired; if SQLite cannot abort a still-active
transaction, execution terminates instead of permitting further writes against
partial state. A failed BEGIN does not touch a caller's existing transaction.
No fallible logging follows commit inside this routine. Successful replay remains
idempotent. No row schema, amount, key, network or activation rule changes.

`WalletRevertAtomic` links the real index, SQLite opener, logger and uint256 code;
it does not use the older mock UTXO correctness model. Temporary-database triggers
force failure in DELETE, in UPDATE after DELETE succeeds, and at COMMIT using a
deferred foreign-key constraint. Each case must throw at the intended stage and
retain both original rows. A nested transaction preserves its pending metadata
until the caller rolls it back. Normal rollback, repeat rollback and database
reopen verify the final rows. The fixture exhausts the index's reused SELECT
before external trigger changes, avoiding a stale WAL read snapshot masking the
intended injected failure.

The original production function fails the regression; the fixed function passes
both the independent CMake/CTest target and ASan/UBSan instrumentation of all five
linked project C++ translation units. SQLite remains uninstrumented and macOS
leak detection is disabled. The Orchard workflow explicitly builds, inventories
and executes this lane and retains its log. Default test builds register it
unconditionally. These are component checks, not whole-daemon or release-binary
qualification.

This does not make WalletWorker's in-memory queue durable, reconcile its separate
WalletManager database, implement Orchard delivery ownership, or prove power-loss
durability. In particular, the existing UTXO database opener's WAL synchronous
policy is unchanged. A recovery owner still needs durable source records,
per-consumer checkpoints, correct SQLite durability settings and readiness after
all effects have completed. No production database or installed binary changed.

## Creation replay after a spend

The same real index is populated through `AddUTXO` by WalletWorker connect and
other wallet import/scanning callers. Its upsert formerly assigned the incoming
spend height even when it was absent. Replaying creation of an output could thus
clear a spend already recorded by a later block. It could also replace an
existing spend height with stale supplied metadata, confusing subsequent undo.

Creation now preserves an existing spend height. A new row, or an existing row
without a spend, may still accept supplied spend metadata. Explicit block
rollback remains responsible for restoring spent outputs to unspent; creation
replay alone cannot authorize that transition. Other upsert fields and the
separate `SpendUTXO` operation are unchanged. Recovery must still validate source
order and apply disconnects before connecting replacement history.

The mandatory real-SQLite `WalletRevertAtomic` lane additionally checks unspent
creation replay, spent creation replay, stale supplied spend heights, reopen,
explicit rollback followed by creation, and imports with spend metadata. The
new assertions fail against the prior production source. Fixed normal and
ASan/UBSan executions pass alongside the existing rollback failure cases. All
five linked project C++ files are instrumented; external SQLite is not, and
macOS leak detection is off. This follow-up does not claim a full daemon build,
power-loss qualification, authenticated source receipts or cross-store atomicity.
