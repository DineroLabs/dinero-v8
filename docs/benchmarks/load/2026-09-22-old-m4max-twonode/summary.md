# Two-node baseline: old (v1 Auth proofs)

Host: M4 Max (macOS 27, 12-core), Release build-spike, daemon 4ed198529

## W2 catch-up: B validates blocks whose proofs it never saw

- blocks produced while B was offline: 3, proofs total 24 (per block: [8, 8, 8])
- synced: True, time to tip after restart 23.6 s; per-block gap s: {'n': 2, 'p50': 7.767010791983921, 'p95': 7.7783542920369655, 'p99': 7.7783542920369655, 'max': 7.7783542920369655, 'mean': 7.772682542010443}
- B CPU % of one core mean 99; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}; A {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during catch-up | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 24 | 1 | 2 | 3 | 3 | 0 | 0 |
| getrawmempool | 24 | 1 | 1 | 3 | 3 | 0 | 0 |
| wallet.shieldedbalance | 24 | 1 | 3 | 3 | 3 | 0 | 0 |
| getblocktemplate | 6 | 3 | 7944 | 7944 | 7944 | 6 | 3 |

## W3 reorg: B abandons its own 2 blocks for A's longer chain with cold proofs

- fork base 144; A side 3 blocks / 24 proofs; converged to A: True in 23.4 s
- B height timeline: [(144, 0.0), (145, 8.0), (146, 15.5), (147, 23.3)]
- B CPU % mean 97; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during reorg | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 24 | 1 | 1 | 3 | 3 | 0 | 0 |
| getrawmempool | 24 | 1 | 1 | 3 | 3 | 0 | 0 |
| wallet.shieldedbalance | 24 | 1 | 1 | 2 | 2 | 0 | 0 |
| getblocktemplate | 2 | 3 | 23247 | 23247 | 23247 | 10 | 1 |

qualification failed: False
