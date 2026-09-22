# Two-node baseline: old (v1 Auth proofs)

Host: AMD EPYC 7763 64-Core Processor, 4 vCPU

## W2 catch-up: B validates blocks whose proofs it never saw

- blocks produced while B was offline: 2, proofs total 16 (per block: [8, 8])
- synced: True, time to tip after restart 17.3 s; per-block gap s: {'n': 1, 'p50': 17.332509387000073, 'p95': 17.332509387000073, 'p99': 17.332509387000073, 'max': 17.332509387000073, 'mean': 17.332509387000073}
- B CPU % of one core mean 93; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}; A {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during catch-up | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 18 | 1 | 2 | 3 | 3 | 0 | 0 |
| getrawmempool | 18 | 1 | 1 | 2 | 2 | 0 | 0 |
| wallet.shieldedbalance | 18 | 1 | 1 | 2 | 2 | 0 | 0 |
| getblocktemplate | 2 | 6 | 17322 | 17322 | 17322 | 7 | 1 |

## W3 reorg: B abandons its own 2 blocks for A's longer chain with cold proofs

- fork base 143; A side 2 blocks / 16 proofs; converged to A: False in 1800.5 s
- B height timeline: [(145, 0.0)]
- B CPU % mean 1; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during reorg | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 1801 | 1 | 2 | 2 | 2 | 0 | 0 |
| getrawmempool | 1801 | 1 | 2 | 2 | 3 | 0 | 0 |
| wallet.shieldedbalance | 1801 | 1 | 2 | 2 | 5 | 0 | 0 |
| getblocktemplate | 901 | 6 | 7 | 7 | 9 | 0 | 0 |

qualification failed: True
