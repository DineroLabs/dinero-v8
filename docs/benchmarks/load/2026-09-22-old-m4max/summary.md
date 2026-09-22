# Integrated-node load baseline: old (v1 Auth proofs)

Host: M4 Max (macOS 27, 12-core), Release build-spike, commit 4ed198529

## steady+service (W1+W4)

- duration 480 s, blocks 18, tx builds 29, errors 0
- probe busy(503) 0, probe replies >1 s 74, stale template pairs 18
- daemon CPU % mean 36 p95 99; RSS MB max 911
- log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| wallet.shield build+submit | 12 | 7084 | 7208 | 7320 | 7320 |
| wallet.transfer build+submit | 17 | 24555 | 24803 | 24844 | 24844 |
| block generate (incl. validation) | 18 | 81 | 97 | 103 | 103 |
| block validation_ms (log) | 0 | | | | |
| probe getblockcount | 76 | 1 | 1 | 1 | 2 |
| probe getrawmempool | 76 | 0 | 0 | 1 | 1 |
| probe wallet.shieldedbalance | 76 | 3164 | 10811 | 10927 | 21740 |
| probe getblocktemplate | 58 | 74 | 3075 | 3077 | 3083 |

## hostile (W5)

- 734487 mutated proofs in 240 s; accepted 0; busy(503) 0
- probe busy(503) 0, probe replies >1 s 0; CPU % mean 5; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| invalid-proof decision (testmempoolaccept) | 734487 | 0 | 0 | 2 | 31 |
| probe getblockcount | 2 | 0 | 1 | 1 | 1 |
| probe getrawmempool | 2 | 0 | 0 | 0 | 0 |
| probe wallet.shieldedbalance | 1 | 0 | 0 | 0 | 0 |
| probe getblocktemplate | 0 | | | | |
