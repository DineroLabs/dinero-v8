# ZK Transaction Structure - Design Document

**Status:** Design document - Do NOT implement yet (user working on Phase A)
**Purpose:** Document how to extend `TxOutput` for confidential transactions

---

## Current Transaction Structure

**File:** `/Users/haydarevich/Documents/DineroCoin/include/wallet/transaction.h`

```cpp
struct TxOutput {
    uint64_t value;  // Amount in una (una)
    std::vector<uint8_t> scriptPubKey;

    TxOutput() : value(0) {}
    TxOutput(uint64_t val, const std::vector<uint8_t>& script)
        : value(val), scriptPubKey(script) {}

    // Witness version detection...
    uint8_t GetWitnessVersion() const;
    bool IsWitness() const;
    bool IsSegWitV0() const;
    bool IsTaproot() const;
};
```

---

## Proposed Confidential Transaction Structure

### Phase A/B Extension (Backward Compatible)

```cpp
// Forward declarations
struct secp256k1_pedersen_commitment;

struct TxOutput {
    // Existing transparent fields (KEEP FOR BACKWARD COMPATIBILITY)
    uint64_t value;  // Amount in una (una) - 0 if confidential
    std::vector<uint8_t> scriptPubKey;

    // NEW: Confidential transaction fields (Phase A/B)
    bool is_confidential;  // True if this is a CT output
    std::vector<uint8_t> commitment;  // 33-byte Pedersen commitment (serialized)
    std::vector<uint8_t> range_proof;  // Bulletproof range proof (Phase B, ~5KB)

    // Constructors
    TxOutput() : value(0), is_confidential(false) {}

    // Transparent output (existing behavior)
    TxOutput(uint64_t val, const std::vector<uint8_t>& script)
        : value(val), scriptPubKey(script), is_confidential(false) {}

    // Confidential output (NEW - Phase A/B)
    TxOutput(const std::vector<uint8_t>& commit,
             const std::vector<uint8_t>& proof,
             const std::vector<uint8_t>& script)
        : value(0),  // Hidden!
          scriptPubKey(script),
          is_confidential(true),
          commitment(commit),
          range_proof(proof) {}

    // Existing methods...
    uint8_t GetWitnessVersion() const;
    bool IsWitness() const;
    bool IsSegWitV0() const;
    bool IsTaproot() const;

    // NEW: Confidential transaction methods
    bool IsConfidential() const { return is_confidential; }
    bool HasRangeProof() const { return !range_proof.empty(); }

    // Get commitment as secp256k1 structure (Phase A)
    bool GetCommitment(secp256k1_pedersen_commitment* commit_out) const;

    // Verify range proof (Phase B)
    bool VerifyRangeProof(secp256k1_context* ctx,
                          uint64_t* min_value_out,
                          uint64_t* max_value_out) const;

    // Size calculations (for fee estimation)
    size_t GetSize() const {
        if (!is_confidential) {
            // Transparent: 8 bytes (value) + varint + scriptPubKey
            return 8 + GetVarintSize(scriptPubKey.size()) + scriptPubKey.size();
        } else {
            // Confidential: 33 bytes (commitment) + varint + range_proof + scriptPubKey
            return 33 +
                   GetVarintSize(range_proof.size()) + range_proof.size() +
                   GetVarintSize(scriptPubKey.size()) + scriptPubKey.size();
        }
    }

private:
    static size_t GetVarintSize(uint64_t n) {
        if (n < 0xfd) return 1;
        if (n <= 0xffff) return 3;
        if (n <= 0xffffffff) return 5;
        return 9;
    }
};
```

---

## Serialization Format

### Transparent Output (Existing)

```
┌──────────────┬───────────────┬─────────────────┐
│ value (8B)   │ script_len    │ scriptPubKey    │
│ uint64_t     │ varint        │ bytes           │
└──────────────┴───────────────┴─────────────────┘
        8 bytes      1-9 bytes      variable
```

**Example:** 1.0 DINERO to P2WPKH address
```
0x00e1f50500000000  // 1.0 DINERO (100,000,000 una)
0x16              // 22 bytes
0x0014...         // P2WPKH script (OP_0 <20-byte-hash>)
```

### Confidential Output (Phase A/B)

```
┌──────────────┬───────────────┬─────────────────┬───────────────┬─────────────────┬───────────────┬─────────────────┐
│ marker (1B)  │ commitment    │ proof_len       │ range_proof   │ script_len    │ scriptPubKey    │
│ 0xFF         │ 33 bytes      │ varint          │ bytes         │ varint        │ bytes           │
└──────────────┴───────────────┴─────────────────┴───────────────┴───────────────┴─────────────────┘
      1 byte        33 bytes       1-9 bytes        ~5KB           1-9 bytes       variable
```

**Example:** Hidden amount to P2WPKH address (Phase B)
```
0xFF              // Confidential marker
0x09...           // 33-byte Pedersen commitment
0xfd1410          // 5140 bytes range proof length (varint)
0x63...           // Bulletproof range proof data
0x16              // 22 bytes
0x0014...         // P2WPKH script (OP_0 <20-byte-hash>)
```

**Note:** Using `0xFF` as marker because:
- In Bitcoin, first byte is the 8-byte value (little-endian)
- Value `0xFF` followed by 7 bytes would be ~1.8 × 10^16 una (180 million DINERO)
- This is above the 21M coin cap, making it a safe sentinel value
- Alternative: Use witness version field extension

---

## Transaction Validation Changes

### Phase A: Commitment Balance Only

```cpp
// src/consensus/tx_validation.cpp
bool CheckTransaction(const Transaction& tx, ValidationState& state) {
    // Existing transparent validation
    if (!HasConfidentialOutputs(tx)) {
        return CheckTransparentTransaction(tx, state);
    }

    // NEW: Mixed transaction validation (transparent + confidential)
    return CheckConfidentialTransaction(tx, state);
}

bool CheckConfidentialTransaction(const Transaction& tx, ValidationState& state) {
    // Step 1: Collect commitments from inputs and outputs
    std::vector<secp256k1_pedersen_commitment> input_commits;
    std::vector<secp256k1_pedersen_commitment> output_commits;

    for (const auto& input : tx.vin) {
        // Look up previous output
        TxOutput prev_output = GetUTXO(input.prevout);

        if (prev_output.is_confidential) {
            secp256k1_pedersen_commitment commit;
            if (!prev_output.GetCommitment(&commit)) {
                return state.Invalid(ValidationState::CONSENSUS, "bad-commitment-parse");
            }
            input_commits.push_back(commit);
        } else {
            // Transparent input: create commitment C = value·G + 0·H
            secp256k1_pedersen_commitment commit;
            unsigned char zero_blind[32] = {0};
            secp256k1_pedersen_commit(GetSecp256k1Context(),
                                      &commit,
                                      zero_blind,
                                      prev_output.value,
                                      secp256k1_generator_h);
            input_commits.push_back(commit);
        }
    }

    for (const auto& output : tx.vout) {
        if (output.is_confidential) {
            secp256k1_pedersen_commitment commit;
            if (!output.GetCommitment(&commit)) {
                return state.Invalid(ValidationState::CONSENSUS, "bad-commitment-parse");
            }
            output_commits.push_back(commit);
        } else {
            // Transparent output: create commitment
            secp256k1_pedersen_commitment commit;
            unsigned char zero_blind[32] = {0};
            secp256k1_pedersen_commit(GetSecp256k1Context(),
                                      &commit,
                                      zero_blind,
                                      output.value,
                                      secp256k1_generator_h);
            output_commits.push_back(commit);
        }
    }

    // Add fee as transparent output (sum(inputs) - sum(outputs) = fee)
    uint64_t fee = CalculateFee(tx);  // Requires at least one transparent output
    if (fee > 0) {
        secp256k1_pedersen_commitment fee_commit;
        unsigned char zero_blind[32] = {0};
        secp256k1_pedersen_commit(GetSecp256k1Context(),
                                  &fee_commit,
                                  zero_blind,
                                  fee,
                                  secp256k1_generator_h);
        output_commits.push_back(fee_commit);
    }

    // Step 2: Verify balance: sum(inputs) == sum(outputs + fee)
    std::vector<const secp256k1_pedersen_commitment*> in_ptrs, out_ptrs;
    for (auto& c : input_commits) in_ptrs.push_back(&c);
    for (auto& c : output_commits) out_ptrs.push_back(&c);

    if (!secp256k1_pedersen_verify_tally(
            GetSecp256k1Context(),
            in_ptrs.data(), in_ptrs.size(),
            out_ptrs.data(), out_ptrs.size())) {
        return state.Invalid(ValidationState::CONSENSUS, "bad-confidential-balance");
    }

    // Step 3: Phase B - Verify range proofs (prevents negative amounts)
    for (const auto& output : tx.vout) {
        if (output.is_confidential && !output.range_proof.empty()) {
            uint64_t min_val, max_val;
            if (!output.VerifyRangeProof(GetSecp256k1Context(), &min_val, &max_val)) {
                return state.Invalid(ValidationState::CONSENSUS, "bad-range-proof");
            }
        }
    }

    return true;
}
```

---

## Wallet Database Changes

### Store Blinding Factors (CRITICAL)

```cpp
// src/wallet/wallet_db.h
struct ConfidentialOutputMetadata {
    std::string txid;
    uint32_t vout;
    uint64_t amount;  // Known amount (we created this output)
    std::vector<uint8_t> blinding_factor;  // 32-byte secret
    std::vector<uint8_t> nonce;  // 32-byte nonce (for receiver)
    std::string recipient_address;  // Who received this
    int64_t timestamp;

    // Security: Encrypt blinding_factor at rest!
    std::vector<uint8_t> encrypted_blinding_factor;
};

class WalletDB {
public:
    // Store metadata for confidential outputs we create
    bool StoreConfidentialOutput(const ConfidentialOutputMetadata& meta);

    // Retrieve metadata for spending
    std::optional<ConfidentialOutputMetadata> GetConfidentialOutput(
        const std::string& txid, uint32_t vout);

    // Security: Encrypt/decrypt blinding factors
    bool EncryptBlindingFactor(const std::vector<uint8_t>& blind,
                               std::vector<uint8_t>& encrypted_out);
    bool DecryptBlindingFactor(const std::vector<uint8_t>& encrypted,
                               std::vector<uint8_t>& blind_out);
};
```

**CRITICAL:** Blinding factors MUST be encrypted at rest. If an attacker gets the wallet database, they should NOT be able to:
1. Spend confidential outputs (needs blinding factor)
2. Learn amounts (needs blinding factor)

**Encryption options:**
- AES-256-GCM with wallet password-derived key
- Store encrypted key in secure enclave (macOS Keychain, Windows Credential Store)
- Use Argon2id for key derivation (already vendored!)

---

## Size Impact Analysis

### Transparent Transaction (Existing)

```
1 input, 2 outputs (P2WPKH)
┌─────────────────────┬──────┐
│ Component           │ Size │
├─────────────────────┼──────┤
│ Version             │ 4    │
│ Marker + Flag       │ 2    │
│ Input count         │ 1    │
│ Input (P2WPKH)      │ 41   │
│ Witness (P2WPKH)    │ 107  │
│ Output count        │ 1    │
│ Output 1 (P2WPKH)   │ 31   │
│ Output 2 (P2WPKH)   │ 31   │
│ Locktime            │ 4    │
├─────────────────────┼──────┤
│ Total               │ 222  │
│ Virtual size (vB)   │ 141  │
└─────────────────────┴──────┘
```

**Fee:** 141 vB × 1 sat/vB = 141 una (0.00000141 DINERO)

### Confidential Transaction (Phase B - Bulletproofs)

```
1 input, 2 confidential outputs (P2WPKH)
┌─────────────────────┬──────┐
│ Component           │ Size │
├─────────────────────┼──────┤
│ Version             │ 4    │
│ Marker + Flag       │ 2    │
│ Input count         │ 1    │
│ Input (P2WPKH)      │ 41   │
│ Witness (P2WPKH)    │ 107  │
│ Output count        │ 1    │
│ Output 1 (CT)       │ 5,194│  ← 33B commitment + 5134B proof + 27B overhead
│ Output 2 (CT)       │ 5,194│
│ Locktime            │ 4    │
├─────────────────────┼──────┤
│ Total               │ 10,547│
│ Virtual size (vB)   │ 10,547│  (No witness discount for range proofs)
└─────────────────────┴──────┘
```

**Fee:** 10,547 vB × 1 sat/vB = 10,547 una (0.00010547 DINERO)
**Overhead:** ~75× larger, ~75× more expensive

### Future: Aggregated Bulletproofs (Optimization)

```
1 input, 2 confidential outputs (aggregated proof)
┌─────────────────────┬──────┐
│ Component           │ Size │
├─────────────────────┼──────┤
│ Version             │ 4    │
│ Marker + Flag       │ 2    │
│ Input count         │ 1    │
│ Input (P2WPKH)      │ 41   │
│ Witness (P2WPKH)    │ 107  │
│ Output count        │ 1    │
│ Output 1 (commit)   │ 60   │  ← 33B commitment + 27B overhead (NO proof)
│ Output 2 (commit)   │ 60   │
│ Aggregated proof    │ 680  │  ← Single proof for both outputs!
│ Locktime            │ 4    │
├─────────────────────┼──────┤
│ Total               │ 958  │
│ Virtual size (vB)   │ 958  │
└─────────────────────┴──────┘
```

**Fee:** 958 vB × 1 sat/vB = 958 una (0.00000958 DINERO)
**Overhead:** ~7× larger (much better!)

---

## Implementation Checklist

**Phase A (Pedersen Commitments):**
- [ ] Extend `TxOutput` structure with confidential fields
- [ ] Implement serialization/deserialization (0xFF marker format)
- [ ] Add `GetCommitment()` helper method
- [ ] Implement `CheckConfidentialTransaction()` validation
- [ ] Add wallet database schema for blinding factors
- [ ] Implement blinding factor encryption (Argon2id + AES-256-GCM)

**Phase B (Range Proofs):**
- [ ] Add range proof generation to transaction builder
- [ ] Implement `VerifyRangeProof()` method
- [ ] Add range proof verification to consensus validation
- [ ] Optimize: Implement aggregated proof support
- [ ] Add fee estimation for confidential TXs

**Phase C (View Keys):**
- [ ] Implement view key scanning (rewind range proofs)
- [ ] Add stealth address support
- [ ] Automatic nonce derivation (ECDH)

---

## Security Considerations

### 1. Blinding Factor Management

**CRITICAL:**
- NEVER log blinding factors
- NEVER send blinding factors over RPC unencrypted
- ALWAYS encrypt at rest in wallet database
- Use secure memory wiping after use

```cpp
// Good practice: Secure memory wiping
void SecureWipeMemory(void* ptr, size_t len) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (len--) *p++ = 0;
}

// After using blinding factor:
unsigned char blind[32];
// ... use blind ...
SecureWipeMemory(blind, 32);
```

### 2. Fee Calculation

**Problem:** Fees must be transparent (miners need to see them)

**Solution:** Require at least one transparent output, or:
- Explicit fee output (transparent)
- Fee range proof (proves fee is in expected range)

### 3. Amount Overflow

**Phase A alone is INSECURE:**
- Attacker can create negative amounts: -100 + 100 = 0 ✅ (balances!)
- Solution: Phase B range proofs prevent this

**Never deploy Phase A to production without Phase B!**

---

## Integration Points

**Files that need changes:**
1. `/Users/haydarevich/Documents/DineroCoin/include/wallet/transaction.h` - Extend TxOutput
2. `/Users/haydarevich/Documents/DineroCoin/src/wallet/transaction.cpp` - Serialization
3. `/Users/haydarevich/Documents/DineroCoin/src/consensus/tx_validation.cpp` - Add CheckConfidentialTransaction()
4. `/Users/haydarevich/Documents/DineroCoin/src/wallet/wallet_db.cpp` - Store blinding factors
5. `/Users/haydarevich/Documents/DineroCoin/src/zk/confidential_tx.cpp` - ZK library (user implementing)
6. `/Users/haydarevich/Documents/DineroCoin/src/rpc/zk_rpc_handlers_context.cpp` - RPC methods (skeleton ready)

**DO NOT modify these files yet** - User is working on Phase A core library first.

---

## Summary

**Current status:** Design document only
**User's focus:** Implementing Phase A core library (Pedersen commitments)
**When to implement:** After Phase A library is complete and tested

**Next steps (when user completes Phase A):**
1. Extend `TxOutput` structure as designed above
2. Implement serialization with 0xFF marker format
3. Add confidential transaction validation
4. Integrate with RPC methods (skeleton already created)

This design ensures backward compatibility while adding powerful privacy features!
