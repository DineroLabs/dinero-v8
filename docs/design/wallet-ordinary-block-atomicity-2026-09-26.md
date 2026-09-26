# Ordinary wallet block writes and retry

Date: 2026-09-26
Status: installed in the existing WalletWorker connect path; coordinated runtime
recovery remains incomplete.

## Behavior

While holding the existing WalletManager database lease, ProcessConnect opens a
checked ordinary-wallet SQLite transaction around transaction confirmations,
UTXO changes and transaction history. Required insert, spend and history failures
now propagate instead of being logged and ignored. confirmTransaction retains its
boolean result for existing callers and offers an optional error output: a missing
history entry is distinct from failed preparation, binding or execution.

The index transaction commits first, followed by checked ordinary-wallet COMMIT.
Vault observations and the worker's height update happen afterwards. A SQL failure
before index commit rolls back both uncommitted stores. If ordinary-wallet COMMIT
fails after the index commit, the ordinary transaction rolls back and height does
not advance. The index is already committed and must be replayed in source order;
this is deliberately not described as an atomic transaction across two databases.
The transaction destructor rolls back owned unfinished work; an active transaction
that cannot be rolled back terminates. Failed BEGIN does not adopt an outer
transaction. Wallet lifetime and connection ownership remain held throughout.

Ordinary-wallet creation replay now updates matching UTXO metadata without
replacing the row or clearing its recorded spend. The upsert supports both the
older wallet-id/outpoint uniqueness and current per-wallet outpoint uniqueness.
A conflict belonging to another wallet is not updated and is reported as failure.
Explicit disconnect remains responsible for restoring unspent state. Transaction
history and UTXO rows in this group are committed together; optional address labels
retain their existing behavior.

## Qualification

The actual WalletDatabaseLease target uses a real WalletManager and UTXOIndex.
The new regression injects errors during ordinary insertion, spending, history
insertion, confirmation and deferred-foreign-key COMMIT. It checks the precise
failure, ordinary rows and history rollback, unchanged height, retry, same-block
spending, creation-only replay and reopening the wallet. The commit-failure case
explicitly observes that the index has committed while the ordinary database has
not. These are wallet-effect fixtures, not consensus-valid blocks. The vault
observer remains the existing test stub.

A fresh declared CMake build compiles the test target and full daemon. The
WalletDatabaseLease, WalletRevertAtomic and CovenantWalletRecovery CTest lanes
pass. The service translation unit also compiles with optional runtime support
off. The sanitizer executable includes all 71 linked project C++ translation
units, including the real worker and its newly reached address encoders. External
RocksDB histogram code retains its existing instrumented object; other external
libraries are uninstrumented and macOS leak detection is off. Initial qualification
caught an upsert constraint mismatch with current wallet schemas and two additional
linked project sources; both were corrected without weakening checks. Copied-source
controls using the original worker, omitting the ordinary transaction, or restoring
the old spend-reset upsert each fail the intended regression without sanitizer
errors; the restored executable passes. No original-source test-first run is claimed.

Local working-source builds use inherited version labels and prebuilt external
libraries. They are not release binary provenance. Exact-source Linux results are
recorded separately.

## Remaining recovery obligations

This does not acknowledge the transient worker queue, bind queued jobs durably to
a wallet, advance a verified source cursor, or install RuntimeBlockNotifications.
The shielded note store runs separately, and its existing error handling is not
changed here. Vault observations, maturity/tip metadata publication, unknown
script handling and other legacy notification paths are not collectively covered
by this ordinary-wallet transaction. A callback can still fail after durable work.

The production owner must verify the existing canonical outbox and reorg intent,
retain per-store progress, reconcile partial commits at restart, handle historical
cross-boundary delivery and finish every consumer before declaring readiness.
Current SQLite durability settings are unchanged; these tests do not simulate
power loss. Activation history, ordinary confidential-transaction compatibility,
whole-node restart/reindex and live Orchard wallet RPCs remain unfinished.
