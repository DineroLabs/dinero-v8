# Imported Taproot transaction signing

The existing Taproot signer rejected ordinary imported keys because their stored
`tr(...)` origin labels are not HD paths. Its ordinary and V1 entry points now
accept the canonical import label derived from the supplied internal public key.
Both still require the full derived TapTweak output to match the UTXO script
before creating any witness. A label alone never authorizes a signature. Existing
HD and confidential-input origin handling remains in place.

`TransactionBuilder` now prefers an exact `txid:vout` key binding. Imported labels
are shortened display identifiers, so imported candidates require that outpoint
binding and cannot use path or single-key fallback. The actual `sendmany` caller
now supplies outpoint entries and restricts its legacy derivation fallback to HD
paths. The distinct `sendtoaddress`/`MapKeyProvider` path is not changed here.
Header comments now correctly describe the supplied keys as untweaked internal
keys, consistent with the existing implementation.

## Tests and boundaries

Three new `WalletImportedTransaction` cases use the actual wallet import and
lookup owner, including an encrypted wallet reopened before lookup. The tests
exercise both actual transaction-signing entry points, verify Schnorr signatures
against their computed transaction messages, and require an amount mutation to
invalidate the signature. Wrong keys, absent origins and mismatched import labels
refuse before witness mutation. A two-input transaction through the real builder
uses exact outpoint keys despite conflicting path-key entries; missing outpoint
bindings refuse. The initial source failed the signing and builder cases, while
the initial refusal case already passed.

The candidates are synthetic, explicitly supplied to the builder. These tests do
not certify chain unspentness, independent consensus execution, activation,
selection readiness, RPC execution, mempool admission or broadcast. The RPC
change is compiled in both daemon configurations. Existing caller-owned raw key
lifetimes and the separate sendtoaddress provider remain broader integration
work. Previously plaintext imported keys still require a complete wallet
encryption migration; the encrypted test imports after wallet encryption.

The required CI lane checks all three execution markers and retains inventory
and logs. No journal, account recreation, receipt/reset, provider installation or
mainnet activation is introduced. Steps 1–4 and release qualification remain open.

## Local qualification

Fresh backend-enabled and backend-disabled configurations built the full daemon
and declared wallet targets. Seven enabled-backend and five disabled-backend
wallet CTests passed, together with the existing ownership and BIP341 sighash
regressions. All 80 project C++ translation units linked into the wallet test
binary were freshly instrumented with ASan/UBSan; its 23 cases passed. Copied
original-signer, original-builder and omitted-output-binding controls failed the
intended assertions, then the restored binary passed. Link maps exclude project
C++ archive members.

External libraries and Rust remain uninstrumented, macOS leak detection is off,
and the full RocksDB ARM instrumentation gate remains open. The disabled-backend
binary, daemon/RPC and extra regression binaries are outside this sanitizer
scope. The unchanged Orchard root selectors enumerate 46 enabled tests; this is
an inventory check, not a claim that all 46 ran locally. These builds are not
release binaries or crash/power-loss qualification.
