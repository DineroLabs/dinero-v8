# Bound Orchard account recovery and retained snapshots

The account consumer now derives Orchard keys and the snapshot AEAD key from
the actual selected WalletManager seed under its database lease. It checks the
expected process session, binds the existing persistent database identity and
requires an existing encrypted account. It does not enroll a baseline or expose
a receipt-only setter.

## Key ownership

Seed, encryption-state and key-cache accessors/writers in WalletManager now take
the existing recursive lifecycle mutex. Create/open/close already held it.
`DatabaseLease::CopyRecoverySeed` checks the live session and unlock timeout,
refuses unavailable/locked keys and produces a noncopyable 64-byte owner. The
copy is cleansed at destruction. Another thread's lock/unlock/replacement waits
for the database lease. Same-thread explicit key changes refuse while a recovery
seed is pinned. Timeout is checked before acquisition; a recovery operation may
hold the lease past the timeout until it finishes. Last-lease release with a live
recovery seed or wrong-thread seed destruction fail-stops.

The seed must be destroyed before its lease, and WalletManager must outlive both.
This is not a universal repair of raw database pointer or unrelated C++ state
races. No claim is made that caller copies, swap or compiler temporaries are
scrubbed. The historical suspected lock inversion was not established; this
change addresses actual missing key ownership.

## Authenticated parent retention

`WalletSnapshotStore::StageReplaceRetaining` retains the previous authenticated
envelope and replaces the current account inside an internal savepoint and the
caller's outer FULL SQLite transaction. The existing AEAD binds wallet ID,
network, genesis, account and revision. Retained revisions are immutable through
this API. A conflicting retained envelope must decrypt to the exact prior state.
A failed replacement cannot leave retained writes even if the caller catches the
exception and commits its outer transaction.

`ReadRetained` authenticates both current and requested earlier state. Missing,
wrong-key, corrupt or remapped history refuses recovery. Existing non-retaining
writers remain readable and may leave history gaps; there is no implicit repair
or completeness claim. Retention has no pruning or total-storage/load
qualification yet. This extends the existing encrypted snapshot store, without
adding a source journal or new delivery receipt.

## Bound effects

`OrchardAccountDelivery` reads, connects, disconnects and applies historical
events under selected wallet/key ownership and a checked FULL transaction.
Actual account scanner/observation effects, existing DNORAC05 source receipt,
and prior-revision retention commit together. Disconnect treats a supplied parent
revision as a locator, authenticates it and requires the immediate-parent scan
identity. Current issued addresses, exact Ready bytes and operation archive
survive rollback. Identity initialization remains a separate earlier commit.
No process-wide wallet height, notifications or readiness are published.

Source events, verified authorizations, prepared transitions and immutable
restore lookups must be acquired from the same validated chain view **before**
wallet ownership. Lookups must not acquire chain locks or wait on a thread that
needs the wallet. The new consumer does not itself create those source views,
certify a pre-origin baseline or locate retained parent revisions automatically.
The integration fixture explicitly enrolls generated state; it is not production
baseline certification.

## Remaining release work

The account consumer is linked into the daemon dependency graph, but is not yet
called by the all-store recovery coordinator, notification provider or startup.
Service-owned intermediate replay views, durable parent locators, baseline and
late-account reconciliation, all-consumer readiness and installation remain.
Activation history and full startup/replay/reindex qualification follow those
obligations. Activation is unset. Ordinary confidential transaction compatibility
and the previously recorded ARM dependency sanitizer issue remain open.

## Qualification scope

Fresh declared CMake targets and the full daemon build passed. Fourteen distinct
local CTest registrations cover the existing ownership/queue/ordinary commit
lanes, new recovery-key lane, storage/account/archive and bound account consumer.
The storage test uses fresh processes exiting before/after COMMIT; the bound
consumer test uses reopen in-process and explicit fixture enrollment. Generated
proofs, bodies and synthetic source digests are not whole-node consensus or
validated-source qualification.

ASan/UBSan passed for all 86 linked project C++ translation units of the bound
consumer, 71 of the key-ownership executable, and two of the storage executable.
These counts overlap. One external RocksDB histogram initializer is additionally
instrumented; other dependencies/Rust are uninstrumented and macOS LSan is off.
Initial mixed instrumentation failed in that initializer before the tests; the
additional instrumentation resolves this harness issue without suppressions.
It does not fix or waive the separate ARM checksum qualification gate.

Six copied-source omission controls cover key lock/session, retention/savepoint,
account commit and account parent retention. All fail intended regressions and
the restored lanes pass. There is no original-source test-first, physical
power-loss, whole-node concurrency or release-binary provenance claim. Default
runtime-reader-off service compilation also passes. CI requires 46 enabled root
Orchard registrations and the separate two-case WalletRecoveryKeys lane; all46
have not been run locally.
