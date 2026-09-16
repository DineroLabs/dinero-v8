# Batch Utreexo verification: canonical coin lookup

## Failure and cause

PR #755, head `4a11b3c6b36bacb61050d2acd1f753ba1169b23f`, failed
`ShieldedAuthRelayLifecycle` in Actions run `35124273501`. After invalidating
and reconsidering the unshield block, the daemon had committed the output and
restored its Utreexo position. `getutxoproofs_batch` generated its proof, while
`verifyutxoproofs_batch` returned `utxo-not-found`.

The verifier called `ChainstateService::getUTXOIndex()`. Despite an outdated
comment describing this as consensus authority, that accessor returns the
wallet-owned SQLite index. The reconnect log shows the wallet worker queued
behind canonical block application. The same error is deterministic on a node
whose wallet does not own the output, with no race required.

## Change and limits

Verification reads value, script, creation height and coinbase status from
`ChainDB::getCoin`, the canonical source already used by `getutxoproof`.
Missing coins, database errors and malformed stored metadata fail closed.
`HashUTXOForCreationHeight`, forest snapshot locking, proof verification,
consensus validation and accumulator mutation are unchanged.

This is a real proof-serving RPC fix, not a sleep/retry or relaxed assertion in
the failing harness. It does not activate compact transaction encoding. Proof
requests spanning a concurrent chain transition can still require a refreshed
proof; this patch does not introduce an atomic multi-RPC snapshot contract.

## Test-first qualification

`UtreexoBatchProofCanonicalCoins` starts two isolated regtest nodes, mines to
one wallet, and confirms the selected mature coin exists canonically but is
absent from the other wallet. Before the fix, proof generation succeeds and
verification fails with the same `utxo-not-found` error as CI. After the fix:

- Both nodes verify the coin, including after non-owning peer restart.
- Corrupting a sibling is rejected by the unchanged proof verifier.
- A nonexistent outpoint is rejected.
- A signed child spends the exact coin; its old proof is rejected on both nodes.
- Disconnect restores the coin and a verifiable proof; reconnect restores
  spentness on both nodes.

Local macOS arm64 regression passed in 19.76 seconds. The original full
`ShieldedAuthRelayLifecycle` also passed, including real shield/transfer/unshield,
restarts, disconnect/reconnect commitments, and mining its signed transparent
child. The new test is registered and explicitly selected in the serial CI
lane; no baseline exemption is added. Full Linux qualification remains pending.

Raw red/green logs, the preserved old daemon, and CI-selection evidence are in
this worktree's ignored `build-review/evidence/` directory.
