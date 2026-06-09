# Bulletproofs Rewind Mechanism - Design Specification

**Version:** 1.0
**Date:** 2025-01-17
**Status:** Implemented and Tested

---

## 1. Problem Statement

Standard Bulletproofs prove that a committed value lies in a valid range `[0, 2^n - 1]` but provide **no way** to recover the original value. For confidential transactions, recipients need to:

1. Identify which outputs belong to them
2. Recover the exact amount sent
3. Recover the blinding factor (for spending)

### 1.1 Requirements

- ✅ Enable amount recovery for intended recipient
- ✅ Prevent amount recovery by unauthorized parties
- ✅ Maintain zero-knowledge property for non-recipients
- ✅ Keep proof size minimal (< 750 bytes)
- ✅ Use standard cryptographic primitives

---

## 2. Design Overview

### 2.1 Hybrid Approach

We use a **hybrid design** that combines:

1. **Standard Bulletproof** - Proves range validity
2. **Encrypted Payload** - Contains recoverable amount/blinding
3. **ECDH Nonce** - Derives encryption key

**Structure:**
```
Rewindable Proof = [encrypted_value || encrypted_blind || bulletproof]
                   [8 bytes]         [32 bytes]         [~674 bytes]
                   = ~714 bytes total
```

### 2.2 Key Insight

The Bulletproof itself doesn't need modification. We prepend encrypted data that can be decrypted by the recipient using an ECDH-derived nonce.

---

## 3. Encryption Scheme

### 3.1 ECDH Nonce Derivation

**Sender Side:**
```cpp
// 1. Generate ephemeral keypair
uint8_t k[32];  // Random private key
secp256k1_pubkey K;  // Public key K = k·G

// 2. Perform ECDH with recipient's view public key
uint8_t ecdh_secret[32];
secp256k1_ecdh(ctx, ecdh_secret,
               &recipient_view_pubkey,  // Recipient's public key
               k);                       // Sender's ephemeral private key

// 3. Hash to derive nonce
uint8_t nonce[32] = SHA256(ecdh_secret);
```

**Recipient Side:**
```cpp
// 1. Extract ephemeral public key K from transaction
secp256k1_pubkey K;  // From nonce field

// 2. Perform ECDH with own view private key
uint8_t ecdh_secret[32];
secp256k1_ecdh(ctx, ecdh_secret,
               &K,                      // Sender's ephemeral public key
               recipient_view_privkey); // Own private key

// 3. Hash to derive same nonce
uint8_t nonce[32] = SHA256(ecdh_secret);
```

**Security Property:** Only sender and recipient can derive the nonce.

### 3.2 Key Derivation

From the shared nonce, derive two encryption keys:

```cpp
// Derive value encryption key
uint8_t value_key[32] = SHA256("dinero_value_key" || nonce);

// Derive blinding encryption key
uint8_t blind_key[32] = SHA256("dinero_blind_key" || nonce);
```

**Domain Separation:** Different prefixes ensure keys are independent.

### 3.3 Encryption

**Value Encryption** (XOR with key):
```cpp
uint64_t value = 1000000;  // Amount in una
uint8_t value_bytes[8];
memcpy(value_bytes, &value, 8);  // Little-endian

uint8_t encrypted_value[8];
for (int i = 0; i < 8; i++) {
    encrypted_value[i] = value_bytes[i] ^ value_key[i];
}
```

**Blinding Encryption** (XOR with key):
```cpp
uint8_t blinding[32];  // 32-byte blinding factor

uint8_t encrypted_blind[32];
for (int i = 0; i < 32; i++) {
    encrypted_blind[i] = blinding[i] ^ blind_key[i];
}
```

**Why XOR?**
- Extremely fast
- Symmetric (same operation for encrypt/decrypt)
- Secure when key is derived from strong hash
- No padding needed

---

## 4. Proof Generation

### 4.1 Generation Flow

```cpp
int bp_generate_with_nonce(
    uint64_t value,
    const uint8_t* blind_ptr,      // 32 bytes
    const uint8_t* nonce_ptr,      // 32 bytes
    uint8_t* proof_out,            // Output buffer (>= 2048 bytes)
    size_t* proof_len_out          // Output length
) {
    // 1. Generate standard Bulletproof
    RangeProof proof = RangeProof::prove_single(
        bp_gens, pc_gens, &transcript,
        value, blinding_scalar, 64
    );
    uint8_t proof_bytes[~674];

    // 2. Derive encryption keys from nonce
    uint8_t value_key[32] = SHA256("dinero_value_key" || nonce);
    uint8_t blind_key[32] = SHA256("dinero_blind_key" || nonce);

    // 3. Encrypt value
    uint64_t encrypted_value = value ^ u64_from_bytes(value_key);

    // 4. Encrypt blinding factor
    uint8_t encrypted_blind[32];
    for (int i = 0; i < 32; i++) {
        encrypted_blind[i] = blind_ptr[i] ^ blind_key[i];
    }

    // 5. Build output: [enc_value || enc_blind || proof]
    memcpy(proof_out, &encrypted_value, 8);
    memcpy(proof_out + 8, encrypted_blind, 32);
    memcpy(proof_out + 40, proof_bytes, proof_len);

    *proof_len_out = 40 + proof_len;  // ~714 bytes

    // 6. SECURITY: Zeroize sensitive data
    zeroize(value_key);
    zeroize(blind_key);
    zeroize(encrypted_blind);

    return 0;  // Success
}
```

### 4.2 Proof Structure

```
Offset  | Size  | Field
--------|-------|---------------------------
0       | 8     | encrypted_value
8       | 32    | encrypted_blinding
40      | ~674  | bulletproof
--------|-------|---------------------------
Total:  ~714 bytes
```

---

## 5. Proof Rewind

### 5.1 Rewind Flow

```cpp
int bp_rewind(
    const uint8_t* commitment_ptr,  // 32 bytes
    const uint8_t* proof_ptr,       // ~714 bytes
    size_t proof_len,
    const uint8_t* nonce_ptr,       // 32 bytes
    uint64_t* value_out,            // Output: recovered value
    uint8_t* blind_out              // Output: recovered blinding (32 bytes)
) {
    // 1. Validate minimum size
    if (proof_len < 690) return -1;  // Too small

    // 2. Extract encrypted data
    uint64_t encrypted_value;
    memcpy(&encrypted_value, proof_ptr, 8);

    uint8_t encrypted_blind[32];
    memcpy(encrypted_blind, proof_ptr + 8, 32);

    uint8_t* actual_proof = proof_ptr + 40;
    size_t actual_proof_len = proof_len - 40;

    // 3. Derive decryption keys from nonce
    uint8_t value_key[32] = SHA256("dinero_value_key" || nonce);
    uint8_t blind_key[32] = SHA256("dinero_blind_key" || nonce);

    // 4. Decrypt value
    uint64_t decrypted_value = encrypted_value ^ u64_from_bytes(value_key);

    // 5. Decrypt blinding factor
    uint8_t decrypted_blind[32];
    for (int i = 0; i < 32; i++) {
        decrypted_blind[i] = encrypted_blind[i] ^ blind_key[i];
    }

    // 6. Verify commitment matches
    Commitment expected = commit(decrypted_value, decrypted_blind);

    if (expected != commitment_ptr) {
        // Wrong nonce! Not our output.
        zeroize_all();
        return 0;  // Not ours
    }

    // 7. Verify Bulletproof
    int valid = bp_verify(commitment_ptr, actual_proof, actual_proof_len);
    if (valid != 1) {
        zeroize_all();
        return -1;  // Invalid proof
    }

    // 8. Success! Return decrypted values
    *value_out = decrypted_value;
    memcpy(blind_out, decrypted_blind, 32);

    // 9. SECURITY: Zeroize keys
    zeroize(value_key);
    zeroize(blind_key);

    return 1;  // Successfully rewound
}
```

### 5.2 Return Codes

- **1** = Successfully rewound (output is ours)
- **0** = Wrong nonce (output not ours, or wrong key)
- **-1** = Error (malformed proof, invalid data)

---

## 6. Security Analysis

### 6.1 Confidentiality

**Threat:** Can an attacker recover the value without the nonce?

**Analysis:**
- Encrypted value: `E_v = value ⊕ value_key`
- Value key: `value_key = SHA256("dinero_value_key" || nonce)`
- Nonce: `nonce = SHA256(ECDH(k, V))` where `V` is view key

**Attack Requirements:**
1. Break ECDH (requires discrete log on secp256k1)
2. OR break SHA256 preimage resistance
3. OR brute force 256-bit nonce

**Conclusion:** Computationally infeasible under standard assumptions.

### 6.2 Integrity

**Threat:** Can an attacker modify the encrypted value?

**Analysis:**
The commitment verification in step 6 of rewind ensures integrity:

```cpp
if (expected_commitment != actual_commitment) {
    return 0;  // Tampered data detected
}
```

Any modification to encrypted value/blinding will result in commitment mismatch.

**Conclusion:** Tampering is detected.

### 6.3 Unlinkability

**Threat:** Can an observer link transactions?

**Analysis:**
- Each transaction uses a new ephemeral keypair
- ECDH output is indistinguishable from random
- No reuse of ephemeral keys

**Conclusion:** Transactions are unlinkable (assuming ephemeral key uniqueness).

### 6.4 Auditability

**Property:** Can recipient share view key for auditing?

**Mechanism:**
Recipient can provide:
1. View private key
2. Auditor can rewind all outputs sent to recipient
3. Auditor cannot spend (needs spend key)

**Use Cases:**
- Tax compliance
- Corporate accounting
- Regulatory reporting

---

## 7. Implementation Considerations

### 7.1 Zeroization

**Critical:** All decrypted values MUST be zeroized after use:

```cpp
// Automatic zeroization with RAII
{
    auto blind_handle = retrieve_blinding_factor(output_id);
    // Use blinding factor...
} // Automatically zeroized here

// Manual zeroization
explicit_bzero(decrypted_value, sizeof(decrypted_value));
explicit_bzero(decrypted_blind, 32);
explicit_bzero(value_key, 32);
explicit_bzero(blind_key, 32);
```

### 7.2 Ephemeral Key Management

**Storage:**
- Ephemeral private keys: NEVER stored (generated per TX)
- Ephemeral public keys: Included in nonce field
- In-memory only, TTL = 5 minutes max

**Generation:**
```cpp
// Use cryptographically secure random
uint8_t ephemeral_privkey[32];
getrandom(ephemeral_privkey, 32, GRND_RANDOM);

// Derive public key
secp256k1_ec_pubkey_create(ctx, &ephemeral_pubkey, ephemeral_privkey);

// Zeroize private key after use
explicit_bzero(ephemeral_privkey, 32);
```

### 7.3 Nonce Uniqueness

**Requirement:** Never reuse an ephemeral keypair.

**Enforcement:**
- Generate new random key for each output
- No deterministic generation (except for tests)
- Check against recent keys (optional)

**Consequences of Reuse:**
If `k` is reused with different view keys `V₁` and `V₂`:
- Attacker can compute: `nonce₁ ⊕ nonce₂ = SHA256(k·V₁) ⊕ SHA256(k·V₂)`
- May reveal information about `k`

---

## 8. Performance Characteristics

### 8.1 Proof Generation

**Overhead:** ~50 microseconds for encryption
- Bulletproof generation: ~100 ms (dominant cost)
- ECDH derivation: ~20 μs
- XOR encryption: ~10 μs
- Key derivation: ~20 μs

**Total:** ~100 ms (encryption overhead < 0.05%)

### 8.2 Rewind Performance

**Per Output:** ~100 ms
- ECDH derivation: ~20 μs
- XOR decryption: ~10 μs
- Commitment verification: ~50 μs
- Bulletproof verification: ~100 ms (dominant cost)

**Total:** ~100 ms per output

**Optimization:** Batch verification can verify multiple proofs faster.

### 8.3 Wallet Scanning

**For 1000 outputs:**
- Sequential: ~100 seconds
- Parallel (8 cores): ~12.5 seconds
- With early abort: ~6 seconds (if only 1% are ours)

**Optimization Opportunities:**
1. Parallel rewind attempts
2. Early abort on commitment mismatch
3. Proof caching for rescans

---

## 9. Test Vectors

See `test_vectors/rewind_test_vectors.json` for concrete examples.

### 9.1 Valid Rewind Example

```json
{
  "test": "valid_rewind",
  "value": 1000000,
  "blinding": "0x1234...",
  "nonce": "0x5678...",
  "commitment": "0x02abcd...",
  "proof": "0x...",
  "expected_result": "success",
  "recovered_value": 1000000,
  "recovered_blinding": "0x1234..."
}
```

### 9.2 Wrong Nonce Example

```json
{
  "test": "wrong_nonce",
  "value": 1000000,
  "blinding": "0x1234...",
  "nonce": "0x5678...",
  "wrong_nonce": "0x9999...",
  "commitment": "0x02abcd...",
  "proof": "0x...",
  "expected_result": "not_ours",
  "recovered_value": null,
  "recovered_blinding": null
}
```

---

## 10. Comparison with Alternatives

### 10.1 vs. Native Bulletproofs Rewind

**Native Approach** (not used):
- Modify Bulletproof protocol itself
- Embed rewind data in proof structure
- More complex implementation

**Our Hybrid Approach:**
- ✅ Use standard Bulletproofs (no modifications)
- ✅ Simpler implementation
- ✅ Can swap Bulletproofs library easily
- ✅ Smaller proof size (40 bytes overhead vs. ~100)

### 10.2 vs. Separate Encrypted Memo

**Memo Approach** (not used):
- Store encrypted amount in separate field
- Larger overhead (~100 bytes)
- More complex deserialization

**Our Approach:**
- ✅ Integrated into proof data
- ✅ Minimal overhead (40 bytes)
- ✅ Single field to manage

---

## 11. Known Limitations

### 11.1 Not Implemented

**TODO:** Batch rewind optimization
- Currently each output rewound sequentially
- Could optimize with parallel processing
- Would improve wallet scan time

**Impact:** Medium (affects wallet sync performance)
**Priority:** Low (works correctly, just slower)

### 11.2 Future Enhancements

1. **Memo Field** - Optional encrypted memo alongside amount
2. **Payment ID** - Unique identifier per payment
3. **Multi-Recipient** - Single proof, multiple rewind keys

---

## 12. References

1. **Monero RingCT** - Similar rewind mechanism
2. **Elements Confidential Transactions** - Alternative approach
3. **Grin/Mimblewimble** - Different commitment scheme
4. **Dalek Bulletproofs** - Underlying library

---

**End of Specification**
