# Checked existing seed envelope reads

`WalletManager::loadMasterSeed` now captures the established seed envelope with
one checked SQLite statement under its existing lifecycle mutex. It requires a
124-byte BLOB and an integer version 1 or 2, or the explicit legacy unmarked
version 0/NULL. Unsupported, negative, oversized or coerced versions refuse.
The statement must finish with `SQLITE_DONE` before password derivation or seed
return. Statement ownership is RAII-managed even if allocation fails.

Version 2 retains 600,000 PBKDF2-HMAC-SHA512 iterations; version 1 retains 100,000.
Only the legacy unmarked envelope tries both, and authenticated decryption must
produce exactly 64 bytes. The envelope's embedded salt remains authoritative;
this change does not add authentication of the separate metadata salt column.
The reader preserves its existing optional-return interface, which does not
classify absence separately from malformed or unavailable storage.

## Ownership and scope

This is a read-only prerequisite for staged unlock. It neither publishes a live
seed nor changes encryption policy, the unlocked flag, PQ keys, mnemonic records,
account archives or historical imported addresses. Reads inside a caller's
transaction remain supported without commit or rollback. This adds no database
lease/session API or whole-operation ownership beyond the existing lifecycle
mutex. Callers still own live-seed matching and wider transaction consistency.

Unlock publication ordering and explicit PQ initialization versus recovery remain
open. Missing or empty PQ storage never proves that a master may be regenerated.
New-wallet initialization must be distinguished from missing established metadata
before replacing the documented first-unlock behavior. No key, account, address,
path, journal or readiness receipt is created by this change.

## Qualification

Three cases exercise the actual private loader through a narrow friend accessor,
with real wallet creation/encryption and isolated SQLite databases. Current and
legacy authenticated envelopes preserve the seed, remain locked and survive
reopen. Version-zero fallback is exercised for both KDFs; the retained NULL branch
is not separately seeded. Malformed types/versions, truncated ciphertext and
interrupted/denied reads refuse. Borrowed transactions and live owners remain
unchanged. The public encryption caller also refuses an unsupported seed version
before changing policy or seed state.

Before production edits, current/legacy compatibility passed and the two refusal
cases failed on the original loader. The public encryption-caller assertion was
added afterward and has no initial-red claim. Initial test compilation exposed
the private method boundary and a missing OpenSSL declaration; a narrow friend
accessor and explicit header corrected the harness before the original-loader
cases executed. A mistaken utility namespace and hex-display case were corrected
before that compilation. Assertions and deadlines were not weakened.

Fresh genuine Orchard ON and OFF configurations built the complete `dinerod`
and declared wallet test target. Seven selected ON CTests and five OFF CTests
passed. All 89 linked project C++ translation units were freshly rebuilt with
ASan/UBSan; 40 cases across 11 suites passed, with 1,307 source/header hashes
stable during compilation. External/Rust/C/PQClean libraries remain
uninstrumented and macOS leak detection is disabled. The daemon and OFF binary
are outside that instrumented graph; the full ARM RocksDB gate remains open.

Copied original-loader, omitted-EOF and coerced-version controls each failed the
intended named regression without sanitizer diagnostics; the restored three
cases passed. These controls modified copied sources only.

The independent CI lane requires the enabled `WalletSeedRead` registration and
all three named executions, retaining the inventory and verbose log. Existing
Orchard root selectors remain unchanged. Actual completed runs determine totals.
Full-node startup, unlock publication, mnemonic/account completeness, pending
ownership, activation, crash/reorg/supply/load and full dependency/platform
qualification remain separate. Mainnet activation is unset.
