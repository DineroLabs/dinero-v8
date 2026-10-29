# Known-script origin adoption

The service can now capture both the ordinary wallet domain and the UTXOIndex
script/path domain using `getRuntimeWalletOrigin(wallet, session, &index)`.
Capture and final recheck hold the real wallet lease before index ownership;
all selected-history acquisition and consensus replay occurs outside that lease.
The projection includes the union of known scripts and binds the index database
path. Each store receives only its own scripts.

`adoptRuntimeWalletOrigin` rechecks actual checked event 1 before wallet ownership.
It preflights ordinary rows/history, pins the wallet session and persistent identity,
and checks the captured index domain. The index commits its selected-origin rows
and actual first-event effects with the existing DNUI01 receipt. While still holding
index ownership, the ordinary wallet commits its origin rows/history and that same
actual event with DNOW01. The transactions remain separate: failure of the ordinary
store preserves the index prefix for ordered retry after reopen. A retry also
checks the expected origin-plus-first-event coin inventory; a first-event receipt
alone is not evidence that older coins were reconstructed.

Compatible existing index rows retain Utreexo/CT auxiliary metadata. Matching
ordinary rows retain display metadata; selected-chain spender identities/heights
are restored from source facts. Existing history categories, labels, amounts and
timestamps are preserved, while selected confirmation heights are reconciled.
Missing incoming or coinbase history uses known source amounts and the existing
receive/generate categories. Missing originated-send/self-spend history refuses;
no fee, send category or local intention is inferred from debit/credit totals.
Unmatched confirmed history, unclaimed coins, conflicting rows, unexplained spent
flags on chain-unspent coins, invalidated receipts and already-advanced prefixes
refuse. No cursor reset, new receipt format or independent journal was added.
Live lockunspent and abandonment sets are untouched. They remain in-memory sets.

This is an explicit service operation over already retained event 1. It is not
installed in startup, notifications, RPC, selection, or a recovery provider.
Subsequent events still require the existing recovery coordinator. Source growth
after the check can require further replay; successful adoption is not lasting
readiness. Complete key/account discovery, invalidated-store/orphan reconciliation,
missing originated history, pending/mempool/account reservation coordination,
CT epoch/value compatibility, long-history/load bounds and the first activation
bootstrap remain open. Existing source limits and ordinary CT refusal remain.
Mainnet activation remains unset and steps 1–4 remain incomplete.

## Qualification status

Fresh declared service/index/replay targets and the full daemon build passed.
The actual service translation unit also compiled with the runtime reader macro
removed. Three final CTests passed: AssumeUtxoReplay (4.08s),
OrchardServiceDeliverySource (7.59s), and OrchardIndexDelivery (78.63s).
Both unchanged root workflow selectors still enumerate 46 enabled registrations;
only the three stated suites ran locally. The existing root execution lane now
requires the new actual adoption marker in its retained detailed log.

The actual service fixture covers distinct ordinary/index domains, an index path
change, receipt-without-baseline refusal, existing Utreexo-position preservation,
missing index/ordinary creation, ordinary history failure after index commit,
ordinary rollback, stale session refusal, wallet reopen/retry and idempotence.
All 203 linked project C++ translation units were freshly ASan/UBSan instrumented;
the actual service operation executed in scope and link maps contain no project
C++ archive members. Three copied-source controls omit the existing-index baseline
check, ordinary baseline write or missing index creation; each fails its intended
assertion without sanitizer diagnostics, and the restored executable passes.
External libraries and Rust remain uninstrumented; macOS leak detection is off.
The full RocksDB ARM qualification gate remains open.

The service fixture uses short selected regtest coinbase history and an actual
canonical empty Orchard boundary. It does not qualify signed historical spends,
CT, originated-history preservation over real signed sends, first-event owned
spends, full startup, new fresh-process/power-loss durability or release provenance.
No original-source test-first claim is made. An initial fixture compile had an
ambiguous UTXOIndex type; it was namespace-qualified. A later position assertion
used GetUTXO, which does not populate that field; the final test reads the actual
stored column. Neither production checks nor test deadlines were relaxed.
