# Snapshot wallet import ownership and atomic progress

`WalletManager::rescanUtxoSet` now owns the selected database through the existing
`DatabaseLease`. The producer runs while that lease is held; it must use prepared
source data and must not acquire chain locks or wait for another thread needing
the wallet. A borrowed transaction is refused without adopting or rolling it
back. An unavailable wallet returns failure.

Watch-script reads, any required `snapshot_anchored` schema addition, owned output
upserts, scan progress and an advancing tip's maturity/confirmation updates share
one checked SQLite transaction with verified `synchronous=FULL`. Required prepare,
bind, step, progress-row and COMMIT failures refuse completion and roll back the
owned transaction. A rollback that leaves the owned transaction active terminates.
The memory height is published only after COMMIT, with publication ownership
reserved beforehand. This retains the existing ordinary display maturity rules.

Owned output fields must fit storage and the supplied snapshot height. Existing
legacy composite uniqueness remains supported. Existing delivery invalidation
triggers still apply: this import does not create a delivery receipt, acknowledge
another store, or restore a previously invalidated receipt.

The actual `ChainstateService::RescanWalletFromSnapshotUTXOs` configured-file path
requires the requested base height and a recognized container version, reports
incomplete entry reads as producer failure, and refuses a configured source
failure instead of substituting the in-memory set. These are entry-delivery
checks. They do not validate the complete snapshot container, checksum, binding
proof, selected ancestry, ordinary historical scripts or unspentness. The
in-memory fallback when no file is configured remains a separate legacy path.

## Qualification

The declared `WalletRescanUtxoSet` executable exercises actual wallet effects,
producer failure, schema rollback, progress/tip/maturity write failures, deferred
COMMIT failure, borrowed ownership, watch-read failure, competing lease
acquisition, reopen, retry and missing progress. Existing visibility, legacy
schema and block-rescan cases remain in that executable.

`SnapshotWalletImport` invokes the actual service method and WalletManager with
an isolated entry-stream fixture: unavailable source, incomplete delivery,
base mismatch, completion, reopen and retry. Its fixture is explicitly not a
consensus-qualified snapshot. Both registrations are required in the Orchard
workflow and their inventories and execution logs are retained.

Fresh declared targets and the full daemon build passed. Three CTests passed:
`WalletRescanUtxoSet`, `SnapshotWalletImport` and `ShieldedStateStartup`. The
service translation unit also compiled with the runtime reader disabled.
Scoped ASan/UBSan builds freshly instrumented all 67 linked project C++ units of
the wallet executable and all 200 of the service executable (overlapping scopes).
The final wallet test unit was rebuilt after improving the competing-lease test;
its other 66 same-turn units were unchanged. Link maps exclude project C++ archive
members. Four omission controls covering ownership, transaction, progress and
source completion failed their intended assertions; restored binaries passed.
External libraries and Rust were uninstrumented, with macOS leak detection off.
The full RocksDB/ARM dependency gate remains separate and open.

These checks do not provide physical power-loss, full-node Orchard lifecycle,
release binary provenance, or independent snapshot/history qualification.

## Remaining baseline requirements

This is the existing snapshot import's transaction boundary, not the missing
transparent recovery baseline owner. Reconciliation still needs a selected,
independently validated source covering pre-origin wallet state, complete script
and account discovery, exact spent/history reconciliation, and coordinated
index/ordinary adoption using the existing receipts. Matching heights, a
successful import, current wallet rows or an empty Orchard pool cannot certify
that baseline. The recovery provider remains uninstalled.
