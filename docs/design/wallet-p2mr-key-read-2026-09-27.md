# Consistent P2MR key reads and address binding

## Actual consumers

`SignP2MR` and `ExportP2MRSeed` now use `CaptureKeyByAddress` to copy public
metadata and all encrypted seed fields from one SQLite statement. The reader
checks prepare/bind results, all stored field types/ranges, the exact requested
wallet ID/address and terminal `SQLITE_DONE`. Checked absence is distinct from
unavailable, malformed, interrupted or ambiguous storage. Caller-owned
transactions refuse without being committed or rolled back. The caller retains
exclusive use of the store connection; this is not a concurrent connection-owner
API.

The existing checked inventory row decoder is shared with the new reader. The
separate legacy public lookup/seed APIs remain narrower and are not used by these
two handlers anymore. No database row, address, path, label, key or schema is
changed by capture.

Both consumers decrypt with the supplied master using authenticated encryption,
derive the ML-DSA public key, and require equality with the captured public key.
The single-leaf commitment must equal both the recorded root and the root decoded
from the exact requested P2MR address before signature creation or plaintext
export. A refused export returns no seed. Scoped guards cleanse handler-owned
master copies, plaintext seed and generated private key on return or exception.
The existing explicit export result still contains caller-owned plaintext that
its caller must cleanse.

## Qualification

Three initial component cases failed on original `660f362b` before production
changes: missing address-root binding, failed-read classification, and separate
metadata/ciphertext snapshots. Later assertions for individual public-key and
cipher corruption, export during replacement, and signature verification after
replacement were added after those initial failures; no initial-red claim is
made for those individual assertions. The initial test compile used a nonexistent
four-argument Verify overload and was corrected to the actual six-argument API
before the original-source cases ran.

The fixtures use actual import, sign and export handlers with synthetic seeds
and non-HD imported metadata. They preserve imported addresses through reopen,
verify real signatures, distinguish wrong owner/master, refuse type/SQL/EOF and
borrowed-transaction errors, and retain the caller's transaction. A separate
SQLite WAL writer replaces a row while the reader's statement is active: signing
and export use the original coherent snapshot, and a subsequent read refuses the
now-conflicting durable row. This is an in-process snapshot check, not crash or
power-loss qualification.

Fresh backend-on/off daemon and component builds, existing P2MR store and RPC
component regressions, and the actual mixed-input provider regression are
passed in both configurations. All ten linked project C++ translation units
were freshly compiled with ASan/UBSan; three cases passed with 1,228 source/header
hashes unchanged. Copied original-handler, omitted-root-binding and omitted-EOF
controls failed the intended assertions without sanitizer diagnostics; the
original instrumented binary then passed again. External/C/PQClean code remains
uninstrumented, macOS leak checking is off, and the full ARM RocksDB gate remains
open. The daemon JSON adapter and normal provider regression are outside this
ten-unit sanitizer graph. A linker-map parser initially failed to decode binary
path bytes; replacement decoding fixed the evidence reader without relaxing its
archive-member checks. The independent CI lane requires the new enabled CTest and all three
case markers, retaining inventory and verbose logs. The existing 46-test
Orchard selectors are unchanged. Actual CI executions determine totals.

## Limits

This authenticates one captured seed/public-key/address-commitment tuple under a
caller-supplied master. It does not prove an HD derivation path, metadata labels,
timestamps, persistent wallet identity, network selection, all-account inventory,
deleted-address history or master initialization policy. The address codec accepts
supported HRPs; the chain-aware caller still owns expected-network validation.
An as-of snapshot is not continuing spendability or authorization after a wallet
session changes. The separate transaction provider's root lookup, main-wallet
session/master ownership, issuance and store registration are not made atomic by
this change.

No missing key is generated, no historical address is substituted, and no empty
inventory authorizes master creation. Legacy `imported_keys` addresses and tweaks,
explicit initialization versus recovery, staged unlock, authenticated complete
inventory, pending reservations, consumer installation and activation remain
unfinished. JSON transport, RPC selection/admission/broadcast, whole-node crash,
reorg, supply and load qualification remain separate. Mainnet activation is unset.

## Test-runner compiler portability

The first exact-source Linux build rejected class-template deduction of a
`std::pair` containing a function name in the case runner. The runner now declares
`std::pair<const char*, void (*)()>` explicitly in a three-element array. Test
bodies, assertions, error handling, completion markers and production code are
unchanged. Fresh backend-on/off component builds execute the same three cases.
The corrected Linux source still requires actual CI completion; prior macOS
passes do not establish GNU compiler compatibility.
