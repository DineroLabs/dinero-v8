# Security Audit Testing Procedures

**Version:** 1.0
**Date:** 2025-01-17
**Purpose:** Practical testing procedures for DineroCoin confidential transaction audit

---

## 1. Build and Setup

### 1.1 Build DineroCoin

```bash
# Clone repository
git clone https://github.com/dinerocoin/dinerocoin
cd dinerocoin

# Build Bulletproofs FFI
cd third_party/bulletproofs_ffi
cargo build --release
cargo test

# Build DineroCoin
cd ../..
./autogen.sh
./configure
make -j$(nproc)
make check
```

### 1.2 Verify Build

```bash
# Check Bulletproofs library
ls -lh third_party/bulletproofs_ffi/target/release/libbulletproofs_ffi.so

# Run tests
./src/test/test_dinerocoin
./test/functional/test_runner.py
```

---

## 2. Proof Generation Testing

### 2.1 Generate Test Proofs

```bash
# Build proof generator tool
cd tools
make generate_bulletproof

# Generate proof for value 1000
./generate_bulletproof --value 1000 --output test_proof_1000.bin

# Generate proof for max value
./generate_bulletproof --value 18446744073709551615 --output test_proof_max.bin

# Generate proof with nonce (rewindable)
./generate_bulletproof --value 5000000 \
    --nonce 0x0505050505050505050505050505050505050505050505050505050505050505 \
    --output test_proof_rewindable.bin
```

### 2.2 Verify Generated Proofs

```bash
# Verify proof
./verify_bulletproof --commitment <hex> --proof test_proof_1000.bin

# Should output: VALID

# Verify with wrong commitment
./verify_bulletproof --commitment <wrong_hex> --proof test_proof_1000.bin

# Should output: INVALID
```

### 2.3 Rewind Testing

```bash
# Rewind proof with correct nonce
./rewind_bulletproof \
    --commitment <hex> \
    --proof test_proof_rewindable.bin \
    --nonce 0x0505050505050505050505050505050505050505050505050505050505050505

# Should output:
# SUCCESS
# Value: 5000000
# Blinding: 0x...

# Rewind with wrong nonce
./rewind_bulletproof \
    --commitment <hex> \
    --proof test_proof_rewindable.bin \
    --nonce 0x0808080808080808080808080808080808080808080808080808080808080808

# Should output:
# NOT_OURS
```

---

## 3. Consensus Rule Testing

### 3.1 Test CON-01: Value Must Be Zero

```bash
# Create invalid TX (non-zero value in confidential output)
dinerocoin-cli createinvalidtx \
    --confidential true \
    --value 1000  # ❌ Should be 0

# Send to node
dinerocoin-cli sendrawtransaction <hex>

# Expected: Rejection with error "confidential-value-not-zero"
```

### 3.2 Test CON-04: Proof Size Limits

```bash
# Create proof that's too small
dd if=/dev/urandom of=small_proof.bin bs=1 count=100

# Create TX with small proof
dinerocoin-cli createconftx --proof small_proof.bin

# Expected: Rejection with error "invalid-proof-size"

# Create proof that's too large
dd if=/dev/urandom of=large_proof.bin bs=1 count=1000

# Create TX with large proof
dinerocoin-cli createconftx --proof large_proof.bin

# Expected: Rejection with error "invalid-proof-size"
```

### 3.3 Test CON-08: Max Outputs

```bash
# Create TX with 101 confidential outputs
dinerocoin-cli createconftx --outputs 101

# Expected: Rejection with error "too-many-confidential-outputs"

# Create TX with 100 outputs (should succeed)
dinerocoin-cli createconftx --outputs 100

# Expected: Accepted
```

### 3.4 Test CON-11: Commitment Balance (TODO)

```bash
# ⚠️ NOT IMPLEMENTED YET

# Create unbalanced TX
# Input:  commit(1000, r1)
# Output: commit(500, r2)
# Expected: Should reject (but currently doesn't)

# TODO: Test this after implementation
```

---

## 4. FFI Boundary Testing

### 4.1 Null Pointer Tests

```cpp
// Test null commitment pointer
int result = bp_verify(
    NULL,        // ❌ null pointer
    proof,
    proof_len
);

// Expected: result == -1 (error)
```

```bash
# Run FFI test suite
cd third_party/bulletproofs_ffi
cargo test test_null_pointers
```

### 4.2 Invalid Size Tests

```cpp
// Test oversized proof_len
uint8_t proof[800];
int result = bp_verify(
    commitment,
    proof,
    99999  // ❌ exceeds MAX_PROOF_SIZE
);

// Expected: result == -1 (error)
```

```bash
cargo test test_invalid_sizes
```

### 4.3 Malformed Data Tests

```cpp
// Test all-zero proof
uint8_t proof[700];
memset(proof, 0, 700);

int result = bp_verify(commitment, proof, 700);

// Expected: result == -1 or 0 (malformed or invalid)
```

```bash
cargo test test_malformed_proofs
```

---

## 5. DoS Attack Simulation

### 5.1 Rate Limiting Test

```bash
# Send 20 confidential TXs in 1 minute
for i in {1..20}; do
    dinerocoin-cli sendconftx --value 1000
    echo "Sent TX $i"
done

# Expected:
# - First 10 accepted
# - Next 10 rejected (rate limited)
```

### 5.2 Mempool Flood Test

```bash
# Generate 1000 large confidential TXs
for i in {1..1000}; do
    dinerocoin-cli createconftx --outputs 100 --size max > tx_$i.hex
done

# Send all to mempool
for i in {1..1000}; do
    dinerocoin-cli sendrawtransaction $(cat tx_$i.hex) &
done
wait

# Check mempool size
dinerocoin-cli getmempoolinfo

# Expected: Mempool size < 300 MB (limit enforced)
```

### 5.3 Peer Disconnection Test

```bash
# Setup: Connect to node
PEER_ID=$(dinerocoin-cli addnode <attacker_ip> add)

# Send 100 invalid confidential TXs
for i in {1..100}; do
    # Send TX with malformed proof
    dinerocoin-cli sendrawtransaction <invalid_tx_hex>
done

# Check peer status
dinerocoin-cli getpeerinfo | jq ".[] | select(.id == $PEER_ID)"

# Expected: Peer banned after ~10 invalid TXs
```

---

## 6. Wallet Scanning Testing

### 6.1 Identify Own Outputs

```bash
# Create confidential output to our address
ADDRESS=$(dinerocoin-cli getnewconfidentialaddress)

# Send funds
TXID=$(dinerocoin-cli sendtoaddress $ADDRESS 1.5)

# Wait for block
dinerocoin-cli generatetoaddress 1 $ADDRESS

# Scan blockchain
dinerocoin-cli rescanblockchain

# Check balance
BALANCE=$(dinerocoin-cli getbalance)

# Expected: Balance includes 1.5 BTC
echo "Balance: $BALANCE"
```

### 6.2 Ignore Others' Outputs

```bash
# Create output to different address
OTHER_ADDRESS="dC8h3kP9mN..."  # Not ours

# Send funds
TXID=$(dinerocoin-cli sendtoaddress $OTHER_ADDRESS 2.0)

# Generate block
dinerocoin-cli generatetoaddress 1 $OUR_ADDRESS

# Rescan
dinerocoin-cli rescanblockchain

# Check balance (should NOT include 2.0)
BALANCE=$(dinerocoin-cli getbalance)

# Expected: Balance unchanged
```

### 6.3 Corrupted Data Handling

```bash
# Create block with corrupted confidential output
# (manually craft invalid output)

# Import block
dinerocoin-cli submitblock <corrupted_block_hex>

# Rescan
dinerocoin-cli rescanblockchain

# Expected:
# - No crash
# - Warning logged
# - Corrupted output skipped
```

---

## 7. Reorg Testing

### 7.1 Simple Reorg

```bash
# Setup: Mine on chain A
dinerocoin-cli generatetoaddress 10 $ADDRESS

# Create TX
TXID=$(dinerocoin-cli sendconftx --value 1000000)

# Mine 5 more blocks (TX confirmed)
dinerocoin-cli generatetoaddress 5 $ADDRESS

# Check TX confirmed
dinerocoin-cli gettransaction $TXID | jq .confirmations
# Output: 5

# Simulate reorg: invalidate last 6 blocks
BLOCKHASH=$(dinerocoin-cli getblockhash <height-5>)
dinerocoin-cli invalidateblock $BLOCKHASH

# Check TX status (should be in mempool again)
dinerocoin-cli getmempoolinfo

# Rescan wallet
dinerocoin-cli rescanblockchain

# Expected:
# - TX moved to mempool
# - Wallet balance updated
# - No crashes
```

---

## 8. RPC Security Testing

### 8.1 Information Leak Detection

```bash
# Call gettransaction on confidential TX
RESPONSE=$(dinerocoin-cli gettransaction $TXID)

# Check for sensitive fields
echo $RESPONSE | grep -i "blinding"
# Expected: No match (blinding factors filtered)

echo $RESPONSE | grep -i "view.*key"
# Expected: No match (view keys filtered)

echo $RESPONSE | grep -i "ephemeral.*priv"
# Expected: No match (ephemeral private keys filtered)
```

### 8.2 RPC Rate Limiting

```bash
# Send 100 RPC requests rapidly
for i in {1..100}; do
    dinerocoin-cli getconfidentialbalance &
done
wait

# Expected: Some requests rejected (rate limited)
```

---

## 9. Cryptographic Testing

### 9.1 Commitment Verification

```python
#!/usr/bin/env python3
import subprocess
import json

# Create commitment
value = 1000000
blinding = "0x" + "01" * 32

# Generate commitment
result = subprocess.run([
    "./tools/create_commitment",
    "--value", str(value),
    "--blinding", blinding
], capture_output=True, text=True)

commitment = result.stdout.strip()
print(f"Commitment: {commitment}")

# Verify commitment matches expected
# C = value * H + blinding * G

# Generate proof
subprocess.run([
    "./tools/generate_bulletproof",
    "--value", str(value),
    "--blinding", blinding,
    "--output", "test.bin"
])

# Verify proof
result = subprocess.run([
    "./tools/verify_bulletproof",
    "--commitment", commitment,
    "--proof", "test.bin"
], capture_output=True, text=True)

assert "VALID" in result.stdout, "Proof verification failed!"
print("✅ Commitment and proof valid")
```

### 9.2 ECDH Consistency

```python
#!/usr/bin/env python3

# Generate ephemeral keypair
ephemeral_priv = generate_random(32)
ephemeral_pub = secp256k1_create_pubkey(ephemeral_priv)

# Recipient view key
view_priv = "0x" + "02" * 32
view_pub = secp256k1_create_pubkey(view_priv)

# Sender computes ECDH
nonce_sender = ecdh(ephemeral_priv, view_pub)

# Recipient computes ECDH
nonce_recipient = ecdh(view_priv, ephemeral_pub)

# Verify they match
assert nonce_sender == nonce_recipient, "ECDH mismatch!"
print("✅ ECDH nonces match")
```

---

## 10. Fuzzing

### 10.1 FFI Fuzzing Setup

```bash
cd third_party/bulletproofs_ffi

# Install cargo-fuzz
cargo install cargo-fuzz

# Initialize fuzzing targets
cargo fuzz init

# Create fuzz target for bp_verify
cat > fuzz/fuzz_targets/fuzz_verify.rs <<EOF
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if data.len() < 33 + 650 {
        return;
    }

    let commitment = &data[0..33];
    let proof = &data[33..];

    // Should not crash
    unsafe {
        bulletproofs_ffi::bp_verify(
            commitment.as_ptr(),
            proof.as_ptr(),
            proof.len()
        );
    }
});
EOF

# Run fuzzer
cargo fuzz run fuzz_verify -- -max_total_time=3600  # 1 hour
```

### 10.2 Review Fuzzing Results

```bash
# Check for crashes
ls fuzz/artifacts/fuzz_verify/

# If crashes found:
for crash in fuzz/artifacts/fuzz_verify/crash-*; do
    echo "Analyzing: $crash"
    xxd $crash | head -20
done
```

---

## 11. Performance Testing

### 11.1 Proof Generation Benchmark

```bash
# Benchmark proof generation
time for i in {1..100}; do
    ./generate_bulletproof --value $((RANDOM * 1000)) --output /dev/null
done

# Expected: ~10 seconds (100 ms per proof)
```

### 11.2 Proof Verification Benchmark

```bash
# Generate 100 proofs
for i in {1..100}; do
    ./generate_bulletproof --value $i --output proof_$i.bin
done

# Benchmark individual verification
time for i in {1..100}; do
    ./verify_bulletproof --commitment <hex> --proof proof_$i.bin >/dev/null
done

# Expected: ~10 seconds (100 ms per proof)

# Benchmark batch verification
time ./verify_bulletproof_batch proof_*.bin

# Expected: ~3-5 seconds (2-3x speedup)
```

### 11.3 Wallet Scan Benchmark

```bash
# Create block with 1000 outputs (10 are ours)
./create_test_block --outputs 1000 --ours 10 > test_block.hex

# Benchmark scan
time dinerocoin-cli submitblock $(cat test_block.hex)
time dinerocoin-cli rescanblockchain

# Expected: < 2 seconds for 1000 outputs
```

---

## 12. Test Vector Validation

### 12.1 Load Test Vectors

```bash
cd test_vectors

# Parse test vectors
python3 <<EOF
import json

with open('bulletproof_proofs_hex.json') as f:
    vectors = json.load(f)

for test in vectors['test_vectors']:
    print(f"Testing: {test['name']}")

    # Extract data
    commitment = test['commitment']
    proof = test['proof_hex']
    expected = test['verification_result']

    # TODO: Verify each test vector
    # (Requires implementing actual proof generation)
EOF
```

---

## 13. Reporting

### 13.1 Test Results Template

```markdown
# Test Results

**Date:** 2025-01-17
**Auditor:** [Name]
**Duration:** [Hours]

## Summary

- Total tests run: X
- Passed: Y
- Failed: Z
- Critical issues: N

## Detailed Results

### Consensus Rules
- CON-01: ✅ PASS
- CON-02: ✅ PASS
- ...
- CON-11: ⚠️ NOT IMPLEMENTED

### FFI Boundary
- Null pointer handling: ✅ PASS
- Size validation: ✅ PASS
- Panic safety: ✅ PASS

### DoS Resistance
- Rate limiting: ✅ PASS
- Mempool flood: ✅ PASS

### Issues Found

1. **[CRITICAL] CON-11 not implemented**
   - File: src/consensus/confidential_validation.cpp:189
   - Impact: Value inflation possible
   - Recommendation: Implement before mainnet

2. **[HIGH] Wallet scanning timing leak**
   - File: wallet_scanner.h
   - Impact: Privacy leak
   - Recommendation: Implement constant-time scanning

...

## Recommendations

[List recommendations here]

## Sign-Off

Auditor: _______________
Date: _______________
```

---

**End of Testing Procedures**
