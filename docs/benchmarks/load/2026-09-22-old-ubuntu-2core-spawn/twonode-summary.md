# Two-node baseline: old (v1 Auth proofs)

Host: AMD EPYC 7763 64-Core Processor, 4 vCPU

## W2 catch-up: B validates blocks whose proofs it never saw

- blocks produced while B was offline: 2, proofs total 16 (per block: [8, 8])
- synced: True, time to tip from process spawn 36.6 s (RPC ready after 19.3 s; 1 block(s) validated before the first RPC sample); per-block gap s among observed: {'n': 1, 'p50': 17.326820864999945, 'p95': 17.326820864999945, 'p99': 17.326820864999945, 'max': 17.326820864999945, 'mean': 17.326820864999945}
- B CPU % of one core mean 93; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}; A {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during catch-up | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 18 | 1 | 1 | 2 | 2 | 0 | 0 |
| getrawmempool | 18 | 1 | 1 | 2 | 2 | 0 | 0 |
| wallet.shieldedbalance | 18 | 1 | 1 | 2 | 2 | 0 | 0 |
| getblocktemplate | 2 | 6 | 17309 | 17309 | 17309 | 7 | 1 |

## W3 reorg: B abandons its own 2 blocks for A's longer chain with cold proofs

- fork base 143; B side 1 block(s); A side 2 blocks / 16 proofs; converged to A: True in 36.6 s from process spawn (RPC ready after 36.6 s; converged before first RPC sample: True)
- B height timeline: [(145, 36.6)]
- B CPU % mean 0; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during reorg | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 1 | 2 | 2 | 2 | 2 | 0 | 0 |
| getrawmempool | 1 | 2 | 2 | 2 | 2 | 0 | 0 |
| wallet.shieldedbalance | 1 | 1 | 1 | 1 | 1 | 0 | 0 |
| getblocktemplate | 1 | 7 | 7 | 7 | 7 | 0 | 0 |

qualification failed: False
