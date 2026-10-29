# Checked ordinary address issuance

The actual `WalletManager::getNewAddress` and `getNewChangeAddress` paths now
commit their address, derivation path and watched script together in a checked
SQLite transaction with synchronous FULL. The existing lifecycle mutex protects
the selected wallet and seed. The transaction starts before the next-index read,
and address derivation remains inside that ownership. A borrowed SQL transaction
is refused before discovery or persistence; it is neither committed nor rolled
back by issuance.

Required schema reads, prepares, bindings, insertions and commit are checked.
Failure rolls back the owned transaction; inability to roll back an active owned
transaction terminates. An existing watched script must match the exact path and
change flag. Its scan metadata is preserved when compatible. The next-index query
requires a complete integer result in the non-hardened derivation range instead
of narrowing an exhausted index or treating missing results as zero.

Live index registration follows commit. A later registration failure leaves the
issued tuple durable for reopening and recovery, even though the caller receives
an empty result. This is not a cross-store acknowledgment. Existing delivery
invalidation guards remain in force; issuance does not reset or adopt receipts.
No new journal, schema, identity or inventory catalog is introduced.

## Limits

This repairs the two existing ordinary HD issuance entry points. It does not
repair already incomplete rows, descriptor/import entry points, complete account
or key discovery, or authenticate a global issuance inventory. Next indices still
derive from present address rows; deletion or backup rollback protection requires
further work. Watch-only/imported/nonzero accounts are not automatically converted
into account-zero HD ownership. No provider or startup/RPC installation, pending
owner, mempool admission or release readiness is implied. Ordinary funds and the
remaining CT compatibility obligations are unchanged.

## Qualification

New actual wallet cases exposed required-write, borrowed-transaction and index
exhaustion failures before production edits. They check receive and change
rollback, deferred commit failure, reopen/retry, complete stored tuples, index
reload, conflicting watch ownership, live publication after commit and index-read
refusal. The initial four-case suite on the original source had three intended
failures and a passing complete-tuple case. The fifth watch/publication case was
added after the fix; no initial failure is claimed for that case. A required CI
lane enumerates and executes all five cases without changing existing selectors.

Fresh actual backend-ON and backend-OFF configurations built the full daemon and
declared affected test targets. Four ON CTests passed: WalletDatabaseLease
(10.56s), WalletAddressIssuance (10.85s), WalletRecoveryKeys (7.76s), and
WalletRescanUtxoSet (10.09s). The OFF issuance and rescan suites passed in 10.67s
and 10.11s. Both unchanged Orchard root selectors still enumerate 46 enabled
tests; those 46 suites were not executed locally.

All 71 linked project C++ units in the ON wallet test were freshly ASan/UBSan
instrumented. Issuance, lease and key cases passed. Copied original-source,
omitted-transaction and ignored-commit-error controls failed the intended named
regressions; the restored binary passed. Normal and control maps contain no
project C++ archive members. Rust/external libraries remain uninstrumented,
macOS leak detection is off, and the ARM dependency gate remains open. The daemon
and OFF binary were not sanitized. No fresh-process, physical-power-loss, full
node or release-provenance qualification is claimed.
