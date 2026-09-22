# Integrated-node load baseline: old (v1 Auth proofs)

Host: M4 Max (macOS 27, 12-core), Release build-spike, commit e59d606e1

## steady+service (W1+W4)

- duration 45 s, blocks 2, tx builds 5, errors 0
- probe busy(503) 0, probe replies >1 s 11, stale template pairs 2
- daemon CPU % mean 38 p95 98; RSS MB max 908
- log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| wallet.shield build+submit | 4 | 7099 | 7298 | 7298 | 7298 |
| wallet.transfer build+submit | 1 | 24454 | 24454 | 24454 | 24454 |
| block generate (incl. validation) | 2 | 75 | 91 | 91 | 91 |
| block validation_ms (log) | 0 | | | | |
| probe getblockcount | 13 | 1 | 1 | 1 | 1 |
| probe getrawmempool | 13 | 0 | 0 | 0 | 0 |
| probe wallet.shieldedbalance | 13 | 3114 | 10665 | 10685 | 10685 |
| probe getblocktemplate | 11 | 62 | 3104 | 3104 | 3104 |

## hostile (W5)

- seed 46481 B re-accepted by testmempoolaccept: [{'allowed': True, 'txid': '6eaf2bc9e90abc60efa8e6ba350cb436076a76776cdf584b15c64145b959fb04'}] in 4 ms (mempool cleared via mempool.clear)
- 1597 mutated proofs in 240 s; accepted 0; busy(503) 0; full verifications (>100 ms) 459
- probe busy(503) 0, probe replies >1 s 0; CPU % mean 0; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| invalid-proof decision, all | 1597 | 5 | 514 | 520 | 552 |
| invalid-proof decision, full verification (>100 ms) | 459 | 511 | 518 | 524 | 552 |
| invalid-proof decision, cheap reject (<=100 ms) | 1138 | 5 | 6 | 6 | 7 |
| probe getblockcount | 240 | 1 | 1 | 1 | 1 |
| probe getrawmempool | 240 | 251 | 483 | 505 | 512 |
| probe wallet.shieldedbalance | 240 | 1 | 1 | 1 | 1 |
| probe getblocktemplate | 120 | 6 | 514 | 517 | 517 |
