# Ordinary wallet rollback transaction

Date: 2026-09-26
Status: installed in WalletManager and its existing WalletWorker disconnect path;
production Orchard recovery ownership remains unfinished.

## Change

WalletManager::onBlockDisconnected acquires the wallet database lease and an owned
BEGIN IMMEDIATE before deleting outputs created at the disconnected height,
removing that height's ordinary transaction history and restoring spent inputs.
Preparation, every binding, statement execution and COMMIT are checked. Any
failure rolls back the owned ordinary-wallet transaction, including a deferred
constraint error at COMMIT. If SQLite already rolled it back, no second rollback
is attempted. An active transaction that cannot be rolled back terminates. Failed
BEGIN leaves an existing transaction untouched. RAII statement owners finalize
statements during both success and unwinding.

The existing historical history-removal semantics and per-wallet schema fallback
are retained. This does not change Orchard pending operations or their encrypted
archive. Input output-indices are bound without signed 32-bit narrowing. Height
zero and heights outside the existing index's signed range refuse before writes.
The worker performs the height check before touching the index, and it refuses a
nested active WalletManager transaction before rolling back the separate index.

The index still commits its rollback first. An ordinary-wallet failure may leave
that index already rolled back, so source-ordered retry remains required. The
visible wallet height changes only after the ordinary SQL group commits. Later
maturity/tip metadata publication and the separate legacy note-store callback are
not part of this transaction or a durable delivery acknowledgment.

## Qualification

The independent WalletBlockDisconnect CTest target uses the real WalletManager,
real separate UTXOIndex and actual WalletWorker. It injects deletion, history,
input-restoration, SQLite automatic rollback and deferred COMMIT failures. Each
case requires the precise error and unchanged ordinary UTXO/history/height state,
while explicitly observing the already-rolled-back index. Removing the failure
allows retry, replay and reopen. Borrowed and nested transaction cases preserve
the caller's writes and the index; invalid-height cases refuse before either
store changes. The fixture tests wallet effects rather than consensus validity;
the vault observer remains the existing test stub.

The new lane is explicitly built, inventoried, executed and retained by the
Orchard workflow. Its registration is independent of WalletDatabaseLease, and
existing test timeouts remain unchanged. Both existing root Orchard selectors
still require 43 registrations.

Local qualification builds the declared target and full daemon from a fresh CMake
configuration. The sanitizer executable has 71 instrumented project C++ files:
three changed files are rebuilt and 68 unchanged objects are reused read-only.
The existing external RocksDB histogram object remains instrumented; other
external libraries are not instrumented and macOS leak detection is off. Local
inherited version labels and prebuilt dependencies are not release provenance.
Exact-source Linux results are recorded separately.

WalletBlockDisconnect, WalletDatabaseLease, WalletRevertAtomic and
CovenantWalletRecovery pass as actual local CTests. The default service translation
unit compiles with optional runtime support off. Copied-source controls restoring
the original manager, removing its transaction, or omitting the worker's nested
transaction preflight fail their intended assertions without sanitizer errors;
the restored executable passes. No original-source test-first run is claimed.

## Remaining recovery

This is one ordinary-store rollback transaction. It does not authenticate a source
cursor, acknowledge the in-memory worker queue, install RuntimeBlockNotifications
or complete cross-store restart recovery. Historical reorg callbacks, legacy
shielded error handling and post-commit observers still require coordinated
reconciliation. The existing canonical outbox, whole-reorg intent and Orchard
account receipts must drive that owner. No production datadir, installed binary,
activation setting or release status changes with this work.
