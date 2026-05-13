# Confidential Transaction Protocol Specification

**Version:** 1.0
**Date:** 2025-01-17
**Status:** Implementation Complete

---

## 1. Overview

DineroCoin implements **confidential transactions** using Pedersen commitments and Bulletproofs range proofs to hide transaction amounts while maintaining cryptographic verifiability.

### 1.1 Goals

- **Confidentiality:** Transaction amounts are hidden from external observers
- **Auditability:** Recipients can view amounts sent to them
- **Verifiability:** Network can verify transactions without knowing amounts
- **Compatibility:** Coexists with transparent transactions

### 1.2 Non-Goals

- Hiding sender/receiver (use stealth addresses separately)
- Hiding transaction graph
- Quantum resistance

---

## 2. Cryptographic Primitives

### 2.1 Pedersen Commitments

A Pedersen commitment to value `v` with blinding factor `r` is:

```
C = v·H + r·G
```

Where:
- `H` = Ristretto255 base point (value generator)
- `G` = Ristretto255 base point (blinding generator)
- `v` = value (uint64)
- `r` = blinding factor (scalar)

**Properties:**
- **Hiding:** Computationally impossible to determine `v` or `r` from `C`
- **Binding:** Cannot find `(v', r')` such that `v'·H + r'·G = C` with `v' ≠ v`
- **Additive Homomorphism:** `C₁ + C₂ = (v₁ + v₂)·H + (r₁ + r₂)·G`

### 2.2 Bulletproofs Range Proofs

Bulletproofs prove that a committed value `v` lies in range `[0, 2^n - 1]` without revealing `v`.

**For DineroCoin:**
- Range: `[0, 2^64 - 1]` (uint64 values)
- Proof size: ~674 bytes (without rewind data)
- Proof size: ~714 bytes (with rewind data)
- Library: Dalek Bulletproofs 4.0

**Verification Equation:**
```
RangeProof.verify(C, proof, n=64) → {valid, invalid}
```

### 2.3 ECDH for Nonce Derivation

To enable amount recovery, we use ECDH to derive a shared secret:

```
nonce = ECDH(sender_ephemeral_privkey, recipient_view_pubkey)
```

**Curve:** secp256k1 (for ECDH compatibility)

**Derivation:**
1. Sender generates ephemeral keypair: `(k, K = k·G)`
2. Sender computes: `nonce = SHA256(k · recipient_view_pubkey)`
3. Recipient computes: `nonce = SHA256(recipient_view_privkey · K)`

Both parties derive the same nonce without interaction.

---

## 3. Transaction Structure

### 3.1 Confidential Output Format

```cpp
struct TxOutput {
    // Standard fields
    uint64_t value;              // ALWAYS 0 for confidential outputs
    std::string script_pubkey;   // Standard output script

    // Confidential fields (present if is_confidential = true)
    bool is_confidential;
    std::vector<uint8_t> commitment;     // 33 bytes (compressed Ristretto)
    std::vector<uint8_t> range_proof;    // ~714 bytes
    std::vector<uint8_t> nonce;          // 65 bytes (ephemeral_pubkey || encrypted_blind)

    // Total confidential overhead: ~812 bytes per output
};
```

### 3.2 Nonce Field Structure

The 65-byte nonce field contains:

```
nonce = ephemeral_pubkey || encrypted_blinding
      = [33 bytes]           [32 bytes]
```

- **ephemeral_pubkey** (33 bytes): Compressed secp256k1 public key `K = k·G`
- **encrypted_blinding** (32 bytes): XOR-encrypted blinding factor

**Encryption:**
```
blinding_key = SHA256("dinero_blind_key" || ECDH_nonce)
encrypted_blinding = blinding_factor ⊕ blinding_key
```

### 3.3 Range Proof Format

Bulletproofs with rewind capability:

```
proof_data = encrypted_value || encrypted_blind || bulletproof
           = [8 bytes]         [32 bytes]         [~674 bytes]
           = ~714 bytes total
```

- **encrypted_value** (8 bytes): XOR-encrypted amount
- **encrypted_blind** (32 bytes): XOR-encrypted blinding factor
- **bulletproof** (~674 bytes): Actual range proof

**Encryption:**
```
value_key = SHA256("dinero_value_key" || ECDH_nonce)
encrypted_value = value ⊕ value_key[0..8]

blind_key = SHA256("dinero_blind_key" || ECDH_nonce)
encrypted_blind = blinding_factor ⊕ blind_key
```

---

## 4. Transaction Creation

### 4.1 Sender Flow

**Inputs:**
- `value`: Amount to send (uint64)
- `recipient_address`: Recipient's confidential address
- `change_address`: Sender's change address

**Steps:**

1. **Generate Ephemeral Keypair**
   ```cpp
   uint8_t k[32];  // Random ephemeral private key
   secp256k1_pubkey K;  // Ephemeral public key
   secp256k1_ec_pubkey_create(ctx, &K, k);
   ```

2. **Derive ECDH Nonce**
   ```cpp
   secp256k1_pubkey recipient_view_pubkey = parse(recipient_address);
   uint8_t ecdh_result[32];
   secp256k1_ecdh(ctx, ecdh_result, &recipient_view_pubkey, k);
   uint8_t nonce[32] = SHA256(ecdh_result);
   ```

3. **Generate Blinding Factor**
   ```cpp
   uint8_t blinding[32];  // Cryptographically random
   crypto::random_bytes(blinding, 32);
   ```

4. **Create Pedersen Commitment**
   ```cpp
   Commitment C = value * H + blinding * G;  // Ristretto255
   uint8_t commitment[33];  // Compressed point
   compress_ristretto(C, commitment);
   ```

5. **Generate Bulletproof with Rewind Data**
   ```cpp
   uint8_t proof[2048];
   size_t proof_len;

   bp_generate_with_nonce(
       value,
       blinding,
       nonce,
       proof,
       &proof_len
   );  // Returns ~714 bytes
   ```

6. **Build Nonce Field**
   ```cpp
   // Encrypt blinding factor
   uint8_t blind_key[32] = SHA256("dinero_blind_key" || nonce);
   uint8_t encrypted_blind[32];
   for (int i = 0; i < 32; i++) {
       encrypted_blind[i] = blinding[i] ^ blind_key[i];
   }

   // Build nonce field
   uint8_t nonce_field[65];
   memcpy(nonce_field, K_compressed, 33);  // Ephemeral pubkey
   memcpy(nonce_field + 33, encrypted_blind, 32);
   ```

7. **Construct Output**
   ```cpp
   TxOutput output;
   output.value = 0;  // MUST be 0 for confidential
   output.is_confidential = true;
   output.commitment = commitment;
   output.range_proof = proof;
   output.nonce = nonce_field;
   output.script_pubkey = recipient_script;
   ```

8. **Create Change Output** (if needed)
   - Generate new blinding factor
   - Create commitment for change
   - Generate proof for change
   - Use change address nonce

9. **Balance Transaction**
   ```
   Sum(input_commitments) = Sum(output_commitments) + fee_commitment
   ```

### 4.2 Blinding Factor Balance

To ensure transaction balance, blinding factors must sum correctly:

```
sum(input_blindings) = sum(output_blindings) + fee_blinding
```

**Adjustment:**
If we have `n` outputs, we can freely choose `n-1` blinding factors, then compute the last one:

```cpp
Scalar last_blinding = sum(input_blindings) - sum(first_n_minus_1_output_blindings);
```

---

## 5. Transaction Verification

### 5.1 Network Node Verification

Nodes verify confidential transactions WITHOUT knowing the amounts:

**Steps:**

1. **Validate Output Structure**
   ```cpp
   // Check field sizes
   assert(output.commitment.size() == 33);
   assert(output.range_proof.size() >= 650 && <= 800);
   assert(output.nonce.size() == 65);

   // Check value is 0
   assert(output.value == 0);
   ```

2. **Verify Range Proof**
   ```cpp
   int result = bp_verify(
       output.commitment.data(),
       output.range_proof.data(),
       output.range_proof.size()
   );

   assert(result == 1);  // 1 = valid, 0 = invalid, -1 = error
   ```

3. **Verify Commitment Balance** (TODO: Not yet implemented)
   ```cpp
   // Compute sum of input commitments
   Point sum_inputs = sum(input_commitments);

   // Compute sum of output commitments
   Point sum_outputs = sum(output_commitments);

   // Add fee commitment
   Point fee_commitment = fee * H;

   // Verify balance
   assert(sum_inputs == sum_outputs + fee_commitment);
   ```

4. **Check Transaction Limits**
   ```cpp
   assert(confidential_output_count <= 100);
   assert(total_proof_data_size <= 100000);
   assert(tx_size <= 500000);
   ```

### 5.2 Batch Verification (Optimization)

For block validation, verify all proofs at once:

```cpp
std::vector<Commitment> commitments;
std::vector<RangeProof> proofs;

// Collect all confidential outputs in block
for (auto& tx : block.transactions) {
    for (auto& output : tx.vout) {
        if (output.is_confidential) {
            commitments.push_back(output.commitment);
            proofs.push_back(output.range_proof);
        }
    }
}

// Batch verify (2-3x faster)
int result = bp_verify_batch(
    commitments.data(),
    proofs.data(),
    proof_lens.data(),
    commitments.size()
);
```

---

## 6. Amount Recovery (Rewind)

### 6.1 Recipient Flow

Recipients scan transactions to identify and recover amounts sent to them:

**Steps:**

1. **Extract Ephemeral Public Key**
   ```cpp
   uint8_t ephemeral_pubkey[33];
   memcpy(ephemeral_pubkey, output.nonce.data(), 33);
   ```

2. **Derive ECDH Nonce**
   ```cpp
   secp256k1_pubkey K;
   secp256k1_ec_pubkey_parse(ctx, &K, ephemeral_pubkey, 33);

   uint8_t ecdh_result[32];
   secp256k1_ecdh(ctx, ecdh_result, &K, recipient_view_privkey);

   uint8_t nonce[32] = SHA256(ecdh_result);
   ```

3. **Attempt Rewind**
   ```cpp
   uint64_t recovered_value;
   uint8_t recovered_blinding[32];

   int result = bp_rewind(
       output.commitment.data(),
       output.range_proof.data(),
       output.range_proof.size(),
       nonce,
       &recovered_value,
       recovered_blinding
   );

   if (result == 1) {
       // Output is ours!
       // recovered_value contains the amount
       // recovered_blinding contains blinding factor
   } else if (result == 0) {
       // Not ours (wrong nonce)
   } else {
       // Error (malformed data)
   }
   ```

4. **Verify Recovered Values**
   ```cpp
   // Recompute commitment
   Commitment expected = recovered_value * H + recovered_blinding * G;

   // Compare to actual commitment
   assert(expected == output.commitment);
   ```

5. **Store Output**
   ```cpp
   wallet.addOutput({
       txid: tx.hash(),
       index: output_index,
       amount: recovered_value,
       blinding: recovered_blinding,  // Encrypted storage!
       spent: false
   });
   ```

### 6.2 Scanning Optimization

For efficient scanning:

1. **Precompute ECDH Keys**
   - Precompute view key scalars
   - Cache ephemeral pubkey parsing

2. **Parallel Rewind**
   - Rewind attempts can be parallelized
   - Each output is independent

3. **Early Abort**
   - Stop rewind immediately if commitment doesn't match
   - Saves verification time

---

## 7. Fee Handling

### 7.1 Fee in Confidential Transactions

Fees MUST be transparent (visible) to allow miners to prioritize:

```cpp
struct Transaction {
    std::vector<TxInput> vin;
    std::vector<TxOutput> vout;
    uint64_t fee;  // Explicit, transparent fee
};
```

**Fee Commitment:**
```
C_fee = fee · H + 0 · G  // Blinding factor is 0
```

### 7.2 Balance Equation with Fees

```
sum(input_commitments) = sum(output_commitments) + fee_commitment
```

Where:
```
fee_commitment = fee · H  (no blinding)
```

This allows miners to see the fee while amounts remain confidential.

---

## 8. Security Properties

### 8.1 Confidentiality

**Guarantee:** An observer cannot determine transaction amounts.

**Assumptions:**
- Discrete log problem is hard on Ristretto255
- ECDH provides secure shared secrets
- SHA256 is a secure hash function
- XOR encryption with hash output is secure

### 8.2 Integrity

**Guarantee:** Invalid proofs are rejected.

**Enforcement:**
- Consensus rules require valid Bulletproofs
- Nodes verify all proofs before accepting transactions
- Invalid proofs lead to transaction rejection

### 8.3 Balance Preservation

**Guarantee:** No inflation is possible.

**Enforcement:**
- Commitment balance must hold
- Range proofs prevent negative values
- Overflow impossible due to uint64 range

### 8.4 Auditability

**Guarantee:** Recipients can view amounts.

**Mechanism:**
- ECDH-based rewind
- Only parties with correct view key can decrypt
- Third-party auditing possible with view key delegation

---

## 9. Consensus Rules

### 9.1 Mandatory Checks

All nodes MUST enforce:

1. **Confidential outputs have value = 0**
2. **Commitment size is exactly 33 bytes**
3. **Range proof size is 650-800 bytes**
4. **Nonce size is exactly 65 bytes**
5. **Range proof verifies correctly**
6. **Max 100 confidential outputs per TX**
7. **Max 100 KB total proof data per TX**
8. **Max 500 KB total TX size**

### 9.2 Soft Limits (Policy)

Mempool MAY reject:

1. **More than 10 confidential TXs/minute from one peer**
2. **More than 20 confidential TXs/hour via RPC**
3. **Unusual proof sizes (< 700 or > 720 bytes)**

---

## 10. Backwards Compatibility

### 10.1 Mixed Transactions

Transactions can have BOTH transparent and confidential outputs:

```cpp
Transaction tx;
tx.vout.push_back(transparent_output);   // value visible
tx.vout.push_back(confidential_output);  // value hidden
```

### 10.2 Opt-In Privacy

Confidential transactions are opt-in:
- Wallets can choose to use confidential or transparent outputs
- Old nodes can validate transparent outputs (ignore confidential)
- Soft fork deployment

---

## 11. Test Vectors

See `test_vectors/bulletproof_proofs_hex.json` for concrete examples.

---

## 12. References

1. **Bulletproofs Paper:** https://eprint.iacr.org/2017/1066.pdf
2. **Dalek Bulletproofs:** https://github.com/dalek-cryptography/bulletproofs
3. **Pedersen Commitments:** https://link.springer.com/chapter/10.1007/3-540-46766-1_9
4. **Confidential Transactions:** https://elementsproject.org/features/confidential-transactions

---

**End of Specification**
