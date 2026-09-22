# Two-node baseline: old (v1 Auth proofs)

Host: Intel(R) Xeon(R) 6973P-C, 4 vCPU

## W2 catch-up: B validates blocks whose proofs it never saw

- blocks produced while B was offline: 2, proofs total 16 (per block: [8, 8])
- synced: True, time to tip from process spawn 29.1 s (RPC ready after 15.5 s; 1 block(s) validated before the first RPC sample); per-block gap s among observed: {'n': 1, 'p50': 13.557404099999985, 'p95': 13.557404099999985, 'p99': 13.557404099999985, 'max': 13.557404099999985, 'mean': 13.557404099999985}
- B CPU % of one core mean 92; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 23, 'RPC server busy': 0}; A {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during catch-up | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 14 | 1 | 1 | 1 | 1 | 0 | 0 |
| getrawmempool | 14 | 1 | 1 | 1 | 1 | 0 | 0 |
| wallet.shieldedbalance | 14 | 1 | 1 | 329 | 329 | 0 | 0 |
| getblocktemplate | 2 | 6 | 13343 | 13343 | 13343 | 5 | 1 |

## W3 reorg: B abandons its own 2 blocks for A's longer chain with cold proofs

- fork base 143; B side 1 block(s); A side 2 blocks / 16 proofs; converged to A: True in 27.8 s from process spawn (RPC ready after 27.8 s; converged before first RPC sample: True)
- B height timeline: [(145, 27.8)]
- B CPU % mean 0; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 23, 'RPC server busy': 0}

| B probe during reorg | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 1 | 1 | 1 | 1 | 1 | 0 | 0 |
| getrawmempool | 1 | 1 | 1 | 1 | 1 | 0 | 0 |
| wallet.shieldedbalance | 1 | 136 | 136 | 136 | 136 | 0 | 0 |
| getblocktemplate | 1 | 123 | 123 | 123 | 123 | 0 | 0 |

qualification failed: True reasons: ['W2 b log: REORG ABORT x23', 'W3 b log: REORG ABORT x23']
