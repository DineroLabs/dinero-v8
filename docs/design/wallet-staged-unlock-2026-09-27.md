# Staged wallet unlock

The actual `unlockWallet` operation now owns the wallet lifecycle lease and a checked FULL transaction before reading policy and credentials. It requires typed encryption policy and verification records, authenticates the password (retaining verified legacy HMAC without migration), decrypts the required 64-byte seed, and prepares viewing keys in scoped buffers. Missing or malformed seed data no longer leaves the wallet marked unlocked.

The same transaction reads and authenticates both the initial identity record and any existing PQ password wrapper. Present corrupt records fail. When both contain a master, the masters must agree. A missing wrapper may be inserted using the same creation-owned master, never fresh randomness. Required reads, insert and COMMIT are checked. Only after successful commit do nonthrowing swaps publish credentials, seed, viewing caches, PQ authority and timeout; the unlocked flag is last. Failures preserve the prior live state, including an already valid unlock, and retain durable key material. Existing viewing caches intentionally survive locking; a failed unlock does not replace them.

Closing or switching wallets now also cleanses the PQ master cache and clears its loaded flag, alongside the existing seed and credential cleanup. The unchanged restore-preservation test exposed the stale previous-wallet cache when switching from the original wallet to a recovered wallet.

## Scope

This stages the existing credential/seed/PQ/viewing-key path, not a certificate of all wallet keys or readiness. Recovery with no historical PQ master, and legacy wallets with neither master record, retain HD-only unlock with PQ unavailable. Absence does not authorize master generation or establish complete historical ownership. Present but corrupt/mismatched records now refuse unlock; previous initial-owner tests strengthen that contract while retaining their no-key/no-wrapper checks.

Seedless wallets now refuse unlock until authenticated non-HD ownership is supported. Complete imported-key/script/account inventory authentication, exact historical imported-address reconciliation, detection of deleted owners or whole-backup rollback, multiple processes, account/archive validation and production notification integration remain open. The four legacy viewing-account caches remain the existing policy, not complete Orchard account discovery. No account recreation, path relabel, cursor reset, mainnet activation or deployment is introduced.

## Qualification contract

Four new enabled 60-second component tests cover unavailable seed/policy/credentials and failed re-unlock preservation; denied reads, borrowed transactions and wrapper commit failures; SQL-free commit observation proving no early live publication; authenticated but disagreeing PQ owners and malformed present records; and verified legacy-HMAC credential retention. Existing initialization/recovery and restore-preservation tests remain required. Results must come from completed execution, not test registration alone.

## Local qualification

Fresh Orchard-enabled and Orchard-disabled full daemon and declared wallet targets built successfully. Each configuration executed all 23 selected CTests successfully. All 69 project C++ translation units selected by the actual wallet-test link map were freshly instrumented with ASan/UBSan; 1,287 source/header hashes remained stable and 17 component cases passed. Copied previous-unlock, omitted-PQ-comparison and early-publication controls each failed the intended assertion without fixture exceptions or sanitizer diagnostics; restoring the implementation passed all four new cases.

The first fixture compile used an unavailable lowercase hash function name and was corrected to the already included OpenSSL API. The first enabled test run found the retained PQ cache on wallet switching; the production close cleanup was repaired and the unchanged preservation test passed in both final configurations. No initial pre-implementation RED claim is made for the staged-unlock cases.

Instrumentation covers the linked project C++ graph only. External libraries, Rust, C and PQClean remain uninstrumented; macOS leak detection is disabled. The full daemon, disabled configuration and separate lease test binary are outside that 69-file instrumented graph. The full ARM RocksDB instrumentation gate remains open. These component results do not establish transport, whole-node, crash/power-loss or release qualification; Linux checks must independently qualify the committed source.
