# Two-node baseline: old (v1 Auth proofs)

Host: AMD EPYC 9V74 80-Core Processor, 4 vCPU

## W2 catch-up: B validates blocks whose proofs it never saw

- blocks produced while B was offline: 2, proofs total 16 (per block: [8, 8])
- synced: True, time to tip from process spawn 37.4 s (RPC ready after 19.8 s; 1 block(s) validated before the first RPC sample); per-block gap s among observed: {'n': 1, 'p50': 17.57504933899986, 'p95': 17.57504933899986, 'p99': 17.57504933899986, 'max': 17.57504933899986, 'mean': 17.57504933899986}
- B CPU % of one core mean 99; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}; A {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during catch-up | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 18 | 1 | 1 | 1 | 1 | 0 | 0 |
| getrawmempool | 18 | 1 | 1 | 1 | 1 | 0 | 0 |
| wallet.shieldedbalance | 18 | 1 | 1 | 3 | 3 | 0 | 0 |
| getblocktemplate | 2 | 6 | 17582 | 17582 | 17582 | 7 | 1 |

## W3 reorg: B abandons its own 2 blocks for A's longer chain with cold proofs

- fork base 143; B side 1 block(s); A side 2 blocks / 16 proofs; converged to A: True in 37.8 s from process spawn (RPC ready after 1.8 s; converged before first RPC sample: False)
- B height timeline: [(143, 1.8), (144, 19.8), (145, 37.6)]
- B CPU % mean 99; B log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| B probe during reorg | n ok | p50 | p95 | p99 | max | missed | > 1 s |
|---|---|---|---|---|---|---|---|
| getblockcount | 37 | 1 | 1 | 2 | 2 | 0 | 0 |
| getrawmempool | 37 | 1 | 1 | 2 | 2 | 0 | 0 |
| wallet.shieldedbalance | 37 | 1 | 1 | 3 | 3 | 0 | 0 |
| getblocktemplate | 3 | 6 | 35811 | 35811 | 35811 | 16 | 1 |

qualification failed: False reasons: []
