# Recovery of all currently enrolled Orchard accounts

`RuntimeWalletRecovery::ResumeEnrolledWalletStores` obtains the actual service-owned immutable replay view before wallet ownership, then coordinates the existing index, ordinary-wallet and encrypted account receipts. It does not install a production notification or startup caller.

## Discovery and ownership

`OrchardAccountDelivery::ReadEnrolledForReplay` holds the real selected-wallet lease, expected process session, persistent database binding and recovery-seed owner. In one checked FULL SQLite transaction it reads every current snapshot row, validates its wallet identity/account/revision, derives that account's actual keys, authenticates its AEAD envelope and fully restores the account against the immutable branch point selected by its authenticated receipt. No metadata-only account escapes. A foreign identity, malformed row, missing schema, empty inventory, zero delivery receipt or failed restoration refuses recovery. No account is silently enrolled or filtered out. An operational maximum of 1024 accounts refuses excess inventory instead of returning a subset; this is not a consensus limit or a memory/load qualification.

The current SQL row inventory is **not an authenticated account catalog**. This change detects membership changes during a recovery pass, not deletion before discovery or restoration of an older entire wallet database. It does not prove baseline completeness, key discovery beyond existing snapshots, initial enrollment, late-account reconciliation or backup provenance. These remain production-readiness obligations.

## Ordered commits and retry

Before effects, every transparent and account applied cursor is checked against the selected source, including ahead accounts and EOF. Before each event, a new owned read rechecks both transparent receipts and the complete sorted account-number/revision/cursor inventory. Only lagging stores are applied: index, ordinary wallet, then accounts in ascending account-number order. Each account uses the existing bound consumer, encrypted receipt and authenticated retained parent locator. A later failure leaves earlier committed prefixes available for ordered retry after reopen; it does not roll back another database, invent acknowledgments or replay ahead accounts.

Final source and inventory reads check the captured result again. The result includes each account number and committed revision, the captured applied prefix and observed source head. It is not lasting readiness, process height, a baseline certificate or completion of other configured consumers. The existing explicit-one-account API remains scoped to that account and now shares the same implementation.

## Limits and remaining work

The existing 2048-record/64 MiB replay capture ceiling still applies, and unsupported older source frames still refuse. Recovery does not silently drop backlog. Full account restoration on every event is deliberately conservative and remains unqualified for large account counts/history. No new identity, receipt, journal, reset-to-ready API or snapshot format is introduced. Independent activation history, general long-history recovery, baseline reconciliation, remaining consumer obligations and production installation remain open.

## Qualification

A fresh actual declared CMake integration target and full `dinerod` build passed. `OrchardIndexDelivery` passed in 53.25 seconds after the final diagnostic refinement. The default runtime-reader-off service translation unit also compiled. Both unchanged workflow selectors contain 46 enabled root tests; only the affected integration registration ran locally for this change.

The integration exercises actual wallet/index/account stores with nonconsecutive accounts 0, 7 and 19; each account derives its own keys. Tests cover zero-progress baseline refusal, corrupted envelopes, foreign wallet rows, recovery across more than 128 transitions, failure in a later account after both transparent stores and the first account commit, reopen/retry, preserved receiver issuance and membership changes during source acquisition. Account 0's real 5000-unit note remains unowned by account 7 and survives undo/reconnect. The production service adapter is compiled; the coordinator test uses the private core seam over the real checked canonical reader, not an installed daemon caller.

All 121 linked project C++ translation units were freshly instrumented with ASan/UBSan. After a diagnostic-only test refinement, that test unit was rebuilt and the other 120 unchanged current-source objects retained. Rust/external libraries remain uninstrumented, macOS LSan is off, and service adapter instrumentation is outside this scope. Final maps exclude project C++ archive members. Four copied-source omission controls cover account discovery, foreign-row refusal, membership rechecks and later-account effects. Their intended assertion failures and the restored passing run are retained in the private evidence ledger.

Generated bodies and explicitly enrolled baselines are not independently validated historical consensus. This change adds no fresh-process, physical power-loss, running-node or release-binary provenance claim. Local labels are inherited from the preceding source; prebuilt OpenSSL and the open ARM RocksDB dependency gate remain separate qualifications.
