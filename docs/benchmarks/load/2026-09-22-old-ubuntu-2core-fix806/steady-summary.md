# Integrated-node load baseline: old (v1 Auth proofs)

Host: AMD EPYC 9V74 80-Core Processor, 4 vCPU

## steady+service (W1+W4), old design at its own generation rate

- 300 s, achieved 0.027 shielded tx/s (8 ok, 0 failed), 8 blocks, 8 shielded tx confirmed (read back from block contents)
- daemon CPU % of one core mean 104 p95 196 (303 samples); RSS max 1148 MB; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max | missed ticks | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| wallet.shield build+submit | 4 | 17111 | 17488 | 17488 | 17488 | | | |
| wallet.transfer build+submit | 4 | 59452 | 59698 | 59698 | 59698 | | | |
| block generate incl. validation (own mempool, cached proofs) | 8 | 117 | 132 | 132 | 132 | | | |
| probe getblockcount every 1.0 s | 308 | 1 | 1 | 2 | 4 | 0 | 0 | 0 |
| probe getrawmempool every 1.0 s | 288 | 1 | 1 | 6080 | 6773 | 20 | 0 | 8 |
| probe wallet.shieldedbalance every 1.0 s | 61 | 1 | 51968 | 52263 | 52473 | 247 | 0 | 12 |
| probe getblocktemplate every 2.0 s | 147 | 7 | 647 | 6406 | 6821 | 7 | 0 stale-with-stable-tip 0 | 7 |

## hostile (W5): mutated copies of unmined seeds through testmempoolaccept, one client

- seeds (bytes): {'transfer_3proofs': 167903, 'shield_1proof': 46481}
- counts: {'transfer_3proofs/proof': {'decisions': 69, 'accepted': 0, 'rejected': 69, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'transfer_3proofs/ciphertext_control': {'decisions': 17, 'accepted': 17, 'rejected': 0, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'shield_1proof/proof': {'decisions': 68, 'accepted': 0, 'rejected': 68, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'shield_1proof/ciphertext_control': {'decisions': 17, 'accepted': 17, 'rejected': 0, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}}
- reject reasons: {'transfer_3proofs/proof': {'txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid': 69}, 'transfer_3proofs/ciphertext_control': {}, 'shield_1proof/proof': {'txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid': 68}, 'shield_1proof/ciphertext_control': {}}
- accepted by lane/field: {'transfer_3proofs/proof': {'out1_zkproof': 0, 'out0_zkproof': 0, 'spend0_zkproof': 0}, 'transfer_3proofs/ciphertext_control': {'out0_encrypted_note': 8, 'out1_encrypted_note': 9}, 'shield_1proof/proof': {'out0_zkproof': 0}, 'shield_1proof/ciphertext_control': {'out0_encrypted_note': 17}}
- **qualification failed: False** reasons: [] (accepted examples: [{'i': 8, 'kind': 'transfer_3proofs/ciphertext_control', 'byte': 118017, 'field': 'out1_encrypted_note', 'reply': [{'allowed': True, 'txid': '0c49baeda10a48735e7beeb4041d3d7b43c7a73902ea49e7dcfe027091d3a448'}]}, {'i': 9, 'kind': 'shield_1proof/ciphertext_control', 'byte': 361, 'field': 'out0_encrypted_note', 'reply': [{'allowed': True, 'txid': 'df78843efa23a48c9a4768d32585c9a5e4777f13d1ce110777f7205aae4f272b'}]}, {'i': 18, 'kind': 'transfer_3proofs/ciphertext_control', 'byte': 73709, 'field': 'out0_encrypted_note', 'reply': [{'allowed': True, 'txid': '44d65b5adfee2abd4f0ada7fe3240c346426e14eba823c5acef630cc19c25e4c'}]}])
- daemon CPU % of one core mean 100; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| decision (ms) by seed and reject reason | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| transfer_3proofs/proof / txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid | 69 | 24 | 2050 | 2117 | 3240 |
| shield_1proof/proof / txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid | 68 | 12 | 1093 | 1122 | 1140 |

| probe during hostile | n | p50 | p95 | p99 | max | missed | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| getblockcount | 62 | 1 | 2 | 2 | 3 | 0 | 0 | 0 |
| getrawmempool | 59 | 427 | 1772 | 1924 | 3174 | 2 | 0 | 12 |
| wallet.shieldedbalance | 62 | 1 | 2 | 2 | 3 | 0 | 0 | 0 |
| getblocktemplate | 31 | 564 | 1938 | 2870 | 2870 | 0 | 0 | 11 |
