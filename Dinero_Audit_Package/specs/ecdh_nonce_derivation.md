# ECDH Nonce Derivation Specification

**Version:** 1.0
**Date:** 2025-01-17
**Status:** Implemented

---

## 1. Overview

This document specifies the Elliptic Curve Diffie-Hellman (ECDH) key agreement protocol used to derive shared nonces for confidential transaction rewind capability in DineroCoin.

### 1.1 Purpose

The ECDH nonce enables:
1. **Selective Disclosure:** Only recipient can decrypt amount
2. **Auditability:** View key sharing allows third-party audits
3. **No Interaction:** Sender derives nonce without recipient involvement
4. **Unlinkability:** Each output uses unique ephemeral key

---

## 2. Cryptographic Foundation

### 2.1 Curve Selection

**Curve:** secp256k1 (same as Bitcoin)

**Parameters:**
```
p = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE FFFFFC2F
a = 0
b = 7
G = (x, y) where:
  x = 79BE667E F9DCBBAC 55A06295 CE870B07 029BFCDB 2DCE28D9 59F2815B 16F81798
  y = 483ADA77 26A3C465 5DA4FBFC 0E1108A8 FD17B448 A6855419 9C47D08F FB10D4B8
n = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE BAAEDCE6 AF48A03B BFD25E8C D0364141
h = 1
```

**Security Level:** ~128 bits

### 2.2 Why secp256k1?

- ✅ **Compatibility:** Same as Bitcoin address keys
- ✅ **Well-Tested:** Extensively audited and used
- ✅ **Hardware Support:** Optimized implementations available
- ✅ **Key Reuse:** Can use existing wallet infrastructure

**Note:** Commitments use Ristretto255, but ECDH uses secp256k1 for key agreement.

---

## 3. Key Hierarchy

### 3.1 Recipient Keys

Recipients have two key types:

**View Key Pair:**
```
view_privkey (v)  - 32-byte scalar
view_pubkey  (V)  - 33-byte compressed point, V = v·G
```

**Purpose:** Decrypt amounts, identify outputs

**Spend Key Pair:**
```
spend_privkey (s) - 32-byte scalar
spend_pubkey  (S) - 33-byte compressed point, S = s·G
```

**Purpose:** Create signatures to spend outputs

### 3.2 Key Derivation

From master seed:
```
master_seed = 64-byte entropy

view_privkey  = HMAC-SHA512(key="dinero_view",  data=master_seed)[0:32]
spend_privkey = HMAC-SHA512(key="dinero_spend", data=master_seed)[0:32]

view_pubkey  = view_privkey · G
spend_pubkey = spend_privkey · G
```

**Security:** View key compromise does NOT allow spending.

---

## 4. ECDH Protocol

### 4.1 Sender-Side Derivation

**Inputs:**
- `recipient_view_pubkey` (V) - 33-byte compressed point
- `value` - uint64 amount to send
- `blinding` - 32-byte random blinding factor

**Steps:**

```cpp
// 1. Generate ephemeral keypair
uint8_t k[32];  // Ephemeral private key
crypto::random_bytes(k, 32);  // MUST be cryptographically random

secp256k1_pubkey K_point;
secp256k1_ec_pubkey_create(ctx, &K_point, k);

uint8_t K[33];  // Ephemeral public key (compressed)
secp256k1_ec_pubkey_serialize(ctx, K, &len, &K_point, SECP256K1_EC_COMPRESSED);

// 2. Parse recipient's view public key
secp256k1_pubkey V_point;
secp256k1_ec_pubkey_parse(ctx, &V_point, recipient_view_pubkey, 33);

// 3. Perform ECDH: shared_secret = k · V = k · (v·G) = v · (k·G) = v·K
uint8_t ecdh_result[32];
int result = secp256k1_ecdh(
    ctx,
    ecdh_result,         // Output: 32-byte shared secret
    &V_point,            // Recipient's view pubkey
    k,                   // Ephemeral private key
    NULL,                // Use default hash function (SHA256)
    NULL                 // No additional data
);
assert(result == 1);

// 4. Hash to derive nonce
uint8_t nonce[32];
SHA256_CTX sha;
SHA256_Init(&sha);
SHA256_Update(&sha, ecdh_result, 32);
SHA256_Final(nonce, &sha);

// 5. SECURITY: Zeroize ephemeral private key
explicit_bzero(k, 32);
explicit_bzero(ecdh_result, 32);
```

**Output:**
- `nonce` - 32-byte shared secret for encryption
- `K` - 33-byte ephemeral public key (included in TX)

### 4.2 Recipient-Side Derivation

**Inputs:**
- `ephemeral_pubkey` (K) - 33-byte compressed point from TX
- `view_privkey` (v) - Recipient's 32-byte view private key

**Steps:**

```cpp
// 1. Parse ephemeral public key from transaction
secp256k1_pubkey K_point;
int result = secp256k1_ec_pubkey_parse(ctx, &K_point, ephemeral_pubkey, 33);
if (result != 1) return ERROR_INVALID_PUBKEY;

// 2. Perform ECDH: shared_secret = v · K = v · (k·G) = k·v·G = k·V
uint8_t ecdh_result[32];
result = secp256k1_ecdh(
    ctx,
    ecdh_result,         // Output: 32-byte shared secret
    &K_point,            // Ephemeral public key from TX
    view_privkey,        // Own view private key
    NULL,                // Use default hash function (SHA256)
    NULL                 // No additional data
);
assert(result == 1);

// 3. Hash to derive same nonce
uint8_t nonce[32];
SHA256_CTX sha;
SHA256_Init(&sha);
SHA256_Update(&sha, ecdh_result, 32);
SHA256_Final(nonce, &sha);

// 4. SECURITY: Zeroize ECDH result
explicit_bzero(ecdh_result, 32);
```

**Output:**
- `nonce` - Same 32-byte shared secret as sender derived

**Verification:** Both parties compute identical `nonce` value.

---

## 5. Key Derivation from Nonce

### 5.1 Domain-Separated Key Derivation

From the shared nonce, derive separate encryption keys:

```cpp
// Derive value encryption key (8 bytes needed)
uint8_t value_key_full[32];
SHA256_CTX sha;
SHA256_Init(&sha);
SHA256_Update(&sha, "dinero_value_key", 16);  // Domain separator
SHA256_Update(&sha, nonce, 32);
SHA256_Final(value_key_full, &sha);

uint8_t value_key[8];
memcpy(value_key, value_key_full, 8);  // Use first 8 bytes

// Derive blinding encryption key (32 bytes needed)
uint8_t blind_key[32];
SHA256_Init(&sha);
SHA256_Update(&sha, "dinero_blind_key", 16);  // Domain separator
SHA256_Update(&sha, nonce, 32);
SHA256_Final(blind_key, &sha);
```

**Domain Separators:**
- `"dinero_value_key"` - For value encryption
- `"dinero_blind_key"` - For blinding factor encryption

**Purpose:** Ensure keys are cryptographically independent.

### 5.2 Encryption

**Value Encryption (XOR):**
```cpp
uint64_t value = 1000000;
uint64_t value_key_u64 = *((uint64_t*)value_key);
uint64_t encrypted_value = value ^ value_key_u64;
```

**Blinding Encryption (XOR):**
```cpp
uint8_t encrypted_blind[32];
for (int i = 0; i < 32; i++) {
    encrypted_blind[i] = blinding[i] ^ blind_key[i];
}
```

**Why XOR?**
- Symmetric (same operation for encrypt/decrypt)
- Fast (no AES overhead)
- Secure when key is from strong hash
- No IV or nonce needed

---

## 6. Security Properties

### 6.1 Confidentiality

**Threat:** Can attacker recover nonce without private keys?

**Analysis:**
```
Attacker sees: K (ephemeral pubkey), V (view pubkey)
Needs: k (ephemeral privkey) OR v (view privkey)

To find k: Solve discrete log K = k·G
To find v: Solve discrete log V = v·G
```

**Security:** Relies on hardness of ECDLP on secp256k1 (~128-bit security).

### 6.2 Unlinkability

**Threat:** Can attacker link two outputs to same recipient?

**Analysis:**
```
Output 1: Uses ephemeral key k₁ → ECDH result = k₁·V
Output 2: Uses ephemeral key k₂ → ECDH result = k₂·V

Attacker sees: K₁ = k₁·G, K₂ = k₂·G
```

**Property:** Without knowing `v`, cannot link `k₁·V` and `k₂·V`.

**Requirement:** MUST use unique ephemeral key for each output.

### 6.3 Forward Secrecy

**Threat:** If view key is compromised, can past transactions be decrypted?

**Answer:** YES - view key compromise reveals all historical amounts.

**Mitigation:**
- Rotate view keys periodically (future upgrade)
- Use subaddresses with different view keys
- Hardware wallet isolation

**Trade-off:** Perfect forward secrecy conflicts with wallet scanning.

### 6.4 Replay Attacks

**Threat:** Can attacker reuse ephemeral pubkey `K`?

**Analysis:**
```
TX1: Uses K → Recipient derives nonce₁ = SHA256(v·K)
TX2: Uses same K → Recipient derives nonce₂ = SHA256(v·K)

Result: nonce₁ = nonce₂ (same encrypted data leaked)
```

**Defense:** MUST generate new random `k` for each output.

**Enforcement:** Wallet-level (no consensus check for ephemeral key uniqueness).

---

## 7. Implementation Details

### 7.1 secp256k1 Library Usage

**Initialization:**
```cpp
secp256k1_context* ctx = secp256k1_context_create(
    SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
);
```

**Randomization (IMPORTANT):**
```cpp
uint8_t seed[32];
crypto::random_bytes(seed, 32);
secp256k1_context_randomize(ctx, seed);
explicit_bzero(seed, 32);
```

**Purpose:** Defends against side-channel attacks.

### 7.2 ECDH Function

**Signature:**
```c
int secp256k1_ecdh(
    const secp256k1_context* ctx,
    unsigned char* output,           // 32-byte output
    const secp256k1_pubkey* pubkey,  // Other party's public key
    const unsigned char* seckey,     // Own private key (32 bytes)
    secp256k1_ecdh_hash_function hashfp,  // Hash function (NULL = SHA256)
    void* data                        // Additional data (NULL)
);
```

**Default Hash:** SHA256(x-coordinate of shared point)

**Return:** 1 on success, 0 on failure

### 7.3 Error Handling

```cpp
// Check pubkey parse result
if (secp256k1_ec_pubkey_parse(ctx, &pubkey, data, 33) != 1) {
    // Invalid public key encoding
    return ERROR_INVALID_EPHEMERAL_PUBKEY;
}

// Check ECDH result
if (secp256k1_ecdh(ctx, output, &pubkey, privkey, NULL, NULL) != 1) {
    // ECDH failed (shouldn't happen with valid inputs)
    return ERROR_ECDH_FAILED;
}
```

### 7.4 Memory Safety

**CRITICAL:** Always zeroize sensitive data:

```cpp
void derive_nonce(const uint8_t* view_privkey, const uint8_t* ephemeral_pubkey, uint8_t* nonce_out) {
    secp256k1_pubkey pubkey;
    uint8_t ecdh_result[32];

    // ... perform ECDH ...

    // Hash to nonce
    SHA256(ecdh_result, 32, nonce_out);

    // SECURITY: Zeroize intermediate values
    explicit_bzero(ecdh_result, 32);
    explicit_bzero(&pubkey, sizeof(pubkey));
}
```

---

## 8. Test Vectors

### 8.1 ECDH Test Vector 1

```json
{
  "test": "ecdh_basic",
  "ephemeral_privkey": "0x0101010101010101010101010101010101010101010101010101010101010101",
  "ephemeral_pubkey": "0x031b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f",
  "view_privkey": "0x0202020202020202020202020202020202020202020202020202020202020202",
  "view_pubkey": "0x02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5",
  "ecdh_shared_secret": "0x...",
  "derived_nonce": "0x...",
  "value_key": "0x...",
  "blind_key": "0x..."
}
```

### 8.2 ECDH Test Vector 2 (Wrong Key)

```json
{
  "test": "ecdh_wrong_key",
  "ephemeral_pubkey": "0x031b84c5567b126440995d3ed5aaba0565d71e1834604819ff9c17f5e9d5dd078f",
  "correct_view_privkey": "0x0202020202020202020202020202020202020202020202020202020202020202",
  "wrong_view_privkey": "0x0303030303030303030303030303030303030303030303030303030303030303",
  "correct_nonce": "0xabcd...",
  "wrong_nonce": "0x1234...",
  "note": "Different nonces prove output not ours"
}
```

### 8.3 Domain Separation Test

```json
{
  "test": "domain_separation",
  "nonce": "0x0404040404040404040404040404040404040404040404040404040404040404",
  "value_key_input": "dinero_value_key || nonce",
  "value_key_output": "0x...",
  "blind_key_input": "dinero_blind_key || nonce",
  "blind_key_output": "0x...",
  "note": "Same nonce produces different keys"
}
```

---

## 9. Comparison with Alternatives

### 9.1 vs. Shared Secret in Proof

**Alternative:** Embed shared secret directly in Bulletproof

**Why Not Used:**
- Modifying Bulletproof protocol is complex
- Larger proof size
- Harder to audit
- Not compatible with standard Bulletproofs library

**Our Approach:**
- ✅ Use standard Bulletproofs
- ✅ Separate ECDH layer
- ✅ Easier to understand and audit

### 9.2 vs. RSA Encryption

**Alternative:** RSA-encrypt value with recipient's public key

**Why Not Used:**
- RSA keys are large (2048-bit)
- Slower encryption/decryption
- Quantum-vulnerable
- Doesn't match elliptic curve ecosystem

**Our Approach:**
- ✅ Native elliptic curve
- ✅ Smaller keys (33 bytes)
- ✅ Faster operations
- ✅ Consistent with rest of protocol

### 9.3 vs. Monero's Approach

**Monero:** Uses similar ECDH but with Curve25519

**DineroCoin Differences:**
- Uses secp256k1 (Bitcoin-compatible)
- Domain-separated key derivation
- XOR encryption instead of ChaCha20

**Rationale:** Prioritize Bitcoin ecosystem compatibility.

---

## 10. Known Limitations

### 10.1 View Key Compromise

**Impact:** All historical amounts revealed

**Mitigation:**
- Keep view key on separate device
- Use hardware wallet for view key
- Rotate view keys periodically (future)

### 10.2 Ephemeral Key Reuse

**Risk:** If same ephemeral key used twice, encrypted data correlates

**Mitigation:**
- MUST generate new random key for each output
- Wallet-level enforcement
- Future: Consensus rule to reject duplicate ephemeral keys in same block

### 10.3 No Post-Quantum Security

**Risk:** Quantum computer breaks ECDLP → recovers private keys

**Timeline:** Not a near-term threat (10+ years)

**Future:** Upgrade to post-quantum ECDH (e.g., SIDH, CSIDH)

---

## 11. Auditor Checklist

### 11.1 Code Review Points

- [ ] Ephemeral key generation uses cryptographically secure RNG
- [ ] secp256k1 context is randomized on initialization
- [ ] ECDH result is zeroized after use
- [ ] Ephemeral private key is zeroized after ECDH
- [ ] View private key is never logged or exposed
- [ ] Domain separators are correct and unique
- [ ] nonce derivation uses SHA256 correctly
- [ ] No ephemeral key reuse across outputs
- [ ] Error handling doesn't leak information
- [ ] Memory allocations are bounded

### 11.2 Test Coverage

- [ ] ECDH produces same result for sender and recipient
- [ ] Different ephemeral keys produce different nonces
- [ ] Wrong view key produces different nonce
- [ ] Domain separation produces independent keys
- [ ] Invalid ephemeral pubkeys are rejected
- [ ] Zero private key is rejected
- [ ] Overflow conditions handled

### 11.3 Cryptographic Verification

- [ ] ECDH uses secp256k1 correctly
- [ ] SHA256 hashing is standard-compliant
- [ ] No custom crypto (all from libsecp256k1)
- [ ] Key generation follows best practices
- [ ] No weak keys allowed

---

## 12. References

1. **RFC 6090:** Fundamental Elliptic Curve Cryptography Algorithms
2. **SEC 2:** Recommended Elliptic Curve Domain Parameters (secp256k1)
3. **BIP-32:** Hierarchical Deterministic Wallets
4. **Monero Stealth Addresses:** Similar ECDH approach
5. **libsecp256k1:** https://github.com/bitcoin-core/secp256k1

---

**End of Specification**
