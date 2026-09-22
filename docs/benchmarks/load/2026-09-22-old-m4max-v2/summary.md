# Integrated-node load baseline: old (v1 Auth proofs)

Host: M4 Max (macOS 27, 12-core), Release build-spike, daemon 4ed198529, harness 32712d2e4+fix

## steady+service (W1+W4), old design at its own generation rate

- 480 s, achieved 0.060 shielded tx/s (29 ok, 0 failed), 18 blocks, 29 shielded tx confirmed (read back from block contents)
- daemon CPU % of one core mean 102 p95 108 (494 samples); RSS max 909 MB; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max | missed ticks | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| wallet.shield build+submit | 12 | 7046 | 7057 | 7252 | 7252 | | | |
| wallet.transfer build+submit | 17 | 24332 | 24409 | 24419 | 24419 | | | |
| block generate incl. validation (own mempool, cached proofs) | 18 | 78 | 95 | 98 | 98 | | | |
| probe getblockcount every 1.0 s | 500 | 1 | 1 | 1 | 3 | 0 | 0 | 0 |
| probe getrawmempool every 1.0 s | 482 | 1 | 300 | 2779 | 2999 | 18 | 0 | 17 |
| probe wallet.shieldedbalance every 1.0 s | 158 | 1 | 10666 | 21045 | 21285 | 342 | 0 | 54 |
| probe getblocktemplate every 2.0 s | 250 | 9 | 2023 | 2805 | 2893 | 0 | 0 stale-with-stable-tip 0 | 17 |

## hostile (W5): mutated copies of unmined seeds through testmempoolaccept, one client

- seeds (bytes): {'shield_1proof': 46481, 'transfer_3proofs': 167903}
- counts: {'shield_1proof': {'decisions': 1221, 'accepted': 0, 'rejected': 1221, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'transfer_3proofs': {'decisions': 1221, 'accepted': 14, 'rejected': 1207, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}}
- reject reasons: {'shield_1proof': {'txn-validation-failed: Transaction validation failed: Input UTXO not found: fa6974b852f7ccd1e9cb4dbc6a33f155f9f34332dde1': 1221}, 'transfer_3proofs': {'txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid': 1205, 'txn-validation-failed: Transaction validation failed: Shielded validation failed: range-proof-invalid': 1, 'exception: vector': 1}}
- **qualification failed: True** (accepted examples: [{'i': 229, 'kind': 'transfer_3proofs', 'reply': [{'allowed': True, 'txid': '184f625553bcfe044fa854616634a45860f26fd7b5d4ffdc34c8141537ab6e09'}]}, {'i': 263, 'kind': 'transfer_3proofs', 'reply': [{'allowed': True, 'txid': '559a0eccae7af3c14b08e9712d956efed6fdeddbf584dd37136d6e6bb6e3705d'}]}, {'i': 297, 'kind': 'transfer_3proofs', 'reply': [{'allowed': True, 'txid': '6c58f1e3ead13259e9d18709463c159cb5600a2bf55eccce0e90c33ab522eb8b'}]}, {'i': 625, 'kind': 'transfer_3proofs', 'reply': [{'allowed': True, 'txid': '4ac1b751b73530de4c45dc29fbc64ddfa9847d4c14cc1501c75413ff909af026'}]}, {'i': 659, 'kind': 'transfer_3proofs', 'reply': [{'allowed': True, 'txid': 'cf2919134a3b917aabfd5f69549e56ae904f8555a877def0ed1e40feb3f9403d'}]}])
- daemon CPU % of one core mean 100; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| decision (ms) by seed and reject reason | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| shield_1proof / txn-validation-failed: Transaction validation failed: Input UTXO not found: fa6974b852f7ccd1e9cb4dbc6a33f155f9f34332dde1 | 1221 | 2 | 2 | 2 | 5 |
| transfer_3proofs / txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid | 1205 | 11 | 932 | 942 | 1400 |
| transfer_3proofs / txn-validation-failed: Transaction validation failed: Shielded validation failed: range-proof-invalid | 1 | 8 | 8 | 8 | 8 |
| transfer_3proofs / exception: vector | 1 | 11 | 11 | 11 | 11 |

| probe during hostile | n | p50 | p95 | p99 | max | missed | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| getblockcount | 240 | 1 | 1 | 1 | 3 | 0 | 0 | 0 |
| getrawmempool | 241 | 351 | 907 | 1168 | 1372 | 0 | 0 | 5 |
| wallet.shieldedbalance | 240 | 1 | 1 | 1 | 5 | 0 | 0 | 0 |
| getblocktemplate | 120 | 366 | 834 | 922 | 1097 | 0 | 0 | 1 |
