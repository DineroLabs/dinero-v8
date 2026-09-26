# Wallet database ownership

Date: 2026-09-26
Status: installed wallet worker ownership; complete runtime recovery remains open.

## Selected wallet and connection

`WalletManager::AcquireDatabaseLease` holds the wallet lifecycle mutex and the
actual serialized SQLite connection mutex. The existing worker takes this lease
around connect, disconnect and reorg jobs, and its synchronous rescan. A job
cannot move between WalletManager databases halfway through processing. The
connect test pause remains before acquisition so it does not hold database locks
while waiting for its external test handshake.

Create, open and close take the same lifecycle mutex. Another thread waits for
release; an attempted same-thread wallet replacement fails before changing the
connection or wallet identity. The lease is noncopyable and must be destroyed on
its acquiring thread. A lease with no selected wallet contains a null connection;
it does not fabricate an initialized wallet. Manager lifetime must exceed lease
lifetime. Existing borrowed raw pointers are not retroactively lifetime-safe.

Holding SQLite's recursive connection mutex for the whole lease also excludes
SQL statements from legacy raw-connection callers on other threads. SQLite's
usual per-statement serialization alone would allow another thread's statement
to join a transaction between BEGIN and COMMIT. Connections without a real
SQLite mutex are refused. Callers must not close or replace the borrowed handle,
retain statements beyond the lease, or wait for work on another thread that
needs this same connection.

## Transaction ownership

Outermost entry requires autocommit, so a pre-existing transaction is refused
without commit or rollback. Nested leases on the same thread share the outer
owner and its transaction. Only the last lease performs abandoned-transaction
cleanup. A transaction begun during the lease remains exclusively owned by that
job. The caller must check COMMIT before publishing in-memory
account state or claiming delivery. An unfinished transaction rolls back when
the last lease exits; inability to roll it back while it remains active terminates
the process. No fallible diagnostics are performed by lease destruction.

The lease does not begin a transaction automatically, change journal/synchronous
settings, suppress statement failures, or make an already committed partial
update whole. Historical jobs still contain independently committed operations
and error logging. Their queue is still transient and not an acknowledgment.

## Remaining recovery integration

The UTXO index is a separate SQLite database with its own mutex. This lease owns
WalletManager's connection only. Account snapshots already carry encrypted
source receipts; the runtime outbox and whole-reorg intent already exist. The
recovery owner still needs idempotent ordinary-wallet and UTXO effects, source
verification, per-store committed progress, account ownership, historical
cross-boundary handling and readiness that waits for every required consumer.
Queued historical jobs are not assigned a durable wallet identity at enqueue.
No runtime notification provider is enabled by this change.

Future recovery must preserve lock order: obtain required selected-chain data
before wallet ownership, and never ask another thread to perform SQL on a leased
connection. WalletManager lifecycle precedes its SQLite connection mutex; worker
UTXO-index calls occur after those locks. Wallet lock/unlock and legacy raw APIs
are not universally serialized C++ state by this connection lease.

## Qualification

The independent WalletDatabaseLease test uses real temporary WalletManager
wallets. It checks the actual SQLite mutex from another thread, blocks a wallet
switch until release, rejects same-thread switches, preserves a borrowed
transaction, rolls back abandoned work, persists committed work over reopen and
prevents a competing SQL statement from entering the leased transaction.
Normal daemon/compatibility builds, sanitizer scope and exact-source Linux
execution are recorded separately; none is final release-binary provenance.

Local qualification rebuilt 63 affected/dependent project translation units and
linked the daemon and selected targets against read-only unaffected libraries.
Eighteen selected CTests passed, followed by the existing covenant wallet
recovery suite. Fresh CMake registers the independent WalletDatabaseLease lane;
the Orchard workflow builds, requires, executes and archives it separately.

All 67 project C++ translation units linked into the ownership test were freshly
instrumented with ASan/UBSan. The first run stopped before main in RocksDB's
histogram initializer because instrumented and uninstrumented vector code mixed.
Compiling that external initializer with the same sanitizers resolved the error;
no sanitizer/container checks were disabled. Other external code remains
uninstrumented and macOS leak detection is off. Three copied-source controls
remove the connection mutex, lifetime lock or abandoned-transaction rollback;
each fails its intended assertion and the restored implementation passes.
Worker/service sources were normally compiled; the 67-TU sanitizer claim covers
the ownership executable, not whole-daemon concurrency or power-loss recovery.

## Runtime entry points and nested rescans

All 20 legacy shielded runtime entry points now take a shared `RuntimeWalletLock`
which acquires the wallet database lease before the process-wide runtime mutex.
Destruction releases the runtime mutex first. A caller waiting for a leased
wallet therefore does not hold the shared runtime lock and block an unrelated
wallet. This pins selected identity for runtime calls, including calls outside
the worker; it does not universally serialize every legacy key-cache mutation.

The original runtime entry points did not acquire the WalletManager SQLite
connection lock: a current reverse-order deadlock was not established. The
confirmed regression is that they could bypass a held wallet lifetime lease.
The fixed order prevents a new inversion as ownership is extended.

The actual `WalletManager::rescanBlockchain` entry now also takes a lease, so
callers outside WalletWorker receive the same ownership. Its already-owned outer
transaction is preserved when it enters runtime scanning. Nested leases cannot
adopt a transaction from an unrelated caller, since the first lease still
requires autocommit. New real-wallet regressions distinguish borrowed versus
nested transactions and test runtime blocking and progress on another wallet;
both fail against the original ownership implementation.

The runtime follow-up rebuilt the daemon and dependent targets after 63 project
translation units and passed 18 selected CTests plus the existing covenant wallet
recovery suite. All six ownership cases pass with 67 project C++ translation
units instrumented: the three changed test/manager/runtime units were rebuilt,
and the unchanged instrumented objects and RocksDB initializer were reused
read-only. Reversing the runtime lock order or allowing an inner lease to roll
back the outer transaction fails the intended regression; the restored source
passes. These are bounded local concurrency tests, not a complete deadlock
proof, running-node qualification or cross-store crash recovery.
