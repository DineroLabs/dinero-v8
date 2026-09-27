# Staged HD ownership inventory

Encrypted `unlockWallet` authenticates present BIP84/BIP86 ownership records from its staged 64-byte seed inside the existing checked FULL transaction, before live key/seed/PQ/view authority is published. `AuthenticateHdInventory` reads every `address_derivation_paths` row, including orphan rows through a LEFT JOIN, and requires:

- A complete five-component path with three hardened components, a receive/change component of 0 or 1, and a normal index. Each component fits 31 bits; supported purposes are 84 and 86.
- Typed account/change/index and optional wallet-ID columns consistent between the path and address records.
- The exact public derivation, address encoding, script, address type, and every present KeyID. Older NULL KeyIDs remain absent.
- An exact watched script/path/change binding. Consistent script/path repetitions remain consistent; conflicting owners refuse. The current address schema prevents duplicate account/change/index tuples, so tests of historical network encodings use distinct recorded indices. Alias creation is not qualified. A reverse pass rejects explicit BIP84/BIP86 watch claims without an authenticated companion tuple.
- Successful SQL completion for both complete scans and individual companion reads.

Derivation uses the scoped, cleansing `BIP32Deriver`; no derived private key escapes it. SQL text uses explicit lengths and rejects embedded NULs. The recorded coin component, nonconsecutive accounts, and supported historical Dinero address prefixes are preserved. Authentication of a historical network encoding does not authorize spending on that network. The existing wallet-open rejection of retired coin type 1447 is unchanged; successful reopen tests use coin type 1448. Compatibility for retired paths is not qualified by this check. No rows, labels, paths, identifiers, accounts, cursors, or caches are rewritten or recreated.

## Boundaries

This authenticates **present explicit HD ownership claims**. It is not an authenticated completeness catalog: deletion of an entire related record set, undiscovered addresses, backup rollback, descriptor coverage, watch-only ownership, account archive completeness, and issuance high-water marks remain separate work. Other watch namespaces retain their existing handling. Missing old companions refuse; the check neither infers a path from account labels nor repairs it during unlock. Unencrypted/seedless discovery and generic signing lookup behavior are outside this change.

Failures preserve prior live authority, including a valid re-unlock, and the existing transaction rolls back. The database lifecycle lease remains held throughout. Cross-process and physical power-loss qualification are not established by the in-process tests.

## Local qualification

Three enabled 60-second tests cover actual receive/change issuance and independent historical BIP84/BIP86 records, nonzero accounts and recorded coin/network preservation, malformed/coherent-wrong bindings and missing companions, failed re-unlock, denied reads, interrupted scans, and retry. Existing fixture assertions and deadlines remain unchanged. Fresh ON/OFF full daemon and declared wallet target builds passed, with 44 actual selected CTests in each configuration. All 76 linked project C++ files were freshly instrumented with ASan/UBSan; 34 cases passed with 1,296 unchanged source/header inputs. Three copied omission controls failed their intended assertions; restoring the implementation passed all three new cases. Existing readiness fixture bodies and 100 prior workflow CTest commands were preserved.

The instrumentation excludes the daemon, separate database-lease binary, backend-off binary, external libraries, Rust, C and PQClean; macOS leak detection was disabled. The separate prior ARM RocksDB qualification retains its own scope. Linux qualification for this exact revision is required independently. These results do not establish whole-node or release readiness.
