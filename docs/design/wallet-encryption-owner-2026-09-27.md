# Populated wallet encryption owner

`encryptWallet`, `decryptWallet` and `changePassphrase` now share a checked
wallet database transaction. The existing lifecycle lease pins the selected
wallet. Borrowed transactions and a pinned recovery key refuse before effects.
The existing seed must decrypt and agree with any loaded seed; a policy change
never creates a replacement seed or account.

The transaction rewrites the existing HD seed envelope, encryption settings and
metadata, Taproot imported-key ciphertexts, legacy imported-key payloads, and any
existing P2MR master-key wrapper. Taproot private keys must reproduce their full
stored internal key, output key and selected-network address. Existing imported
payloads must decode to valid private scalars. Ciphertext authentication or
policy mismatches refuse the entire operation. Reads, bindings, writes and
COMMIT are checked. A failed owned transaction rolls back; failure to roll back
an active transaction terminates. The durability policy is synchronous FULL.

The seed remains in the existing version-2 envelope format with its independent
salt and PBKDF2 parameters. Account encryption and mnemonic recovery records are
seed-derived; the policy transaction does not replace them. Existing P2MR master
material is rewrapped, never regenerated. Only after COMMIT does the owner replace
live keys, seed and viewing caches. New encryption locks the wallet; rotation
preserves a still-valid unlocked session or leaves a locked wallet locked.
Temporary owner buffers and replaced live secret caches are cleansed. Existing
cryptographic helper copies and caller-owned keys have separate lifetimes.

## Explicit decryption limit

The existing P2MR master-key format requires an encrypted wallet policy. Explicit
decryption therefore refuses when that wrapper exists. Ordinary wallets without
that wrapper can be explicitly decrypted while preserving imported keys. This
refusal protects existing P2MR ownership; it does not complete support for
unencrypted P2MR wallets or qualify their broader lifecycle.

Legacy imported-key payloads retain their raw or hexadecimal representation when
rewrapped. This is storage migration, not a certificate for the legacy import
registration or signing lookup paths. Incomplete historical records refuse;
there is no automatic metadata reconstruction, plaintext fallback for encrypted
records, new journal, schema format, account recreation or receipt reset.

## Validation scope

Five `WalletEncryptionOwner` cases exercise populated import encryption, locked
and unlocked passphrase rotation, wallet reopen, actual imported-key lookup and
Schnorr verification, seed and existing P2MR master preservation, and ordinary
explicit decryption. Required seed/import/settings/metadata/P2MR writes and
COMMIT failure leave the previous durable and live owner intact. Borrowed
transactions, recovery-key pins, wrong passphrases and malformed legacy
ciphertext refuse. An actual legacy import's payload and label survive a locked
rotation and explicit decryption round trip. That case does not exercise its
legacy signing lookup.

Three initial cases failed the preceding implementation before the repair.
Additional fault checks were added afterward. The required CI lane retains its
inventory and requires all five execution markers. Reopen tests run in the same
process: they are not fresh-process crash or physical power-loss qualification.

Complete encrypted-record discovery and historical reconciliation, P2MR
unencrypted policy, imported registration ownership, pending reservations,
installed chain consumers, independently validated activation, whole-node
restart/reorg and platform/load qualification remain open. Steps 1–4 and the
release are incomplete. Mainnet activation remains unset.

## Local execution

Fresh actual backend-enabled and backend-disabled configurations built the full
daemon and declared wallet targets. Final execution passed nine enabled and
seven disabled CTests, including all five encryption-owner cases.

All 89 project C++ files linked into the wallet test were freshly ASan/UBSan
instrumented after the final source edit. Input hashes were unchanged throughout
compilation. All 31 cases passed. Three copied controls restoring the preceding
owner, omitting imported-key migration or omitting P2MR rewrapping failed their
intended assertions without sanitizer diagnostics; the original binary then
passed all five encryption cases again. Maps exclude project C++ archive members.
The separate full daemon and disabled-backend binary were not sanitizer targets.
Rust, external libraries and PQClean remain uninstrumented; macOS leak detection
was off and the ARM full-RocksDB instrumentation gate remains open.

Both unchanged Orchard root selectors enumerate 46 enabled tests; this does not
claim all 46 were run locally. The new independent encryption lane adds one
CTest to future exact-source CI. These builds are component evidence, not final
release binaries or provenance for the prebuilt OpenSSL dependency.
