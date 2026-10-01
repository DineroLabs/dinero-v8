# Authenticated Orchard account and archive capture

Shared transparent reservation checks need current account and reached archival ownership without constructing a scanner or linking archive mutations into ordinary wallet signing. `OrchardArchiveReader` owns the existing exact DNORAR01 decoder, record identity binding, seed checks and read operations. The state-changing archive owner derives from this read-only boundary; its byte encoding, savepoint, selected observation and reactivation checks are unchanged.

`CaptureCurrent` requires an existing caller-owned SQLite transaction, authenticates the account envelope, decodes its metadata through `OrchardAccountMetadata`, then follows every authenticated archive predecessor with exact descending sequence, nonzero/unique IDs and terminal zero. The entire value is returned only after completion. Captured records retain exact body, intent/input commitments, observed cause, identity and revision. Capacity above65536 fails before traversal; this is an operational refusal, not memory/load qualification. No seed, account, record, receipt or path is created. No read is a selected-chain or reservation-release certificate.

The real full selected-source account inventory now consumes this capture and still compares every reached identity/revision against every present SQL row, rejecting duplicates and unclaimed rows. Full current account restoration and retained predecessor restoration, catalog authentication and original selected archive reactivation remain in place. This does not yet connect ordinary coin selection to Orchard input reservations.

The existing archive fixture gains read-only encrypted reopen, exact two-record ownership capture, wrong owner/key/domain and denied-read refusal, caller transaction preservation and late missing-predecessor refusal followed by rollback/retry. Old fixture/assertion bodies and600second deadline are unchanged. A required CI execution checks three completion markers. Fresh declared ON/OFF full daemon and component targets passed. Selected normal CTests:83 ON and76 OFF, plus ON OrchardRuntimeReader and two independent600-second account/archive custom runners. All linked project C++ freshly ASan/UBSan in six separately compiled graphs:200 replay,126 wallet,35 state-machine GTests, two complete custom runners and five ordinary DaemonApp modes; maps and stable input hashes verified. Ten new inventory/payment cases plus required metadata/archive markers executed. External/Rust/C/PQClean uninstrumented; macOS LSan off; normal regression/reader/OFF binaries outside these graphs. Exact GNU CI and release remain separate. No original-source RED, unsafe controls, activation or release-readiness claim. The canonical pending-baseline fixture discovers its real funded output through origin adoption; its original progress, retained-body, reopen and invalidation assertions passed after replacing manual pre-enrollment. The additional enabled OrchardWalletStorage runner passed normally with read-only current/retained authentication and write refusals, giving 87 ON and 76 OFF total CTest executions. Its fixture is outside the six instrumented graphs; the separately archived two-translation-unit storage ASan/UBSan result remains a distinct narrow qualification. Whole shared reservation integration, shielding RPC and release qualification remain pending.

Read-only connection support: the snapshot-store constructor accepts an existing
read-only main database while keeping the same FULL-or-stronger synchronous and
supported-journal checks. Schema initialization and both replacement APIs still
require a writable connection and caller transaction. A read-only WAL connection
must explicitly select FULL if its connection default is NORMAL; no persistent
schema or data write is needed to set that connection option. Archive and storage
fixtures require current/retained authentication and rejection of every writer
without ending the caller transaction or changing row counts.

A fresh isolated original-source probe reproduced constructor refusal and the
changed-source probe passed. The actual storage fixture, including the added
read-only checks, passed with both project translation units freshly compiled
under ASan/UBSan; external OpenSSL/SQLite were uninstrumented and macOS LSan was
off. This narrow result does not qualify the complete archive runner, full daemon,
OFF configuration, other linked graphs or release. Fresh broad qualification is
required after this storage change.
