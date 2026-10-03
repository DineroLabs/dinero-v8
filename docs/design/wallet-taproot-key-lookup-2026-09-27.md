# Stored Taproot import key lookup

`WalletManager::deriveKeyForScriptPubKey` now resolves the durable tuple written
by `storeTaprootKey` before consulting the HD key cache or derivation paths.
Previously, the persisted `tr(...)` watch label reached the BIP32 parser, which
refused it; the alternate import lookup read a different table. The new tests
failed on the original lookup before this change.

## Ownership and reads

For a P2TR script, the actual wallet lifecycle owner and `DatabaseLease` pin the
selected database and key policy. The existing checked transaction owner gives
candidate, tuple and encryption-policy reads one SQLite snapshot. A borrowed
transaction or pinned recovery seed refuses; an expired unlock is checked before
material is returned. This consistent-read requirement also applies to the P2TR
probe before ordinary HD fallback. No wallet rows, receipts or identities change.

An imported address, import watch label, or key record requires a complete,
compatible tuple: the exact network address/script, imported account `-1`, public
mapping, watch label, public key blobs and encryption flag. Durable encryption
settings and metadata must agree with the live owner. Encrypted blobs use the
existing authenticated decryption routine. The returned private key must derive
the stored internal x-only key and the requested TapTweak output. Missing or
incompatible records refuse, without backfilling or guessing an HD path. Imported
plaintext is not cached; temporary lookup-owned secret buffers are cleansed.
The returned raw-key vector retains the existing caller-owned API lifetime.

## Qualification

Two `WalletTaprootLookup` cases exercise the real wallet/database and actual
script-to-key method. They cover plaintext and encrypted imports, reopen/lock,
wrong selected wallet, valid Schnorr signatures against the output key, damaged
ciphertext, committed tuple corruption/deletion, denied SQL reads, and preserved
caller transactions. Ordinary HD lookup is also checked. This signs a fixed test
message through the real signing primitive; it is not a transaction/RPC test.
The required CI lane verifies both execution markers and retains its inventory
and log. Existing Orchard root selectors remain unchanged.

Fresh backend-enabled and genuinely backend-disabled configurations build the
full daemon and declared wallet targets. Six enabled-backend and four
disabled-backend CTests pass. The wallet binary's 71 linked project C++ units
are freshly ASan/UBSan-instrumented; the lookup, import, issuance, lease and
recovery-key cases execute. Original-code and omitted-mapping-check controls
fail their intended assertions. External/Rust libraries are uninstrumented,
macOS leak detection is off, and the full ARM RocksDB qualification gate remains
open. The daemon and disabled-backend binary are outside that sanitizer scope.
These are component checks, not release provenance or whole-node qualification.

## Remaining work

This is forward imported-key resolution, not complete discovery, reconciliation
of older incomplete imports, selection readiness, or working imported sends.
`TaprootTxSigner::SignInput` still has an HD-path gate that must be reconciled with
real imported ownership. Existing `hasSigningMaterialForScriptPubKey` is not this
verified lookup. Encryption of an already populated plaintext import wallet and
broader passphrase/decryption migration remain unresolved; policy/flag mismatches
refuse here. The tests import encrypted keys after wallet encryption.

No provider, startup integration, activation, pending/mempool reservation owner,
new journal, cursor reset or account recreation is introduced. Mainnet activation
remains unset. Steps 1–4 and release qualification remain incomplete.
