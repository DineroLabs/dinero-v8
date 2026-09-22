# Integrated-node load baseline: old (v1 Auth proofs)

Host: Intel(R) Xeon(R) 6973P-C, 4 vCPU

## steady+service (W1+W4), old design at its own generation rate

- 300 s, achieved 0.037 shielded tx/s (11 ok, 0 failed), 7 blocks, 11 shielded tx confirmed (read back from block contents)
- daemon CPU % of one core mean 102 p95 99 (320 samples); RSS max 1536 MB; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max | missed ticks | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| wallet.shield build+submit | 5 | 12366 | 12680 | 12680 | 12680 | | | |
| wallet.transfer build+submit | 6 | 43031 | 43280 | 43280 | 43280 | | | |
| block generate incl. validation (own mempool, cached proofs) | 7 | 217 | 498 | 498 | 498 | | | |
| probe getblockcount every 1.0 s | 322 | 1 | 2 | 2 | 3 | 0 | 0 | 0 |
| probe getrawmempool every 1.0 s | 299 | 1 | 2 | 5379 | 5635 | 23 | 0 | 11 |
| probe wallet.shieldedbalance every 1.0 s | 80 | 1 | 36304 | 36581 | 36641 | 242 | 0 | 16 |
| probe getblocktemplate every 2.0 s | 156 | 9 | 467 | 5458 | 5669 | 5 | 0 stale-with-stable-tip 0 | 8 |

## hostile (W5): mutated copies of unmined seeds through testmempoolaccept, one client

- seeds (bytes): {'transfer_3proofs': 167903, 'shield_1proof': 46481}
- counts: {'transfer_3proofs/proof': {'decisions': 69, 'accepted': 0, 'rejected': 69, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'transfer_3proofs/ciphertext_control': {'decisions': 17, 'accepted': 17, 'rejected': 0, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'shield_1proof/proof': {'decisions': 69, 'accepted': 0, 'rejected': 69, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'shield_1proof/ciphertext_control': {'decisions': 17, 'accepted': 17, 'rejected': 0, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}}
- reject reasons: {'transfer_3proofs/proof': {'txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid': 69}, 'transfer_3proofs/ciphertext_control': {}, 'shield_1proof/proof': {'txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid': 69}, 'shield_1proof/ciphertext_control': {}}
- accepted by lane/field: {'transfer_3proofs/proof': {'out0_zkproof': 0, 'spend0_zkproof': 0, 'out1_zkproof': 0}, 'transfer_3proofs/ciphertext_control': {'out0_encrypted_note': 8, 'out1_encrypted_note': 9}, 'shield_1proof/proof': {'out0_zkproof': 0}, 'shield_1proof/ciphertext_control': {'out0_encrypted_note': 17}}
- **qualification failed: False** reasons: [] (accepted examples: [{'i': 8, 'kind': 'transfer_3proofs/ciphertext_control', 'byte': 118017, 'field': 'out1_encrypted_note', 'reply': [{'allowed': True, 'txid': 'ad964fcb544daf2233bf7d5373b5fba2ce66260ca1a55b80d0662eccd8bc3dae'}]}, {'i': 9, 'kind': 'shield_1proof/ciphertext_control', 'byte': 361, 'field': 'out0_encrypted_note', 'reply': [{'allowed': True, 'txid': '2897ac81702f8a1d0af5de33a793955f9213a6e323a3625a0dc2f7b608fda647'}]}, {'i': 18, 'kind': 'transfer_3proofs/ciphertext_control', 'byte': 73709, 'field': 'out0_encrypted_note', 'reply': [{'allowed': True, 'txid': '086ed8ba0cbbc9a80a7eb77c276878071285d0f8008cb46546ff2e59f751f3cb'}]}])
- daemon CPU % of one core mean 99; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| decision (ms) by seed and reject reason | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| transfer_3proofs/proof / txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid | 69 | 16 | 1567 | 1701 | 2322 |
| shield_1proof/proof / txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid | 69 | 8 | 849 | 886 | 892 |

| probe during hostile | n | p50 | p95 | p99 | max | missed | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| getblockcount | 61 | 1 | 2 | 2 | 2 | 0 | 0 | 0 |
| getrawmempool | 61 | 477 | 1392 | 1452 | 1591 | 0 | 0 | 7 |
| wallet.shieldedbalance | 61 | 1 | 2 | 2 | 2 | 0 | 0 | 0 |
| getblocktemplate | 31 | 590 | 1759 | 1880 | 1880 | 0 | 0 | 9 |
