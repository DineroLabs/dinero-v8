# Forward private-key imports

`importPrivateKey` now derives the current network's Taproot address using the
same key derivation as the ordinary signer and delegates persistence to the
existing checked Taproot key owner. It no longer writes an independent key row,
replaces imported address slot zero, or publishes an imported plaintext cache.
The owner persists the key, public mapping, watched script and imported address
in its existing FULL transaction, then publishes to the live index.

This is a forward-import path. Existing legacy imported-key records cause this
route to refuse before effects. The inventory check runs inside the persistence
transaction, so a successful preflight cannot be detached from its writes.
Missing inventory schema or a failed read also refuses. Legacy records retain
their bytes and identity; they are not converted to a different address or HD
path. Historical reconciliation and legacy signing remain separate release gates.

The single-key and backup-import RPCs capture the selected wallet session before
preparing input. A stale session cannot import into a subsequently selected
wallet. Backup imports require a nonempty recorded address and verify that the
provided key derives that exact address before persistence. A historical address
mismatch is counted as a failed import. A multi-key file remains a sequence of
individual imports, not one all-or-nothing file transaction.

The single-key RPC explains that it uses current Taproot address rules. A raw key
alone does not establish which historical address format a user needs to recover.
This change does not scan historical variants or certify complete recovery.

## Validation scope

Three component cases exercise the actual API: multiple imports beside an
existing descriptor import, repeated import, encrypted creation and reopen,
actual key lookup and Schnorr verification; expected-address and stale-session
refusal; borrowed transactions, required key-write failure, locked wallets and
legacy-inventory/read refusal. Existing imports remain usable after each case.
The required CI lane retains the inventory and checks all three case markers.

Tests and implementation were added together; no original-source red claim is
made. The existing encryption regression now seeds an explicit predecessor-format
legacy row because the forward API no longer produces that storage format. Its
payload/label rotation and decryption assertions remain in place.

RPC entrypoints are compiled, not exercised by the component cases. Full backup
semantics, legacy reconciliation, authenticated key/account inventory, historical
scanning, reservations, selection, admission, broadcast and complete Orchard
consumer/activation integration remain open. Mainnet activation is unset; steps
1–4 and release qualification are incomplete.

The existing post-commit live-index publication behavior is inherited: an
exception can report refusal after the durable tuple has committed. This branch
is not newly injected. Component fixtures use their existing network setup;
other network address variants are not separately qualified by these cases.

## Local qualification

Fresh actual backend-enabled and backend-disabled configurations built the full
`dinerod` and declared wallet lease/rescan targets. All ten selected ON CTests
and eight OFF CTests passed. Both unchanged Orchard root selectors enumerate
46 enabled registrations; those 46 were not all executed locally.

All 89 project C++ translation units linked into the wallet test binary were
freshly built with ASan/UBSan. The source/header input hashes remained unchanged;
34 cases across nine suites passed. Copied controls omitting expected-address,
legacy-inventory, or selected-session binding each failed the intended named
regression without sanitizer diagnostics. The restored three import cases passed.
The sanitized and control maps contain no project C++ archive members.

The RPC entrypoint and daemon are outside that sanitizer binary; external, Rust,
C and PQClean code are uninstrumented and macOS leak detection is off. The ARM
full-RocksDB instrumentation gate remains open. Inherited build labels and
prebuilt OpenSSL are not release provenance. These tests do not qualify physical
power loss, a new process after a crash, whole-node recovery, load or a release.
