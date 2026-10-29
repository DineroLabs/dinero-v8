# Reloading known wallet scripts

`LoadAddressesIntoUTXOIndex` captures watched scripts and every nonempty stored
address script together under the actual wallet database lease and a checked
transaction. Finding one watched script no longer hides other recorded address
paths. Existing path records must match the address's script, account, change
flag and index. An address without a path needs a consistent existing watch
record. Malformed records, conflicting paths and failed reads refuse the reload.

The operation does not backfill SQL or construct an HD path from address
metadata. It prepares the entire recognition map before publishing. The index
merges into a copy under its script mutex, refuses conflicting existing paths,
and swaps the map only after the merge succeeds. A refusal leaves the index map
unchanged. Existing unrelated registrations remain intact; this is not stale or
foreign registration removal. The wallet binding service reports reload refusal.
The external index owner must still keep the index alive for the call.

This covers the ordinary known-script domain: watch records plus nonempty address
scripts. Missing/empty address scripts, orphan descriptors, deleted issuance rows,
undiscovered keys and missing accounts remain discovery obligations. Recorded
path consistency is not cryptographic key ownership, authenticated inventory
completeness, spendability, current chain recovery or provider readiness. Watch
labels remain recognition metadata. No delivery receipt, cursor, coin or history
row is modified, and no account or key is recreated.

## Regression coverage

Three cases exercise actual wallet issuance and imported-key persistence before
reloading into an actual index. They cover mixed receive/change/import/watch-only
records when one watch row is missing, reopen, no SQL backfill, conflicting or
missing recorded paths, failed SQL reads, borrowed transactions and an existing
index path conflict. The index-conflict case orders scripts so a deliberately
non-atomic merge would publish an earlier insertion before reaching the conflict.

Two complete initial cases failed against the original reload implementation
before production changes. The third reached the intended refusal assertions,
then hit a fixture SQL quoting error while restoring an HD path containing
apostrophes. The fixture was corrected without weakening its assertions. An
initial test-only compile error passed a string to a C-string helper. The
subsequent deterministic index ordering refinement has no original-source red
claim. Final copied-original controls use the corrected cases.

Actual service/startup and import RPC callers are compiled but are not executed by
these component cases. No fullStart, new process crash, power-loss, whole-node,
load or release qualification is established. Mainnet remains unset and the
production notification provider remains absent.

## Local qualification

Final actual backend ON and OFF configurations built the full daemon and declared
wallet lease/rescan targets; all 11 ON and 9 OFF CTests passed. Both unchanged
Orchard root selectors enumerate 46 enabled registrations, not 46 local executions.
All 89 linked project C++ files were freshly instrumented with ASan/UBSan after
the last source change, with 1307 source/header hashes unchanged. All 37 cases
across ten suites passed. Copied original, omitted-address-inventory and partial
index-publication controls fail intended assertions; the restored three cases
pass. Sanitized/control maps contain no project C++ archive members.

The daemon WalletService code and OFF binary are outside this sanitizer binary.
External/Rust/C/PQClean code is uninstrumented; macOS leak detection is off and
the full ARM RocksDB gate remains open. Inherited build labels and prebuilt
OpenSSL are not release provenance. Whole-node and platform qualification remain.
