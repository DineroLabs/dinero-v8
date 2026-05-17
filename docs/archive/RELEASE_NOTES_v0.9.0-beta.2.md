## DB reliability: RAII transactions + WAL + 5s busy timeout prevent lock storms and nested-txn failures.
## RPC accuracy: getblockcount and getblockhash now reflect DB truth.
## Ops tooling: Smoke test hardened (auto port discovery, health gating, contention guardrail).
## Stability: Restart-safe; mining continues under contention; clean logs.
