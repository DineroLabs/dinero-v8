# Wallet block index transactions

Date: 2026-09-26
Status: installed in the existing WalletWorker connect path; coordinated wallet
recovery and Orchard notification ownership remain incomplete.

## Installed behavior

WalletWorker now applies one connected block's UTXO-index operations through
`UTXOIndex::ApplyAtomically`. The index holds its recursive database mutex across
checked BEGIN IMMEDIATE, the synchronous callback and checked COMMIT. Existing
query and mutation methods can run within that callback; other threads cannot
join the transaction between those calls. Callers must check mutation results
and propagate failures. This is an index transaction, not a generic transaction
across everything invoked by the callback.

The worker checks AddUTXO and SpendUTXO results and throws on failure. An insert
failure after an earlier spend and insert aborts all index changes for that block.
Same-block outputs remain visible to later transactions inside the callback.
Creation replay preserves recorded spends as specified by the existing index
replay rule. The worker stages owned output observations before commit and invokes
the vault observer afterwards. Its early height update is removed: index refusal
or failure cannot publish the new height. A worker without an index also leaves
height unchanged rather than claiming a completed scan.

The callback cannot adopt a transaction started elsewhere or nest another owned
transaction. Legacy BeginTransaction/CommitTransaction/RollbackTransaction,
reinitialization and whole-store reset refuse during ownership. Older bulk
processing methods that have their own transaction control refuse too. Index
lifetime must exceed the synchronous operation; callers must not wait for work
on another thread that needs the same index. Existing worker lock order remains
WalletManager lease before index ownership. No callback or transaction owner
escapes to another thread.

On callback or commit failure, the owned transaction rolls back if still active.
If rollback fails and SQLite still reports an active transaction, the process
terminates. Failed acquisition never commits or aborts someone else's transaction.
The successful path performs no fallible diagnostics after its internal commit.

## Qualification

The mandatory WalletDatabaseLease executable now includes an actual WalletWorker
connect regression with a real temporary index, registered ownership script and
two transactions, including a same-block spend. An insertion trigger fails the
second transaction. The test requires the exact worker write error and unchanged
index rows, then removes the trigger and checks successful retry and replay.
The original worker fails these assertions. The fixture selects regtest explicitly;
its transactions test wallet effects, not consensus validity. WalletManager is
not attached to that worker instance, and the existing test-only vault observer
stub avoids claiming qualification of those separate consumers.

The mandatory WalletRevertAtomic executable also checks callback abort, borrowed
transaction refusal, nested ownership and legacy-control refusal, deferred
foreign-key failure at COMMIT, and exclusion of a concurrent metadata writer.
It retains the earlier rollback and creation-replay regressions. All five linked
project C++ files are instrumented with ASan/UBSan; SQLite is external and macOS
leak detection is disabled. Removing rollback or callback-wide locking fails the
corresponding assertion. The worker itself is normally compiled and tested, not
included in that five-file sanitizer claim.

Local daemon and affected executables were rebuilt after 72 project translation
units; unchanged libraries were used read-only. Default service compilation with
optional Orchard runtime support off is retained. Fresh CMake registration checks
require the ownership and rollback lanes, which the Orchard workflow explicitly
runs. Exact-source Linux results are tracked separately. Inherited local binary
version labels are not release provenance.

## Remaining ownership

WalletManager SQL, the legacy shielded note store and vault observations remain
separate from this index transaction. Existing per-write WalletManager error
logging is not a durable acknowledgment, and a later observer failure can still
occur after index commit. The in-memory worker queue is not a recovery log.
Authenticated outbox replay, per-store progress, coordinated restart, historical
cross-boundary recovery and all-consumer readiness still need the production
owner. The index opener's WAL synchronous policy is unchanged; this is SQL
failure atomicity, not power-loss qualification. No Orchard provider, activation,
production database or installed binary is changed by this work.
