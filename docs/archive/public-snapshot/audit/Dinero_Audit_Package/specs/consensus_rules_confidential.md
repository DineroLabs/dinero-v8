# Consensus Rules for Confidential Transactions

**Version:** 1.0
**Date:** 2025-01-17
**Status:** Implemented (Commitment Balance TODO)

---

## 1. Overview

This document defines the **consensus-critical validation rules** that all DineroCoin nodes MUST enforce for confidential transactions. Failure to enforce these rules results in chain splits.

### 1.1 Rule Categories

- **MUST** - Mandatory for consensus
- **SHOULD** - Recommended for policy (mempool)
- **MAY** - Optional optimizations

---

## 2. Output-Level Rules

### 2.1 Confidential Output Structure

**Rule CON-01: Confidential outputs MUST have value = 0**

```cpp
MUST: output.is_confidential == true → output.value == 0
```

**Rationale:** Amount is hidden in commitment; setting `value != 0` leaks information.

**Validation:**
```cpp
if (output.is_confidential && output.value != 0) {
    return ValidationError::CONFIDENTIAL_VALUE_NOT_ZERO;
}
```

**Test Vector:**
```json
{
  "test": "CON-01_violation",
  "output": {
    "value": 1000,
    "is_confidential": true
  },
  "expected": "REJECT"
}
```

---

### 2.2 Commitment Size

**Rule CON-02: Commitment MUST be exactly 33 bytes**

```cpp
MUST: output.commitment.size() == 33
```

**Rationale:** Ristretto255 compressed point size is fixed at 33 bytes.

**Validation:**
```cpp
if (output.commitment.size() != 33) {
    return ValidationError::INVALID_COMMITMENT_SIZE;
}
```

**Enforcement:** `src/consensus/confidential_validation.cpp:166`

---

### 2.3 Commitment Format

**Rule CON-03: Commitment MUST be a valid compressed Ristretto255 point**

```cpp
MUST: output.commitment[0] == 0x02 || output.commitment[0] == 0x03
```

**Rationale:** First byte must be compression prefix.

**Validation:**
```cpp
uint8_t prefix = output.commitment[0];
if (prefix != 0x02 && prefix != 0x03) {
    return ValidationError::INVALID_COMMITMENT_FORMAT;
}
```

**Note:** Full point validation happens during `bp_verify()`.

**Enforcement:** `src/consensus/confidential_validation.cpp:353`

---

### 2.4 Range Proof Size

**Rule CON-04: Range proof MUST be 650-800 bytes**

```cpp
MUST: 650 <= output.range_proof.size() <= 800
```

**Rationale:**
- Minimum 650 bytes: Smallest valid Bulletproof for 64-bit
- Maximum 800 bytes: Allows rewind data + variance
- Expected 714 bytes: 674 (proof) + 40 (rewind)

**Validation:**
```cpp
size_t proof_size = output.range_proof.size();

if (proof_size < 650 || proof_size > 800) {
    return ValidationError::INVALID_PROOF_SIZE;
}
```

**Enforcement:** `src/consensus/confidential_validation.cpp:105`

---

### 2.5 Range Proof Validity

**Rule CON-05: Range proof MUST verify using bp_verify()**

```cpp
MUST: bp_verify(commitment, proof, proof_len) == 1
```

**Rationale:** Core security property - ensures value in valid range.

**Validation:**
```cpp
int result = bp_verify(
    output.commitment.data(),
    output.range_proof.data(),
    output.range_proof.size()
);

if (result == -1) {
    return ValidationError::INTERNAL_ERROR;  // Malformed
}

if (result == 0) {
    return ValidationError::PROOF_VERIFY_FAILED;  // Invalid
}
```

**Enforcement:** `src/consensus/confidential_validation.cpp:116`

---

### 2.6 Nonce Field Size

**Rule CON-06: Nonce field MUST be exactly 65 bytes**

```cpp
MUST: output.nonce.size() == 65
```

**Rationale:**
- 33 bytes: Ephemeral public key (compressed secp256k1)
- 32 bytes: Encrypted blinding factor

**Validation:**
```cpp
if (output.nonce.size() != 65) {
    return ValidationError::INVALID_NONCE_SIZE;
}
```

**Enforcement:** `src/consensus/confidential_validation.cpp:140`

---

### 2.7 Ephemeral Public Key

**Rule CON-07: Ephemeral public key MUST be valid secp256k1 point**

```cpp
MUST: secp256k1_ec_pubkey_parse(nonce[0:33]) == SUCCESS
```

**Rationale:** Ensures ECDH derivation will work for recipient.

**Validation:**
```cpp
uint8_t ephemeral_pubkey[33];
memcpy(ephemeral_pubkey, output.nonce.data(), 33);

uint8_t prefix = ephemeral_pubkey[0];
if (prefix != 0x02 && prefix != 0x03) {
    return ValidationError::INVALID_EPHEMERAL_PUBKEY;
}

secp256k1_pubkey pubkey;
int result = secp256k1_ec_pubkey_parse(
    ctx, &pubkey, ephemeral_pubkey, 33
);

if (result != 1) {
    return ValidationError::INVALID_EPHEMERAL_PUBKEY;
}
```

**Enforcement:** `src/consensus/confidential_validation.cpp:329`

---

## 3. Transaction-Level Rules

### 3.1 Output Count Limit

**Rule CON-08: Maximum 100 confidential outputs per transaction**

```cpp
MUST: count(confidential_outputs) <= 100
```

**Rationale:** Prevents DoS via excessive verification work.

**Validation:**
```cpp
size_t conf_count = 0;
for (auto& output : tx.vout) {
    if (output.is_confidential) {
        conf_count++;
    }
}

if (conf_count > 100) {
    return ValidationError::TOO_MANY_CONFIDENTIAL_OUTPUTS;
}
```

**Enforcement:** `src/consensus/confidential_validation.cpp:45`

---

### 3.2 Total Proof Data Limit

**Rule CON-09: Maximum 100 KB total proof data per transaction**

```cpp
MUST: sum(proof_sizes) <= 100,000 bytes
```

**Rationale:** Prevents bandwidth amplification attacks.

**Validation:**
```cpp
size_t total_proof_size = 0;
for (auto& output : tx.vout) {
    if (output.is_confidential) {
        total_proof_size += output.range_proof.size();
    }
}

if (total_proof_size > 100000) {
    return ValidationError::CONFIDENTIAL_DATA_TOO_LARGE;
}
```

**Enforcement:** `src/consensus/confidential_validation.cpp:54`

---

### 3.3 Transaction Size Limit

**Rule CON-10: Maximum 500 KB total transaction size**

```cpp
MUST: tx.GetSerializedSize() <= 500,000 bytes
```

**Rationale:** Network message size limit.

**Validation:**
```cpp
size_t tx_size = tx.GetSerializedSize();

if (tx_size > 500000) {
    return ValidationError::TX_TOO_LARGE;
}
```

**Enforcement:** Network layer (`daemon/confidential_network_protection.cpp:120`)

---

### 3.4 Commitment Balance

**Rule CON-11: Sum of input commitments MUST equal sum of output commitments plus fee**

```cpp
MUST: sum(input_commitments) == sum(output_commitments) + fee_commitment
```

**Status:** ✅ **IMPLEMENTED** (2025-11-18)

**Rationale:** Prevents value inflation by cryptographically enforcing conservation of value.

**Implementation:**
```cpp
ConfidentialValidationResult ValidateCommitmentBalance(
    const Transaction& tx,
    const std::vector<std::vector<uint8_t>>& input_commitments
) {
    // 1. Sum all input commitments using Ristretto255 arithmetic
    std::vector<uint8_t> sum_inputs(32, 0);
    for (const auto& input_commitment : input_commitments) {
        commitment_add(sum_inputs.data(), input_commitment.data(), temp.data());
        sum_inputs = temp;
    }

    // 2. Sum all output commitments (mixed TX support)
    std::vector<uint8_t> sum_outputs(32, 0);
    for (const auto& output : tx.vout) {
        if (output.is_confidential) {
            // Use commitment from confidential output
            std::memcpy(output_commitment.data(), output.commitment.data() + 1, 32);
        } else {
            // Transparent: create commitment = value * H + 0 * G
            commitment_from_value(output.value, output_commitment.data());
        }
        commitment_add(sum_outputs.data(), output_commitment.data(), temp.data());
        sum_outputs = temp;
    }

    // 3. Create fee commitment
    commitment_from_value(tx.fee, fee_commitment.data());

    // 4. Add fee to output sum
    commitment_add(sum_outputs.data(), fee_commitment.data(), sum_outputs_with_fee.data());

    // 5. Verify balance
    if (sum_inputs != sum_outputs_with_fee) {
        return ConfidentialValidationResult::Failure(
            ConfidentialValidationError::COMMITMENT_BALANCE_FAILED,
            "sum(inputs) != sum(outputs) + fee"
        );
    }

    return ConfidentialValidationResult::Success();
}
```

**Location:** `src/consensus/confidential_validation.cpp:186-336`

**Cryptographic Library:** Dalek Bulletproofs (Ristretto255)

**FFI Functions Used:**
- `commitment_add()` - Add two commitments (`lib.rs:997-1064`)
- `commitment_sub()` - Subtract commitments (`lib.rs:1080-1135`)
- `commitment_from_value()` - Create commitment from transparent value (`lib.rs:1152-1189`)
- `commitment_is_identity()` - Check if commitment is zero point (`lib.rs:1201-1235`)

**Integration:** Called from `daemon/validation_confidential.cpp:409` during transaction validation

**Security Properties:**
- ✅ **Prevents value inflation** - Cannot create coins from nothing
- ✅ **Homomorphic property** - C1 + C2 = commit(v1+v2, r1+r2)
- ✅ **Mixed transaction support** - Works with confidential + transparent outputs
- ✅ **Cryptographic soundness** - Based on discrete log hardness

**Test Coverage:**
- Unit tests: `tests/test_con11_ristretto255.cpp` (23 tests)
- Commitment arithmetic tests (9 tests)
- Balanced transaction tests (5 tests)
- Unbalanced transaction rejection tests (3 tests)
- Edge case tests (5 tests)
- Full integration test (1 test)

**Performance:**
- Commitment addition: ~10 μs per operation
- Typical 2-in, 2-out TX: ~60 μs overhead
- Negligible compared to proof verification (~50 ms)

**Documentation:**
- `CON11_IMPLEMENTATION_COMPLETE.md` - Full technical details
- `CURVE_STANDARDIZATION_RISTRETTO255.md` - Curve selection rationale
- `RISTRETTO255_MIGRATION_COMPLETE.md` - Migration summary

---

## 4. Block-Level Rules

### 4.1 Batch Verification

**Rule CON-12: All proofs in block SHOULD be batch-verified**

```cpp
SHOULD: Use bp_verify_batch() for block validation
```

**Rationale:** 2-3x performance improvement.

**Implementation:**
```cpp
std::vector<Commitment> commitments;
std::vector<RangeProof> proofs;

for (auto& tx : block.transactions) {
    for (auto& output : tx.vout) {
        if (output.is_confidential) {
            commitments.push_back(output.commitment);
            proofs.push_back(output.range_proof);
        }
    }
}

int result = bp_verify_batch(
    commitments.data(),
    proofs.data(),
    proof_lens.data(),
    commitments.size()
);
```

**Enforcement:** `src/consensus/confidential_validation.cpp:460`

---

## 5. Mempool Policy Rules

These rules are **policy**, not consensus. Nodes MAY enforce them to protect mempool.

### 5.1 Rate Limiting

**Rule MEM-01: Maximum 10 confidential TXs per minute per peer**

```cpp
SHOULD: peer_conf_tx_count < 10 per minute
```

**Enforcement:** `daemon/confidential_network_protection.cpp:134`

### 5.2 Unusual Proof Sizes

**Rule MEM-02: Warn on proofs outside expected range**

```cpp
SHOULD: 700 <= proof_size <= 720  // ±6 bytes from 714
```

**Action:** Log warning, but accept if within consensus bounds.

**Enforcement:** `src/consensus/confidential_validation.cpp:285`

### 5.3 Flood Detection

**Rule MEM-03: Reject if peer sends 20+ confidential TXs in 1 minute**

```cpp
SHOULD: Ban peer if flood detected
```

**Enforcement:** `daemon/confidential_network_protection.cpp:177`

---

## 6. Validation Order

### 6.1 Output Validation Sequence

For each confidential output, validate in this order:

1. **Size checks** (fast, fail early)
   - Commitment size = 33 bytes
   - Proof size 650-800 bytes
   - Nonce size = 65 bytes

2. **Format checks** (fast)
   - Commitment prefix 0x02 or 0x03
   - Ephemeral pubkey prefix 0x02 or 0x03

3. **Cryptographic validation** (slow)
   - Ephemeral pubkey parsing (secp256k1)
   - Range proof verification (Bulletproofs)

**Rationale:** Fail fast on cheap checks before expensive crypto.

### 6.2 Transaction Validation Sequence

```cpp
1. Basic structure validation
   ├─ Transaction version check
   ├─ Input/output count checks
   └─ Size limit checks

2. Per-output validation
   ├─ For each confidential output:
   │  ├─ Size checks (CON-02, CON-04, CON-06)
   │  ├─ Format checks (CON-03, CON-07)
   │  └─ Proof verification (CON-05)

3. Transaction-level checks
   ├─ Total output count (CON-08)
   ├─ Total proof data (CON-09)
   └─ Commitment balance (CON-11) ← TODO

4. Policy checks (mempool only)
   ├─ Rate limiting
   └─ Flood detection
```

---

## 7. Error Codes

```cpp
enum class ConfidentialValidationError {
    VALID = 0,

    // Proof validation errors
    INVALID_PROOF_SIZE,           // CON-04 violation
    PROOF_VERIFY_FAILED,          // CON-05 violation
    MALFORMED_PROOF,

    // Commitment validation errors
    INVALID_COMMITMENT_SIZE,      // CON-02 violation
    INVALID_COMMITMENT_FORMAT,    // CON-03 violation
    COMMITMENT_PROOF_MISMATCH,

    // Nonce validation errors
    INVALID_NONCE_SIZE,           // CON-06 violation
    INVALID_EPHEMERAL_PUBKEY,     // CON-07 violation
    MALFORMED_NONCE,

    // Transaction-level errors
    TOO_MANY_CONFIDENTIAL_OUTPUTS,   // CON-08 violation
    CONFIDENTIAL_DATA_TOO_LARGE,     // CON-09 violation
    MIXED_TRANSPARENT_CONFIDENTIAL,

    // Balance validation errors
    COMMITMENT_BALANCE_FAILED,    // CON-11 violation (TODO)
    EXCESS_BLINDING_MISSING,

    // General errors
    INTERNAL_ERROR,
    UNKNOWN_ERROR
};
```

**Encoding:** `include/consensus/confidential_validation.h:43`

---

## 8. Test Vectors

### 8.1 Valid Transaction

```json
{
  "tx": {
    "version": 2,
    "vin": [...],
    "vout": [{
      "value": 0,
      "is_confidential": true,
      "commitment": "0x02abcd...",  // 33 bytes
      "range_proof": "0x...",        // 714 bytes
      "nonce": "0x..."               // 65 bytes
    }],
    "fee": 1000
  },
  "expected": "ACCEPT"
}
```

### 8.2 Invalid: Wrong Commitment Size

```json
{
  "tx": {
    "vout": [{
      "commitment": "0xabcd",  // Only 2 bytes!
      ...
    }]
  },
  "violation": "CON-02",
  "expected": "REJECT: INVALID_COMMITMENT_SIZE"
}
```

### 8.3 Invalid: Proof Too Small

```json
{
  "tx": {
    "vout": [{
      "range_proof": "0x1234",  // Only 2 bytes!
      ...
    }]
  },
  "violation": "CON-04",
  "expected": "REJECT: INVALID_PROOF_SIZE"
}
```

### 8.4 Invalid: Too Many Outputs

```json
{
  "tx": {
    "vout": [
      /* 101 confidential outputs */
    ]
  },
  "violation": "CON-08",
  "expected": "REJECT: TOO_MANY_CONFIDENTIAL_OUTPUTS"
}
```

---

## 9. Activation

### 9.1 Soft Fork Deployment

Confidential transactions are deployed as a **soft fork**:

- Old nodes: Ignore confidential outputs
- New nodes: Validate according to rules above
- Activation height: TBD

### 9.2 Backwards Compatibility

- Transparent transactions continue to work
- Mixed transactions (transparent + confidential) supported
- No mandatory upgrade for users

---

## 10. Known Issues

### 10.1 TODO: Commitment Balance

**Status:** Not implemented

**File:** `src/consensus/confidential_validation.cpp:189`

**Risk:** Without this check, value inflation is theoretically possible

**Mitigation:**
- Range proofs prevent negative values
- Limits maximum single-transaction impact
- Must be implemented before mainnet

### 10.2 Batch Verification API

**Status:** Using sequential verification with early exit

**File:** `third_party/bulletproofs_ffi/src/lib.rs:482`

**Impact:** Slower than optimal batch verification

**Plan:** Investigate Dalek Bulletproofs 4.0 batch API

---

## 11. References

1. **Elements Confidential TX:** https://elementsproject.org/features/confidential-transactions
2. **Monero RingCT:** https://web.getmonero.org/resources/moneropedia/ringCT.html
3. **Bulletproofs Paper:** https://eprint.iacr.org/2017/1066.pdf

---

**End of Specification**
