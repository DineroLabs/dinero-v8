## What changed
Introduced RAII transaction helper SqliteTxn (SAVEPOINT-backed, exception-safe).
Migrated all write paths to RAII; removed legacy manual transactions.
Standardized SQLite open: WAL, foreign_keys=ON, busy_timeout=5000ms.
RPC reads height from chain_state.best_block_height; hashes from block_index BLOB → hex.
Hardened smoke test: port discovery + health gating + contention guardrail.
Fixed port collision/unified-port behavior in tests.

## Why
Eliminate nested transaction errors.
Make DB concurrency robust under contention.
Ensure RPC/DB truth alignment; unblock downstream services.

## Proof
healthz OK; height advances (+10/5s).
RPC ↔ DB parity exact (e.g., 1154 ↔ 1154).
Hash parity @1 exact.
WAL active for all DBs.
Logs clean (no SQLITE_BUSY / nested txn) in final runs.

## Operational notes
PRAGMA busy_timeout is per-connection; CLI may print 0. Daemon logs show the effective 5000ms.
meta.height is legacy; authoritative height is chain_state.
