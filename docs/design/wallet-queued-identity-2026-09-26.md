# Wallet identity across queued delivery

The existing worker pins the selected database while processing a job. That
alone does not bind work waiting in the queue: another wallet can be opened
between enqueue and processing, including reopening the same named wallet.

## Implemented ownership

All three existing queue entry points capture the WalletManager session under
its lifecycle/SQLite lease. The transient job owns the numeric token, not a
borrowed connection or a cross-thread lease. Dispatch requires a binding when
there is a manager. Each connect, disconnect and reorg path compares it under
its processing lease before shielded callbacks, index mutations, ordinary SQL,
height changes or observers. The lease then prevents replacement during effects.

Create/open/close selection changes advance a process-local non-repeating session; active
rename and the internal selection setter also invalidate it under the lifecycle
lock. Reopening the same name cannot reuse old jobs. Failed attempts after replacement begins
conservatively invalidate queued work; lookup/validation refusals before selection
changes preserve it. Temporary create-time selection and restoration have distinct
sessions. Reentrant replacement refused by an
existing lease leaves its session unchanged. Counter exhaustion terminates
rather than recycling an identity. Empty selection is a real session; selecting
a wallet later cannot adopt its old work, and dispatch without a selected database
refuses before effects. Index-only workers retain their
existing no-manager behavior.

## Recovery boundary

This is not a durable wallet identifier, source cursor or completion receipt.
Tokens are scoped to one manager instance, which must outlive its worker.
The worker still logs failed transient jobs; its queue is neither retained nor
replayed after restart. Rejected work requires canonical-source recovery, and
this change does not yet implement that recovery or an all-consumer readiness
gate. The shared index and script registration also need recovery coordination
when changing wallets. Synchronous rescan still selects/pins its wallet at call
time. General C++ key/cache races outside the lease remain out of scope.

Next work must use the existing canonical outbox and whole-reorg intent, verify
source cursors before wallet ownership, and commit each store's receipt with its
actual effects. Session tokens must not become a second journal or a substitute
for the encrypted account's existing receipt. Production Orchard notifications
remain disabled until real consumer readiness and activation history exist.

## Qualification scope

The independent WalletQueuedIdentity CTest uses real WalletManager databases,
the real worker queue entry points and dispatch, and a separate real UTXOIndex.
A test access shim deliberately separates enqueue and dispatch to control wallet
replacement without scheduler timing. It is not a running worker-thread test.
Connect/disconnect/reorg refusal checks precede index effects, ordinary-wallet
probe writes and height changes. Same-name reopen, empty selection, absent job
binding and successful newly bound delivery are exercised. Existing vault test
stubs remain; fixtures establish wallet effects, not consensus-valid blocks.

The new registration has its own unchanged 30-second timeout. CI checks the
enabled registration and both actual internal case names, preserving the prior
43 root Orchard lanes and separate ordinary-wallet failure-stage lanes.

Local qualification used a fresh declared CMake build of the actual ownership
and disconnect targets and full daemon. Ten selected CTests passed; the optional
runtime-reader-off service translation unit also compiled. All 71 project C++
translation units linked into the ownership executable were freshly instrumented
with ASan/UBSan, with the final worker/test refinement recompiled before final
execution. The existing instrumented external RocksDB histogram object was
reused; other external libraries remain uninstrumented and macOS LSan was off.
Four copied-source controls remove session comparison, empty-selection refusal,
session advancement or enqueue binding. Each fails the intended regression
without sanitizer errors; the restored source passes. This is not an
original-source test-first result, running-thread/whole-node qualification or
power-loss evidence. Local build labels identify the parent configuration and
are not release-binary provenance.
