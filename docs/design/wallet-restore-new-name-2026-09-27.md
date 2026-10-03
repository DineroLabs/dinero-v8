# Restoration requires a new wallet name

`RpcRestoreWallet` rejects an existing registered name before opening it or changing policy, encryption, seed, mnemonic, birthday, addresses or selection. Legacy positional and named `replace_existing` arguments cannot enable replacement. The existing creation owner also rejects pre-existing database files, including files absent from the registry. The encryption-reset helper and seed-replacement branch have been removed from restore.

Restore under a new name to keep the original wallet separately accessible with its original passphrase. The new wallet contains the supplied recovery identity and uses its own optional encryption password. A mnemonic does not recover separately imported keys or a historical random PQ master.

## Regression contract

The former overwrite/re-encrypt test is replaced by `RestoreRequiresNewNamePreservesOriginal`. It prepares an encrypted wallet with an imported key and established PQ master, refuses both parameter forms and legacy boolean/string overrides, and compares durable fixture files (including registry and WAL journals; volatile SQLite shared-memory reader marks excluded), selected database, session and lock state. It restores under a new name, refuses an inactive existing target without switching selection, then reopens both wallets and checks their original/recovery mnemonic and independent passphrases, original seed ciphertext, imported private key and PQ master.

`RestoreRefusesUnencryptedAndUnregisteredTargets` covers an unencrypted wallet and a pre-existing unregistered file. Both cases remain enabled with the existing 60-second deadline. The independent Orchard lane requires both exact CTest registrations and successful execution markers. The previous three fresh-restore cases remain unchanged.

These are synchronous RPC-component and SQLite tests. They do not qualify JSON network transport, concurrent processes, physical power loss, whole-restore atomicity, or release readiness. Durable new-identity versus historical-recovery ownership, staged unlock, authenticated complete inventory and the remaining node/platform gates stay open. Mainnet activation remains unset.

## Local qualification

Fresh backend-enabled and backend-disabled builds each built the full daemon and declared wallet targets and passed all 15 selected CTests. All 69 linked project C++ translation units were rebuilt with ASan/UBSan; nine recovery cases passed with 1,287 source/header input hashes unchanged. A copied-original RPC handler failed the new preservation assertions, and the restored implementation passed both new cases. External dependencies and the separate daemon/OFF binaries are outside that sanitizer scope; macOS leak detection is off. Linux exact/full/heavy qualification remains separate and pending.
