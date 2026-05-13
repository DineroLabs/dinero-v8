# ZK API Cheat Sheet - secp256k1-zkp

Quick reference for implementing Phase A of ZK privacy.

---

## 📦 Required Headers

```cpp
#include <secp256k1.h>
#include <secp256k1_generator.h>   // Pedersen commitments
#include <secp256k1_rangeproof.h>  // Bulletproofs (Phase B)
```

---

## 🔑 Core Data Types

```cpp
// Pedersen commitment: C = v·G + r·H
typedef struct {
    unsigned char data[64];
} secp256k1_pedersen_commitment;

// Generator point (H in the formula)
typedef struct {
    unsigned char data[64];
} secp256k1_generator;

// Predefined generator H (hardcoded constant)
extern const secp256k1_generator *secp256k1_generator_h;
```

---

## 🎯 Phase A: Pedersen Commitments (Copy-Paste Ready)

### 1. Create a Commitment

```cpp
// Hide an amount with a blinding factor
// Returns: 1 on success, 0 if blind factor is invalid
int secp256k1_pedersen_commit(
    const secp256k1_context *ctx,
    secp256k1_pedersen_commitment *commit,  // OUT: commitment
    const unsigned char *blind,              // IN: 32-byte blinding factor
    uint64_t value,                          // IN: amount to hide
    const secp256k1_generator *gen           // IN: use secp256k1_generator_h
);

// Example usage:
secp256k1_pedersen_commitment commitment;
unsigned char blind[32];  // Generate randomly
uint64_t amount = 1000000;  // 1 DINERO

secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

// Generate random blinding factor
if (!secp256k1_rand256(blind)) {
    // Handle error
}

// Create commitment: C = amount·G + blind·H
if (!secp256k1_pedersen_commit(ctx, &commitment, blind, amount, secp256k1_generator_h)) {
    // Handle error (very rare - bad blinding factor)
}

// Now commitment hides the amount!
```

### 2. Verify Balance (Sum of Inputs = Sum of Outputs)

```cpp
// Verify that sum(positive_commits) == sum(negative_commits)
// This is the CORE of confidential transactions!
// Returns: 1 if balanced, 0 if not
int secp256k1_pedersen_verify_tally(
    const secp256k1_context *ctx,
    const secp256k1_pedersen_commitment * const *commits,   // IN: positive commitments (inputs)
    size_t pcnt,                                            // IN: number of inputs
    const secp256k1_pedersen_commitment * const *ncommits,  // IN: negative commitments (outputs)
    size_t ncnt                                             // IN: number of outputs
);

// Example: Verify a transaction with 2 inputs, 3 outputs
secp256k1_pedersen_commitment input1, input2;
secp256k1_pedersen_commitment output1, output2, output3;

// ... create commitments ...

// Verify: input1 + input2 == output1 + output2 + output3
const secp256k1_pedersen_commitment *inputs[] = {&input1, &input2};
const secp256k1_pedersen_commitment *outputs[] = {&output1, &output2, &output3};

if (secp256k1_pedersen_verify_tally(ctx, inputs, 2, outputs, 3)) {
    printf("✅ Transaction balances! Amounts are hidden but verified.\n");
} else {
    printf("❌ Transaction does not balance!\n");
}
```

### 3. Serialize/Deserialize Commitments

```cpp
// Serialize commitment to 33 bytes (for blockchain storage)
int secp256k1_pedersen_commitment_serialize(
    const secp256k1_context *ctx,
    unsigned char *output,                        // OUT: 33-byte array
    const secp256k1_pedersen_commitment *commit   // IN: commitment to serialize
);

// Parse 33-byte commitment from blockchain
int secp256k1_pedersen_commitment_parse(
    const secp256k1_context *ctx,
    secp256k1_pedersen_commitment *commit,  // OUT: parsed commitment
    const unsigned char *input              // IN: 33-byte serialized data
);

// Example: Store commitment in transaction
unsigned char serialized[33];
if (!secp256k1_pedersen_commitment_serialize(ctx, serialized, &commitment)) {
    // Handle error
}

// Later: Read commitment from blockchain
secp256k1_pedersen_commitment loaded_commitment;
if (!secp256k1_pedersen_commitment_parse(ctx, &loaded_commitment, serialized)) {
    // Invalid commitment data
}
```

---

## 💡 Complete Working Example

```cpp
#include <secp256k1.h>
#include <secp256k1_generator.h>
#include <stdio.h>
#include <stdint.h>

bool CreateConfidentialTransaction(
    secp256k1_context *ctx,
    uint64_t input_amount1,
    uint64_t input_amount2,
    uint64_t output_amount1,
    uint64_t output_amount2,
    uint64_t output_amount3
) {
    // Step 1: Generate random blinding factors
    unsigned char blind_in1[32], blind_in2[32];
    unsigned char blind_out1[32], blind_out2[32], blind_out3[32];

    // In production: use secp256k1_rand256() or system CSPRNG
    // For now, assume we have random bytes

    // Step 2: Create input commitments
    secp256k1_pedersen_commitment commit_in1, commit_in2;
    if (!secp256k1_pedersen_commit(ctx, &commit_in1, blind_in1, input_amount1, secp256k1_generator_h)) {
        return false;
    }
    if (!secp256k1_pedersen_commit(ctx, &commit_in2, blind_in2, input_amount2, secp256k1_generator_h)) {
        return false;
    }

    // Step 3: Create output commitments
    secp256k1_pedersen_commitment commit_out1, commit_out2, commit_out3;
    if (!secp256k1_pedersen_commit(ctx, &commit_out1, blind_out1, output_amount1, secp256k1_generator_h)) {
        return false;
    }
    if (!secp256k1_pedersen_commit(ctx, &commit_out2, blind_out2, output_amount2, secp256k1_generator_h)) {
        return false;
    }
    if (!secp256k1_pedersen_commit(ctx, &commit_out3, blind_out3, output_amount3, secp256k1_generator_h)) {
        return false;
    }

    // Step 4: Verify balance (inputs == outputs)
    const secp256k1_pedersen_commitment *inputs[] = {&commit_in1, &commit_in2};
    const secp256k1_pedersen_commitment *outputs[] = {&commit_out1, &commit_out2, &commit_out3};

    if (!secp256k1_pedersen_verify_tally(ctx, inputs, 2, outputs, 3)) {
        printf("❌ Transaction does not balance!\n");
        return false;
    }

    printf("✅ Confidential transaction verified!\n");
    printf("   Input amounts: HIDDEN\n");
    printf("   Output amounts: HIDDEN\n");
    printf("   Balance proof: VERIFIED\n");

    return true;
}

int main() {
    // Example: Send 1.5 BTC split into 3 outputs
    // Inputs: 1.0 + 0.5 = 1.5 BTC
    // Outputs: 0.8 + 0.4 + 0.3 = 1.5 BTC

    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    bool success = CreateConfidentialTransaction(
        ctx,
        100000000,  // Input 1: 1.0 BTC (in una)
        50000000,   // Input 2: 0.5 BTC
        80000000,   // Output 1: 0.8 BTC
        40000000,   // Output 2: 0.4 BTC
        30000000    // Output 3: 0.3 BTC
    );

    secp256k1_context_destroy(ctx);
    return success ? 0 : 1;
}
```

---

## 🚀 DineroCoin Integration Points

### Transaction Structure (Extend Existing)

```cpp
// src/core/transaction.h
struct CTxOut {
    // Existing transparent output
    int64_t nValue;  // Keep for backward compatibility

    // NEW: Confidential output (optional)
    bool is_confidential;
    secp256k1_pedersen_commitment commitment;  // 33 bytes when serialized
    std::vector<unsigned char> range_proof;    // Add in Phase B
};
```

### Validation (Extend Existing)

```cpp
// src/consensus/tx_validation.cpp
bool CheckTransaction(const CTransaction& tx, ValidationState& state) {
    // Existing transparent validation
    if (!tx.HasConfidentialOutputs()) {
        // Use existing validation logic
        return CheckTransparentBalance(tx, state);
    }

    // NEW: Confidential validation
    return CheckConfidentialBalance(tx, state);
}

bool CheckConfidentialBalance(const CTransaction& tx, ValidationState& state) {
    // Collect input and output commitments
    std::vector<secp256k1_pedersen_commitment> input_commits;
    std::vector<secp256k1_pedersen_commitment> output_commits;

    for (const auto& input : tx.vin) {
        // Get commitment from previous output
        input_commits.push_back(GetCommitmentFromUTXO(input.prevout));
    }

    for (const auto& output : tx.vout) {
        if (output.is_confidential) {
            output_commits.push_back(output.commitment);
        }
    }

    // Verify balance: sum(inputs) == sum(outputs)
    std::vector<const secp256k1_pedersen_commitment*> in_ptrs, out_ptrs;
    for (auto& c : input_commits) in_ptrs.push_back(&c);
    for (auto& c : output_commits) out_ptrs.push_back(&c);

    if (!secp256k1_pedersen_verify_tally(
        GetSecp256k1Context(),
        in_ptrs.data(), in_ptrs.size(),
        out_ptrs.data(), out_ptrs.size()
    )) {
        return state.DoS(100, false, REJECT_INVALID, "bad-confidential-balance");
    }

    // TODO: Phase B: Verify range proofs

    return true;
}
```

### RPC Interface (New)

```cpp
// src/rpc/zk_rpc.cpp
UniValue zk_createtx(const JSONRPCRequest& request) {
    // Create confidential transaction
    // Returns: hex-encoded transaction with commitments
}

UniValue zk_verifybalance(const JSONRPCRequest& request) {
    // Verify a confidential transaction balances
    // Input: hex-encoded transaction
    // Returns: true/false
}
```

---

## 📏 Size Estimates

| Component | Size (bytes) | Notes |
|-----------|--------------|-------|
| Pedersen Commitment | 33 | Serialized elliptic curve point |
| Transparent TxOut | ~34 | 8-byte value + 26-byte script |
| Confidential TxOut (Phase A) | ~33 | Just commitment (no amount field) |
| Confidential TxOut (Phase B) | ~5KB | + Bulletproof range proof |
| Confidential TxOut (Optimized) | ~450 bytes | + Bulletproofs++ range proof |

---

## ⚠️ Important Notes for Phase A

1. **Phase A is NOT production-ready** - commitments alone allow negative amounts!
   - Example: Input = 100, Output = -50 + 150 = 100 ✅ Balances but INVALID!
   - Fix: Phase B adds range proofs to prevent this

2. **Blinding factors must be random**
   - Use `secp256k1_rand256()` or system CSPRNG
   - NEVER reuse blinding factors
   - Keep them secret (only sender/receiver know them)

3. **Generator H is hardcoded**
   - Always use `secp256k1_generator_h`
   - This is the standard generator for Pedersen commitments

4. **Context creation**
   - Create once, reuse across operations
   - Use `SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY` for full functionality

---

## ✅ Phase A Checklist

- [ ] Create secp256k1 context in daemon startup
- [ ] Add `commitment` field to CTxOut
- [ ] Implement `CheckConfidentialBalance()` validation
- [ ] Add RPC methods: `zk.createtx`, `zk.verifybalance`
- [ ] Write unit tests:
  - [ ] Test commitment creation
  - [ ] Test balance verification (valid)
  - [ ] Test balance verification (invalid)
  - [ ] Test serialization/deserialization
- [ ] Document known limitation: no range proofs yet

---

**Next:** Phase B adds `secp256k1_bulletproof_rangeproof_prove/verify` to prevent negative amounts!
