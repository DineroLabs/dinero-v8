# Integrated-node load baseline: old (v1 Auth proofs)

Host: AMD EPYC 7763 64-Core Processor, 4 vCPU

## steady+service (W1+W4), old design at its own generation rate

- 300 s, achieved 0.030 shielded tx/s (9 ok, 0 failed), 9 blocks, 9 shielded tx confirmed (read back from block contents)
- daemon CPU % of one core mean 104 p95 196 (351 samples); RSS max 1463 MB; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| metric (ms) | n | p50 | p95 | p99 | max | missed ticks | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| wallet.shield build+submit | 4 | 16588 | 16896 | 16896 | 16896 | | | |
| wallet.transfer build+submit | 5 | 57616 | 57831 | 57831 | 57831 | | | |
| block generate incl. validation (own mempool, cached proofs) | 9 | 110 | 118 | 118 | 118 | | | |
| probe getblockcount every 1.0 s | 356 | 1 | 1 | 2 | 3 | 0 | 0 | 0 |
| probe getrawmempool every 1.0 s | 332 | 1 | 1 | 6202 | 6651 | 24 | 0 | 8 |
| probe wallet.shieldedbalance every 1.0 s | 69 | 1 | 25429 | 50745 | 51080 | 287 | 0 | 15 |
| probe getblocktemplate every 2.0 s | 173 | 6 | 87 | 5472 | 5733 | 5 | 0 stale-with-stable-tip 0 | 7 |

## hostile (W5): mutated copies of unmined seeds through testmempoolaccept, one client

- seeds (bytes): {'transfer_3proofs': 167903, 'shield_1proof': 46481}
- counts: {'transfer_3proofs/proof': {'decisions': 63, 'accepted': 0, 'rejected': 63, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'transfer_3proofs/ciphertext_control': {'decisions': 15, 'accepted': 15, 'rejected': 0, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'shield_1proof/proof': {'decisions': 63, 'accepted': 0, 'rejected': 63, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}, 'shield_1proof/ciphertext_control': {'decisions': 15, 'accepted': 15, 'rejected': 0, 'malformed': 0, 'busy503': 0, 'transport': 0, 'protocol': 0}}
- reject reasons: {'transfer_3proofs/proof': {'txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid': 63}, 'transfer_3proofs/ciphertext_control': {}, 'shield_1proof/proof': {'txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid': 63}, 'shield_1proof/ciphertext_control': {}}
- accepted by lane/field: {'transfer_3proofs/proof': {'spend0_zkproof': 0, 'out0_zkproof': 0, 'out1_zkproof': 0}, 'transfer_3proofs/ciphertext_control': {'out1_encrypted_note': 8, 'out0_encrypted_note': 7}, 'shield_1proof/proof': {'out0_zkproof': 0}, 'shield_1proof/ciphertext_control': {'out0_encrypted_note': 15}}
- **qualification failed (acceptance in a proof lane): False** (accepted examples: [{'i': 8, 'kind': 'transfer_3proofs/ciphertext_control', 'byte': 118017, 'field': 'out1_encrypted_note', 'reply': [{'allowed': True, 'txid': '8d7bc12591ba33d49304aed1f858228f72381f9bd8ce070a7eff1427cd47a34a'}]}, {'i': 9, 'kind': 'shield_1proof/ciphertext_control', 'byte': 361, 'field': 'out0_encrypted_note', 'reply': [{'allowed': True, 'txid': 'c23ef735c03c19021ec9ab73b3117dccabd99c14daf02fee8eaf5b1bfc6fe6ed'}]}, {'i': 18, 'kind': 'transfer_3proofs/ciphertext_control', 'byte': 73709, 'field': 'out0_encrypted_note', 'reply': [{'allowed': True, 'txid': 'ac3a669b19912c14712e282790b049ca294da0245b23aa2e0178523aecfb7a06'}]}])
- daemon CPU % of one core mean 100; log counters {'SAFE MODE': 0, 'INVARIANT VIOLATION': 0, 'corrupt': 0, 'REORG ABORT': 0, 'RPC server busy': 0}

| decision (ms) by seed and reject reason | n | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| transfer_3proofs/proof / txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid | 63 | 23 | 1975 | 2017 | 2036 |
| shield_1proof/proof / txn-validation-failed: Transaction validation failed: Shielded validation failed: proof-invalid | 63 | 13 | 1036 | 1060 | 1081 |

| probe during hostile | n | p50 | p95 | p99 | max | missed | errors | >1 s |
|---|---|---|---|---|---|---|---|---|
| getblockcount | 61 | 1 | 2 | 2 | 3 | 0 | 0 | 0 |
| getrawmempool | 60 | 405 | 1221 | 1951 | 2007 | 1 | 0 | 9 |
| wallet.shieldedbalance | 61 | 1 | 2 | 3 | 3 | 0 | 0 | 0 |
| getblocktemplate | 31 | 965 | 1845 | 1976 | 1976 | 0 | 0 | 15 |
