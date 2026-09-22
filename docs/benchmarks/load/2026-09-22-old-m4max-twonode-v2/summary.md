# Two-node baseline: old (v1 Auth proofs)

Host: M4 Max (macOS 27, 12-core), Release build-spike, daemon 4ed198529, harness 7f28da4ae

## W2 catch-up: B validates blocks whose proofs it never saw

- blocks produced while B was offline: 3, proofs total 24 (per block: [8, 8, 8])
- synced: True, time to tip from process spawn 24.7 s (RPC ready after 0.8 s; 0 block(s) validated before the first RPC sample); per-block gap s among observed: {'n': 2, 'p50': 7.903601332975086, 'p95': 7.905311125039589, 'p99': 7.905311125039589, 'max': 7.905311125039589, 'mean': 7.904456229007337}
- B CPU % of one core mean 99; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}; A {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during catch-up | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 25 | 1 | 1 | 1 | 1 | 0 | 0 |
| getrawmempool | 25 | 1 | 1 | 3 | 3 | 0 | 0 |
| wallet.shieldedbalance | 25 | 1 | 1 | 3 | 3 | 0 | 0 |
| getblocktemplate | 7 | 4 | 8083 | 8083 | 8083 | 6 | 3 |

## W3 reorg: B abandons its own 2 blocks for A's longer chain with cold proofs

- fork base 144; B side 2 block(s); A side 3 blocks / 24 proofs; converged to A: True in 24.5 s from process spawn (RPC ready after 0.8 s; converged before first RPC sample: False)
- B height timeline: [(146, 0.8), (144, 1.0), (145, 8.9), (146, 16.8), (147, 24.5)]
- B CPU % mean 99; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during reorg | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 24 | 1 | 1 | 1 | 1 | 0 | 0 |
| getrawmempool | 24 | 1 | 1 | 2 | 2 | 0 | 0 |
| wallet.shieldedbalance | 24 | 1 | 1 | 3 | 3 | 0 | 0 |
| getblocktemplate | 3 | 47 | 21676 | 21676 | 21676 | 9 | 1 |

qualification failed: False
