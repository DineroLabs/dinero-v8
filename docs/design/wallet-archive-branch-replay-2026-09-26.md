# Archived operation observations on a replacement branch

## Recovery behavior

A previous recovery owner could advance an account's source receipt onto a
replacement branch while leaving an operation archived under its old completion
cause. Restoring reservations alone would miss an intervening confirmation or
conflict. Bound account recovery now finds the actual common ancestor of the
current cursor and archived cause in the existing immutable replay graph.

After `StageReactivate` restores the operation, a private account method visits
actual selected blocks from that ancestor through the account checkpoint. It
checks hash and parent continuity, typed block identity commitments or historical
Merkle identity, and uses the existing operation-observation logic over real
transaction IDs, inputs and nullifiers. The earliest selected confirmation or
conflict is retained. Missing ancestry/body/lookup results refuse rather than
being interpreted as a disconnected cause or an empty block.

Reservations and any new observation share the existing bound owner's FULL
SQLite transaction. Failure after reactivation cannot commit unobserved pending
state. The source cursor, note scan, parent locator, issued addresses, signed
operation bytes and archive remain intact. Reconciliation does not claim another
source event was applied, re-prove a transaction, or authorize broadcast. A
subsequent actual disconnect removes the derived observation through the normal
account undo path.

`ForkHeight`, selected hashes and body lookups use immutable service-owned replay
material captured before wallet ownership; no selected-chain locks are acquired
while holding the wallet. No new journal, identity, schema or delivery receipt is
introduced. Existing single-account and enrolled-account coordinators use this
through their bound recovery consumer. Production notification/startup
installation is still absent.

## Scope

The retained graph must cover both branches and their ancestor. Current replay
capture, account inventory and pending-operation capacity limits still refuse
rather than truncate. This does not certify baseline completeness, pre-origin
state, account discovery, independently valid historical consensus or readiness
for other consumers. Historical CT compatibility remains unresolved. Mainnet
activation remains unset.

## Regression

The integration fixture stores a real completed operation under an authenticated
archive, connects a sibling block through the actual indexed canonical writer,
and advances the account with the low-level owner that does not reconcile
archives. The sibling retains the real transactions, Utreexo proof and state
commitments while changing its header identity. Reconciliation must restore the
operation and detect its genuine transparent-input conflict on that sibling.
These generated blocks and explicit wallet baseline are not independently
validated historical consensus or a running production recovery provider.

An injected observation-write failure follows the reservation write. The test
requires the original revision, source receipt and empty pending state to remain
unchanged, then reopens the wallet and retries. It checks the replacement hash
and conflict outcome, stable note balance, cursor, parent locator, issuance and
archive, repeated recovery without revision churn, and removal of the
observation on the sibling's later disconnect. The new case uses a Reserved
operation and a one-block activated replacement branch; it does not newly qualify
Ready broadcast, a long replacement branch or historical-only branch replay.

## Local qualification

A fresh declared build of the affected executables and full `dinerod` passed.
Three CTests passed: OrchardIndexDelivery (64.22s), OrchardOperationArchive
(8.53s), and OrchardAccountDelivery (17.93s). The actual service translation unit
also compiled with its runtime-reader macro removed. Both unchanged workflow
selectors enumerate 46 enabled root tests; only the three named tests ran
locally for this change.

All 122 project C++ translation units linked into the integration executable
were freshly instrumented with ASan/UBSan. The instrumented regression passed,
and the final link map has no project C++ archive members. Rust and external
libraries are uninstrumented, macOS leak checking is disabled, and the service
adapter is outside this sanitizer scope. The separate ARM RocksDB dependency
gate remains open. Build labels inherit the preceding commit, not release
provenance. Existing archive-suite process-boundary tests were rerun; no new
fresh-process or physical power-loss test is claimed.

Three copied-source sanitizer controls used the preceding bound owner, omitted
the observation write, or returned the wrong fork height. Each failed the
intended regression without sanitizer diagnostics; the restored executable
passed again. These are post-implementation controls, not an original-source
test-first claim.
