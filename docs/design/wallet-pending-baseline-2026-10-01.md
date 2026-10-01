# Ordinary pending payment and recovery baseline

Source and fixtures prepared; not compiled or executed.

`DatabaseLease::StagePayment` retains the actual signed body and reservations in
its existing FULL SQLite transaction. The unconfirmed history INSERT continues
to run the existing ordinary invalidation trigger. A private typed transition
may preserve the exact previously present receipt only if all pre-existing main
user-table rows and schema remain unchanged, excluding only the selected metadata
row's pending envelope/receipt fields and the one checked new history row. The
pending envelope must exactly equal the newly authenticated encoding; history
must exactly match the typed intent, fee, time, wallet and unconfirmed state.
The eleven known history columns are required; unknown extensions refuse.

Before and after preservation, complete reads check terminal DONE and compare
SQL-type-tagged SHA256 row hashes including duplicate multiplicity. Columns use
explicit lengths, INTEGER64, REAL bits and NULL tags. Fields are hashed directly
from SQLite-owned memory without another plaintext key/seed buffer. The snapshot
includes present key/account/archive/other consumer tables. Schema and all18
ordinary invalidation triggers must remain byte-identical. Any failed read,
unexpected side effect, preservation write or outer COMMIT refuses the payment.
No callback, chain lookup, trigger disable or public receipt reset API is added.

Absent or invalidated receipts are never created or normalized. The prior opaque
receipt is preserved, not reinterpreted as a new progress/readiness certificate;
its ordinary reader still validates the encoded identity/domain/checksum. A valid
receipt remains an as-of store acknowledgment, not current selected-chain or
whole-wallet readiness. Limits of one million rows/64MiB per capture and4096
tables refuse instead of truncating; this is not load/resident qualification.

Four ordinary wallet fixtures exercise actual signing/pending retention using an
explicit opaque marker solely for preservation mechanics. A separate canonical
fixture adopts the actual checked origin, retains a real signed payment and
checks the original progress through reopen. Existing arbitrary-history writes
still invalidate it. Fresh declared ON/OFF full daemon and component targets passed. Selected normal CTests:83 ON and76 OFF, plus ON OrchardRuntimeReader and two independent600-second account/archive custom runners. All linked project C++ freshly ASan/UBSan in six separately compiled graphs:200 replay,126 wallet,35 state-machine GTests, two complete custom runners and five ordinary DaemonApp modes; maps and stable input hashes verified. Ten new inventory/payment cases plus required metadata/archive markers executed. External/Rust/C/PQClean uninstrumented; macOS LSan off; normal regression/reader/OFF binaries outside these graphs. Exact GNU CI and release remain separate. No original-source RED, unsafe controls, activation or release-readiness claim. The canonical pending-baseline fixture discovers its real funded output through origin adoption; its original progress, retained-body, reopen and invalidation assertions passed after replacing manual pre-enrollment. The additional enabled OrchardWalletStorage runner passed normally with read-only current/retained authentication and write refusals, giving 87 ON and 76 OFF total CTest executions. Its fixture is outside the six instrumented graphs; the separately archived two-translation-unit storage ASan/UBSan result remains a distinct narrow qualification. Existing
assertions and deadlines are preserved; new mandatory CI checks require all five
case markers. Shared ordinary/Orchard reservation ownership, selected-source
reconciliation and actual shield RPC remain the next integration work.
