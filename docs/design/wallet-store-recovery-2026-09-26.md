# Resuming transparent wallet stores from canonical delivery

`RuntimeWalletRecovery::ResumeTransparentStores` connects the actual
ChainstateService checked source API to the existing index and ordinary wallet
consumers. It resumes already enrolled stores under the intended live wallet
session and persistent database binding. It creates no new receipt or journal.
It does not enroll a missing store or certify its pre-origin baseline.

## Ordered recovery

The owner snapshots both receipts under one wallet lease, releases ownership,
and checks the source origin and both applied cursors. This includes the store
that is ahead after a partial commit and cursors already at EOF. The source page
now exposes the checked post-transition tip of the requested nonzero cursor;
receipt position must agree with that tip. This field is derived from the
retained record without changing the on-disk format.

Recovery captures a source head and reads bounded pages from the lagging cursor.
Each event reacquires wallet ownership and checks that both receipts still match
the snapshot before applying effects. The index commits first; the ordinary
wallet commits second. A failure leaves their real committed prefixes for the
next attempt. Ahead stores are skipped until the lagging store catches up;
old events are not reapplied over newer effects. Reopen uses a fresh process
session and the same persistent database identity.

Every retained transition through the captured head is processed, including
historical transitions below activation and repeated transitions returning to
the same final tip. A page limit is not a backlog limit. The owner does not hold
a wallet lease while requesting selected-chain data and refuses entry under an
existing caller lease. Callers must also respect the established chain, wallet,
then index lock order and keep all three objects alive for the synchronous call.

After the stores agree at the captured head, recovery checks the source again
and rereads both store receipts. The result contains the applied prefix and the
most recently observed source head. The source may already have advanced, or may
advance immediately after that observation. The result is not a lasting wallet
readiness certificate and does not publish the process-wide wallet height.
Concurrent store changes or a wallet replacement cause refusal and ordered retry.

## Scope and remaining integration

Missing or invalidated receipts require baseline reconciliation. Existing
receipts do not prove historical baseline completeness, key ownership or
independently validated chain history. Historical confidential-output events
still refuse in the underlying consumers; this does not authorize losing CT
funds. Account/note, vault, mempool, proof-cache, relay and oracle recovery remains
separate. This entry point is linked into the daemon dependency graph, but no
production notification provider or startup caller is installed by this change.
It cannot be used as all-consumer readiness. Account recovery and baseline
validation must join the owner before production notification installation.

The integration regression uses actual checked generated source and real wallet
and index SQLite stores. It exercises committed index prefixes with ordinary
failures and reopen/retry, ahead-cursor validation, EOF position mismatch,
concurrent store changes, caller lease refusal, source reads without wallet
ownership, stale session rejection, and more than 128 retained historical
transitions. It also advances the source after the captured target and requires
another pass for those additional events. Fixtures and seeded baselines are not
independently validated historical consensus or a running daemon.

## Qualification

Fresh declared CMake builds of the integration executable and full daemon pass.
The reader-disabled service translation unit also compiles. Three actual CTests
pass: OrchardIndexDelivery, OrchardRuntimeOutbox and OrchardServiceDeliverySource.
The existing mandatory integration lane contains the recovery regressions;
45 enabled root registrations remain present in both workflow selectors, and
not all 45 were executed locally. The public service adapter is compiled in the
daemon dependency build; the coordinator regression invokes its private test
source seam over the actual checked reader, not a running service or daemon.

All 114 linked project C++ translation units of the integration executable were
freshly instrumented with ASan/UBSan. The test unit was rebuilt after refining
the interleaving regression. Four copied-source controls omit position checking,
receipt rechecking, multi-page replay, or caller-lease refusal; each fails an
intended regression without sanitizer diagnostics, and restored source passes. The final maps
exclude project C++ archive members. Rust and external libraries remain
uninstrumented; macOS leak detection is disabled. Full ARM RocksDB sanitizer
qualification remains a separate open gate. No original-source test-first,
fresh-process crash, physical power-loss, full-node or release-binary provenance
claim is made. Local build labels and prebuilt OpenSSL dependencies are inherited.
