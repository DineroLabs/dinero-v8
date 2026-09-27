# Historical imported-key reads

`WalletManager::deriveKeyForScriptPubKey` now resolves predecessor
`imported_keys` records in its existing lifecycle lease and checked FULL SQLite
transaction before consulting a private-key cache or HD path. A matching modern
import owner conflicts with the predecessor owner and refuses the lookup.

The reader checks SQL TEXT storage with explicit byte lengths, including embedded
NULs in historical ciphertext. Unencrypted records retain the historical hex64
format; encrypted records authenticate the established raw32 or hex64 payload.
Durable encryption settings and metadata must agree with the live policy. The
scalar must be valid and its historical `SHA256(internal_xonly || 0x00)` public
tweak must reproduce the exact requested script and recorded MAIN address.
Prepare, bind and terminal `SQLITE_DONE` are checked. A borrowed transaction is
left untouched. Temporary plaintext is cleansed and imports are not cached.
The returned copy belongs to the caller; the lease ends with this lookup and
does not authorize an entire later signing operation.

This replaces the old fallback's null-terminated binary read and manual statement
lifetime. It does not rewrite rows, labels, paths, accounts or scripts. The return
contract remains an **internal scalar**, not a tweaked output scalar. Historical
transaction signing still requires an explicit historical policy through the
signer and its callers; canonical TapTweak signing is not interchangeable.

Three enabled 60-second component cases exercise plaintext/encrypted formats,
reopen, unchanged durable/live state, malformed owners despite a preexisting
cache entry, denied/incomplete reads, retry and borrowed-transaction refusal.
Production and fixtures were introduced together; no original-source failing
execution or crash reproduction is claimed. Copied omission controls cover the
new binding, EOF and cache ordering separately.

This is a checked lookup for a present predecessor record, not an authenticated
complete inventory, deletion/backup rollback certificate, historical spending
qualification, HD fallback reconciliation, or release readiness. A missing record
still permits the existing HD path. Modern import and ordinary HD behavior retain
their existing contracts. Mainnet activation remains unset.

## Local qualification scope

Fresh backend-enabled and backend-disabled full daemon builds and both declared
wallet components passed, with 41 selected CTests in each configuration. The
main wallet fixture links 76 project C++ units; all were rebuilt with ASan/UBSan
and its 31 selected cases passed. External libraries, Rust, C/PQClean, the daemon,
the separate lease fixture and the backend-disabled binary are outside that
instrumented graph. macOS leak detection is disabled; full ARM RocksDB sanitizer
qualification remains open. These component results do not establish release
readiness or historical transaction signing.
