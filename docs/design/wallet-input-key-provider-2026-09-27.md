# Input-bound wallet key providers

The ordinary `TransactionSigner` previously looked up keys by each coin's path.
Imported Taproot paths are shortened origin labels, so the RPC key maps could
not reliably identify their signing material through that interface.

`KeyProvider::GetPrivateKeyForInput` now checks an exact `txid:vout` entry first.
If present, that entry is authoritative for lookup: a wrong or empty key cannot
fall back to a path entry. Imported `tr(...)` labels cannot be lookup fallbacks.
Existing non-import path lookup remains available. The signer still verifies the
full derived Taproot output against the coin's script before producing a witness.
It also checks that every supplied coin matches its transaction input before any
key lookup or signing begins.

The actual `sendtoaddress`, consolidation and legacy shield-with-covenant callers
now supply input-keyed maps and limit HD derivation fallback to `m/` paths. The
hybrid provider's existing configuration field name is retained for compatibility;
its map accepts input identifiers as well as legacy paths. P2MR inputs continue
through their separate encrypted-seed signing path, before ordinary key lookup.

## Qualification boundaries

Three `WalletInputProvider` tests exercise the actual signer and map/hybrid
providers. Stored, reopened encrypted Taproot imports sign two-input transactions
despite conflicting path entries. Missing input bindings and wrong keys refuse.
An actual HD address retains path fallback. Signatures verify against computed
transaction messages and reject changed amounts; unsigned transaction structure
is retained. Supplied coins that do not match transaction inputs refuse.

The mixed-input case creates a real encrypted P2MR store entry with synthetic
BIP32 fixture material, then signs imported P2TR and P2MR inputs together through
`WalletKeyProvider`. The P2MR verification primitive accepts the witness and
rejects a changed message. Wrong master keys, wrong wallet scope and missing
stores refuse. This covers the provider's existing P2MR obligations at component
level; it does not certify the production P2MR wallet owner or activation.

All three initial cases failed against the preceding source before the provider
repair. The transaction-input correspondence assertion was added later. Coins
are synthetic supplied candidates. RPC handlers are compiled, not executed by
this lane. Selection, chain unspentness, full consensus execution, mempool
admission, broadcast and complete shield/send/unshield remain unqualified here.
Caller-owned key lifetimes and ownership across an entire signing job remain
separate integration work.

The required CI lane retains an exact inventory and requires all three execution
markers. No new journal, account recreation, receipt reset, provider installation
or mainnet activation is introduced. Existing-import encryption migration,
complete discovery and reconciliation, consumer readiness and independently
validated first activation remain open. Steps 1–4 and the release are incomplete.

## Local execution

Fresh backend-enabled and backend-disabled configurations built the full daemon
and declared wallet targets. Eight enabled-backend and six disabled-backend
wallet CTests passed. Existing ownership, BIP341 sighash and wallet round-trip
regressions passed; the round-trip test uses a mock mempool.

All 89 project C++ translation units linked into the wallet test binary were
freshly instrumented with ASan/UBSan. All 26 cases passed. Three copied controls
restoring path-only dispatch, allowing imported-label fallback or omitting input
correspondence failed their intended assertions without sanitizer diagnostics.
The restored binary passed all 26 cases. Link maps exclude project C++ archive
members. This includes the actual hybrid provider and P2MR creation/signing
helpers, but excludes the changed RPC entrypoints, daemon, disabled-backend
binary and separate regression binaries.

Rust, external libraries and C/PQClean remain uninstrumented, macOS leak detection
is off, and full RocksDB ARM instrumentation remains an open gate. The unchanged
Orchard root selectors enumerate 46 enabled tests; all 46 were not run locally.
These builds are not release binaries, whole-node tests or crash/power-loss proof.
