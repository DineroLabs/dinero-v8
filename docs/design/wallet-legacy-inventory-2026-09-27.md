# Historical legacy import validation before unlock

The actual staged unlock transaction now reads every present imported_keys row through terminal SQLITE_DONE. Address and ciphertext must be TEXT with explicit byte lengths; encrypted raw 32-byte scalars and encrypted 64-character hex scalars are accepted through the established authenticated decryption owner. Invalid types, ciphertext framing, decryption, scalars, late rows or incomplete reads refuse before live publication.

For each scalar, the code derives its x-only public key, applies the predecessor SHA256(internal_xonly || 0x00) tweak and requires the exact historical MAIN address. It does not substitute modern TapTweak or the current network. No imported row, label, script/path mapping, account, cursor or key is rewritten or regenerated; temporary plaintext is scoped and cleansed. The predecessor writer could truncate binary ciphertext by using a null-terminated SQL binding; damaged records are refused, never reconstructed or normalized.

This authenticates every PRESENT legacy imported scalar/address tuple during ENCRYPTED unlock. It does not establish completeness against deletion or backup rollback; validate unencrypted-wallet discovery; authenticate every modern Taproot/PQ/account archive; reconcile recorded address/script indexes; or establish signing support for the historical tweak. Existing generic legacy key lookup and transaction signing remain separate open work. Full recovery/readiness and release gates remain open.

Three enabled 60-second component tests cover both encrypted payload encodings with embedded-NUL ciphertext and reopen preservation; a malformed later row, authenticated wrong scalar/address binding, wrong SQL type and truncated data; and denied/interrupted reads with state preservation and successful retry. Qualification results require actual completed execution.

## Delivery identity regression fixture

The completed predecessor initial-owner CI exposed an outdated assumption in the delivery identity rollback test: new creation now persists its identity before sealing the initial owner. The fixture first checks that this established identity is returned without an UPDATE. It then explicitly constructs the predecessor schema (no initial-owner record or delivery-id column) to exercise lazy enrollment. All original write/commit rollback, borrowed transaction and malformed identity assertions remain. No production identity checks are removed. The final qualification includes both delivery-binding lanes and the lease/recovery suites.

## Local qualification

Fresh enabled and disabled full daemon and declared wallet targets built. Each configuration passed30selected CTests including both delivery-binding lanes and lease/recovery suites. All69 linked project C++ translation units in the mainnet-readiness component were freshly ASan/UBSan instrumented after final source edits;1287source/header hashes remained stable and20cases passed. Copied omitted-inventory, omitted-address-binding and omitted-EOF controls each failed the intended assertions without fixture exceptions or sanitizer diagnostics; restored3cases passed.

The first ON/OFF26selected tests passed before expanded final qualification. The unchanged delivery fixture reproduced the completed7304Linux failure locally before its setup repair; its original assertion body remains byte-identical. The separate lease/delivery test binary is outside the69instrumented files and was qualified by actual normal ON/OFF execution. External/Rust/C/PQClean instrumentation, macOS leak detection, full ARM RocksDB, whole-node and release gates retain their previously documented limits. No initial original RED claim is made for the new inventory cases.
