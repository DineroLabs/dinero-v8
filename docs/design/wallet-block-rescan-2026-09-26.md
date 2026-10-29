# Checked wallet block-rescan transactions

The existing `WalletManager::rescanBlockchain` entry point now refuses a borrowed
SQLite transaction before discovery or effects. Its existing database lease pins
the selected wallet. Required watch-script reads, statement preparation, bindings
and execution are checked. Missing ownership data refuses completion instead of
being treated as an empty wallet.

Owned UTXO cleanup, restored spends, replayed outputs/spends, scan progress and the
persisted tip share one checked `synchronous=FULL` transaction. The in-memory
height publishes after COMMIT, with its mutex reserved before the commit. A
failure rolls back this function's transaction; an active transaction that cannot
be rolled back terminates. Caller-owned transactions are left unchanged. Cleanup
is scoped to the selected wallet ID, and the inclusive height loop cannot wrap.
Existing output insertion and display-address behavior are retained.

Existing address discovery runs before the effects transaction. Newly issued
addresses remain issued if subsequent replay fails. Discovery failure now refuses
the scan; this fixed initial gap is not complete key/account discovery. Existing
receipt-invalidation guards apply, and no delivery receipt or index acknowledgment
is created.

## Scope

This entry point still reads mutable archival ChainDB data while holding wallet
ownership. It is not the independently validated, immutable selected-origin source
required by coordinated recovery. Do not add chain locks beneath the wallet lease
or use this scan's successful result as a baseline certificate. It does not
reconcile the index, ordinary transaction history, confidential output values,
all derived key ownership or account inventory. The separate legacy shielded
consumer remains unchanged; this work does not qualify its in-memory rollback.
Production Orchard provider installation remains gated.

## Qualification

The existing declared `WalletRescanUtxoSet` test now also drives a real WalletManager
and archival ChainDB through block rescan. Its generated transaction bodies are
wallet-effect fixtures, not independently consensus-validated history. It injects
cleanup, restored-spend, output, spend, progress, tip and deferred COMMIT failures,
checks unchanged durable effects/progress and published height, refuses borrowed
transactions and unavailable watch reads, then verifies successful real input
spend recording, actual wallet reopen and idempotent retry. Existing snapshot and
legacy-schema checks remain. The required Orchard wallet-import lane checks an
additional execution marker for block effects/progress/tip.

The fresh declared test target and full daemon build passed. One actual CTest,
`WalletRescanUtxoSet`, passed (including its existing snapshot/legacy cases and
new block cases). Both unchanged Orchard root selectors still enumerate 46 enabled
registrations; those 46 were not all executed locally. The service translation
unit also compiled with the runtime reader disabled.

All 67 linked project C++ translation units were freshly instrumented with
ASan/UBSan; the final link map excludes project C++ archive members. Three copied
source controls (original rescan implementation, omitted progress check and
omitted COMMIT check) failed the intended assertions without sanitizer diagnostics;
the restored suite passed. External libraries and Rust were uninstrumented and
macOS leak detection was off. The full RocksDB/ARM qualification gate remains open.
No original-source test-first, new fresh-process, physical power-loss, running
Orchard provider, whole-node lifecycle or release-binary provenance is implied.
