# ZK Privacy Testing Strategy

**Status:** Testing guide - for when Phase A implementation is complete
**Purpose:** Comprehensive test plan to verify Pedersen commitments and range proofs

---

## Test Hierarchy

```
Unit Tests (Fast, isolated)
├── Phase A: Pedersen Commitments
│   ├── Commitment creation
│   ├── Commitment serialization
│   ├── Balance verification
│   └── Blinding factor arithmetic
│
├── Phase B: Range Proofs
│   ├── Proof generation
│   ├── Proof verification
│   ├── Proof rewinding (view keys)
│   └── Aggregated proofs
│
Integration Tests (Medium, cross-component)
├── Transaction creation (transparent → confidential)
├── Transaction validation (consensus rules)
├── Wallet integration (blinding factor storage)
└── RPC methods (zk.createtx, zk.scanviewkey)
│
End-to-End Tests (Slow, full system)
├── Confidential payment flow
├── Mixed transparent/confidential TXs
└── Multi-hop confidential transactions
```

---

## Phase A: Pedersen Commitment Tests

### Test 1: Basic Commitment Creation

**File:** `tests/test_zk_commitments.cpp`

```cpp
#include <gtest/gtest.h>
#include <secp256k1.h>
#include <secp256k1_generator.h>
#include "zk/confidential_tx.h"

TEST(PedersenCommitments, CreateCommitment) {
    // Setup
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Generate random blinding factor
    unsigned char blind[32];
    ASSERT_TRUE(secp256k1_rand256(blind));

    // Create commitment for 1.0 DINERO (100,000,000 una)
    uint64_t amount = 100000000;
    secp256k1_pedersen_commitment commit;

    ASSERT_TRUE(secp256k1_pedersen_commit(
        ctx, &commit, blind, amount, secp256k1_generator_h));

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Commitment created successfully

### Test 2: Commitment Serialization

```cpp
TEST(PedersenCommitments, SerializeDeserialize) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Create commitment
    unsigned char blind[32];
    secp256k1_rand256(blind);
    uint64_t amount = 50000000;
    secp256k1_pedersen_commitment commit;
    secp256k1_pedersen_commit(ctx, &commit, blind, amount, secp256k1_generator_h);

    // Serialize to 33 bytes
    unsigned char serialized[33];
    ASSERT_TRUE(secp256k1_pedersen_commitment_serialize(ctx, serialized, &commit));

    // Deserialize back
    secp256k1_pedersen_commitment loaded;
    ASSERT_TRUE(secp256k1_pedersen_commitment_parse(ctx, &loaded, serialized));

    // Verify they match (serialize again and compare)
    unsigned char serialized2[33];
    secp256k1_pedersen_commitment_serialize(ctx, serialized2, &loaded);
    ASSERT_EQ(0, memcmp(serialized, serialized2, 33));

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Round-trip serialization works

### Test 3: Balance Verification (Valid)

```cpp
TEST(PedersenCommitments, VerifyBalanceValid) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Transaction: 1.0 DINERO input → 0.6 DINERO + 0.4 DINERO outputs
    // Inputs: 100,000,000 una
    // Outputs: 60,000,000 + 40,000,000 una

    // Generate blinding factors
    unsigned char blind_in[32], blind_out1[32], blind_out2[32];
    secp256k1_rand256(blind_in);
    secp256k1_rand256(blind_out1);

    // CRITICAL: Balance blinding factors (sum must equal zero)
    // blind_in = blind_out1 + blind_out2
    // Therefore: blind_out2 = blind_in - blind_out1
    unsigned char blind_out2_neg[32];
    memcpy(blind_out2_neg, blind_out1, 32);
    secp256k1_pedersen_blind_sum(ctx, blind_out2, (const unsigned char**)&blind_in, 1,
                                 (const unsigned char**)&blind_out2_neg, 1);

    // Create commitments
    secp256k1_pedersen_commitment commit_in, commit_out1, commit_out2;
    secp256k1_pedersen_commit(ctx, &commit_in, blind_in, 100000000, secp256k1_generator_h);
    secp256k1_pedersen_commit(ctx, &commit_out1, blind_out1, 60000000, secp256k1_generator_h);
    secp256k1_pedersen_commit(ctx, &commit_out2, blind_out2, 40000000, secp256k1_generator_h);

    // Verify balance
    const secp256k1_pedersen_commitment* inputs[] = {&commit_in};
    const secp256k1_pedersen_commitment* outputs[] = {&commit_out1, &commit_out2};

    ASSERT_TRUE(secp256k1_pedersen_verify_tally(ctx, inputs, 1, outputs, 2));

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Balance verification succeeds

### Test 4: Balance Verification (Invalid)

```cpp
TEST(PedersenCommitments, VerifyBalanceInvalid) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Transaction: 1.0 DINERO input → 0.6 DINERO + 0.5 DINERO outputs
    // INVALID: 1.0 ≠ 1.1

    unsigned char blind_in[32], blind_out1[32], blind_out2[32];
    secp256k1_rand256(blind_in);
    secp256k1_rand256(blind_out1);
    secp256k1_rand256(blind_out2);

    secp256k1_pedersen_commitment commit_in, commit_out1, commit_out2;
    secp256k1_pedersen_commit(ctx, &commit_in, blind_in, 100000000, secp256k1_generator_h);
    secp256k1_pedersen_commit(ctx, &commit_out1, blind_out1, 60000000, secp256k1_generator_h);
    secp256k1_pedersen_commit(ctx, &commit_out2, blind_out2, 50000000, secp256k1_generator_h);

    const secp256k1_pedersen_commitment* inputs[] = {&commit_in};
    const secp256k1_pedersen_commitment* outputs[] = {&commit_out1, &commit_out2};

    ASSERT_FALSE(secp256k1_pedersen_verify_tally(ctx, inputs, 1, outputs, 2));

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Balance verification fails (as expected)

### Test 5: Negative Amount Attack (Phase A Vulnerability)

```cpp
TEST(PedersenCommitments, NegativeAmountAttack) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // ATTACK: Create negative amounts that balance
    // Input: 100 DINERO
    // Output 1: -50 DINERO (hidden as 2^64 - 50)
    // Output 2: 150 DINERO
    // Balance: 100 = -50 + 150 ✅ (but INVALID!)

    unsigned char blind_in[32], blind_out1[32], blind_out2[32];
    secp256k1_rand256(blind_in);
    secp256k1_rand256(blind_out1);

    // Balance blinding factors
    unsigned char blind_out1_neg[32];
    memcpy(blind_out1_neg, blind_out1, 32);
    secp256k1_pedersen_blind_sum(ctx, blind_out2, (const unsigned char**)&blind_in, 1,
                                 (const unsigned char**)&blind_out1_neg, 1);

    secp256k1_pedersen_commitment commit_in, commit_out1, commit_out2;
    secp256k1_pedersen_commit(ctx, &commit_in, blind_in, 100, secp256k1_generator_h);

    // Negative amount: -50 represented as 2^64 - 50
    uint64_t negative_50 = UINT64_MAX - 50 + 1;
    secp256k1_pedersen_commit(ctx, &commit_out1, blind_out1, negative_50, secp256k1_generator_h);
    secp256k1_pedersen_commit(ctx, &commit_out2, blind_out2, 150, secp256k1_generator_h);

    const secp256k1_pedersen_commitment* inputs[] = {&commit_in};
    const secp256k1_pedersen_commitment* outputs[] = {&commit_out1, &commit_out2};

    // WARNING: This will PASS balance verification!
    // Phase A alone cannot detect negative amounts!
    ASSERT_TRUE(secp256k1_pedersen_verify_tally(ctx, inputs, 1, outputs, 2));

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ⚠️  Test passes (demonstrates Phase A vulnerability!)
**Fix:** Phase B range proofs will prevent this

---

## Phase B: Range Proof Tests

### Test 6: Basic Range Proof Generation

**File:** `tests/test_zk_rangeproofs.cpp`

```cpp
#include <gtest/gtest.h>
#include <secp256k1.h>
#include <secp256k1_rangeproof.h>
#include "zk/confidential_tx.h"

TEST(RangeProofs, GenerateProof) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Create commitment
    unsigned char blind[32], nonce[32];
    secp256k1_rand256(blind);
    secp256k1_rand256(nonce);
    uint64_t amount = 50000000;

    secp256k1_pedersen_commitment commit;
    secp256k1_pedersen_commit(ctx, &commit, blind, amount, secp256k1_generator_h);

    // Generate range proof
    unsigned char proof[5134];
    size_t proof_len = 5134;

    ASSERT_TRUE(secp256k1_rangeproof_sign(
        ctx,
        proof, &proof_len,
        0,  // min_value = 0
        &commit,
        blind,
        nonce,
        0,  // exp = 0 (most private)
        0,  // min_bits = 0 (auto)
        amount,
        NULL, 0,  // No message
        NULL, 0,  // No extra commit
        secp256k1_generator_h
    ));

    // Verify proof size is reasonable
    ASSERT_LE(proof_len, 5134);  // Should be ≤ 5134 bytes
    ASSERT_GE(proof_len, 450);   // Should be ≥ 450 bytes (optimized)

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Range proof generated

### Test 7: Range Proof Verification

```cpp
TEST(RangeProofs, VerifyProof) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Create commitment and proof
    unsigned char blind[32], nonce[32];
    secp256k1_rand256(blind);
    secp256k1_rand256(nonce);
    uint64_t amount = 75000000;

    secp256k1_pedersen_commitment commit;
    secp256k1_pedersen_commit(ctx, &commit, blind, amount, secp256k1_generator_h);

    unsigned char proof[5134];
    size_t proof_len = 5134;
    secp256k1_rangeproof_sign(ctx, proof, &proof_len, 0, &commit, blind, nonce,
                              0, 0, amount, NULL, 0, NULL, 0, secp256k1_generator_h);

    // Verify proof (validator doesn't know amount!)
    uint64_t min_value, max_value;
    ASSERT_TRUE(secp256k1_rangeproof_verify(
        ctx, &min_value, &max_value, &commit, proof, proof_len,
        NULL, 0, secp256k1_generator_h
    ));

    // Verify range is [0, 2^64)
    ASSERT_EQ(0, min_value);
    ASSERT_GT(max_value, amount);  // Max should be > actual amount

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Range proof verification succeeds

### Test 8: Negative Amount Prevention

```cpp
TEST(RangeProofs, PreventNegativeAmount) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Try to create proof for negative amount
    unsigned char blind[32], nonce[32];
    secp256k1_rand256(blind);
    secp256k1_rand256(nonce);

    // Negative amount: -100 represented as 2^64 - 100
    uint64_t negative_amount = UINT64_MAX - 100 + 1;

    secp256k1_pedersen_commitment commit;
    secp256k1_pedersen_commit(ctx, &commit, blind, negative_amount, secp256k1_generator_h);

    // Try to generate range proof (this should FAIL or produce invalid proof)
    unsigned char proof[5134];
    size_t proof_len = 5134;

    // Proof generation might succeed, but verification will fail
    bool sign_ok = secp256k1_rangeproof_sign(
        ctx, proof, &proof_len, 0, &commit, blind, nonce,
        0, 0, negative_amount, NULL, 0, NULL, 0, secp256k1_generator_h
    );

    if (sign_ok) {
        // If signing succeeded, verification should fail
        uint64_t min_value, max_value;
        ASSERT_FALSE(secp256k1_rangeproof_verify(
            ctx, &min_value, &max_value, &commit, proof, proof_len,
            NULL, 0, secp256k1_generator_h
        ));
    }

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Negative amounts are prevented

### Test 9: Proof Rewinding (View Key)

```cpp
TEST(RangeProofs, RewindProof) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Sender creates proof
    unsigned char blind[32], nonce[32];
    secp256k1_rand256(blind);
    secp256k1_rand256(nonce);
    uint64_t amount = 12345678;

    secp256k1_pedersen_commitment commit;
    secp256k1_pedersen_commit(ctx, &commit, blind, amount, secp256k1_generator_h);

    unsigned char proof[5134];
    size_t proof_len = 5134;
    secp256k1_rangeproof_sign(ctx, proof, &proof_len, 0, &commit, blind, nonce,
                              0, 0, amount, NULL, 0, NULL, 0, secp256k1_generator_h);

    // Receiver rewinds proof with nonce (view key)
    uint64_t min_value, max_value, recovered_value;
    unsigned char recovered_blind[32];
    unsigned char message[4096];
    size_t message_len = 4096;

    ASSERT_TRUE(secp256k1_rangeproof_rewind(
        ctx, recovered_blind, &recovered_value,
        message, &message_len,
        nonce,  // View key!
        &min_value, &max_value,
        &commit, proof, proof_len,
        NULL, 0, secp256k1_generator_h
    ));

    // Verify recovered amount matches original
    ASSERT_EQ(amount, recovered_value);

    // Verify recovered blinding factor matches
    ASSERT_EQ(0, memcmp(blind, recovered_blind, 32));

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ Receiver can extract amount with view key

---

## Integration Tests

### Test 10: Confidential Transaction Creation

**File:** `tests/test_zk_integration.cpp`

```cpp
TEST(ConfidentialTX, CreateTransaction) {
    // Create a confidential transaction
    // Input: 1.0 DINERO (transparent UTXO)
    // Output 1: 0.6 DINERO (confidential)
    // Output 2: 0.39 DINERO (confidential change)
    // Fee: 0.01 DINERO (transparent)

    ConfidentialTransactionBuilder builder;

    // Add transparent input
    builder.addInput("abc123...", 0, 100000000);

    // Add confidential outputs
    builder.addConfidentialOutput("dinero1q...", 60000000);  // Recipient
    builder.addConfidentialOutput("dinero1q...", 39000000);  // Change

    // Set fee
    builder.setFee(1000000);

    // Build transaction
    Transaction tx;
    std::vector<ConfidentialOutputMetadata> metadata;
    ASSERT_TRUE(builder.build(tx, metadata));

    // Verify outputs are confidential
    ASSERT_TRUE(tx.vout[0].is_confidential);
    ASSERT_TRUE(tx.vout[1].is_confidential);

    // Verify range proofs exist
    ASSERT_FALSE(tx.vout[0].range_proof.empty());
    ASSERT_FALSE(tx.vout[1].range_proof.empty());

    // Verify balance
    ASSERT_TRUE(VerifyConfidentialBalance(tx));
}
```

### Test 11: RPC Method - zk.createtx

```cpp
TEST(ZkRPC, CreateConfidentialTx) {
    // Set up test context
    ExecutionContext ctx;

    // Build RPC request
    din::Json params;
    params["inputs"] = din::Json::array();
    params["inputs"][0]["txid"] = "abc123...";
    params["inputs"][0]["vout"] = 0;
    params["inputs"][0]["amount"] = 100000000;
    params["inputs"][0]["blinding_factor"] = "hex...";

    params["outputs"] = din::Json::array();
    params["outputs"][0]["address"] = "dinero1q...";
    params["outputs"][0]["amount"] = 50000000;
    params["outputs"][0]["confidential"] = true;

    params["fee"] = 10000;

    // Call RPC method
    auto result = rpc_context_zk_createtx(ctx, params);

    // Verify response
    ASSERT_TRUE(result.contains("hex"));
    ASSERT_TRUE(result.contains("txid"));
    ASSERT_TRUE(result.contains("commitments"));
    ASSERT_TRUE(result["verify"]["balance"]);
    ASSERT_TRUE(result["verify"]["range_proofs"]);
}
```

---

## Performance Benchmarks

### Benchmark 1: Commitment Creation Speed

```cpp
TEST(Performance, CommitmentCreation) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    const int iterations = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        unsigned char blind[32];
        secp256k1_rand256(blind);
        secp256k1_pedersen_commitment commit;
        secp256k1_pedersen_commit(ctx, &commit, blind, 100000000, secp256k1_generator_h);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    double avg_us = duration.count() / (double)iterations;
    std::cout << "Average commitment creation: " << avg_us << " µs" << std::endl;

    // Should be < 100 µs per commitment
    ASSERT_LT(avg_us, 100.0);

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ < 100 µs per commitment (~10,000 commitments/second)

### Benchmark 2: Range Proof Generation Speed

```cpp
TEST(Performance, RangeProofGeneration) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    const int iterations = 100;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        unsigned char blind[32], nonce[32];
        secp256k1_rand256(blind);
        secp256k1_rand256(nonce);

        secp256k1_pedersen_commitment commit;
        secp256k1_pedersen_commit(ctx, &commit, blind, 100000000, secp256k1_generator_h);

        unsigned char proof[5134];
        size_t proof_len = 5134;
        secp256k1_rangeproof_sign(ctx, proof, &proof_len, 0, &commit, blind, nonce,
                                  0, 0, 100000000, NULL, 0, NULL, 0, secp256k1_generator_h);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double avg_ms = duration.count() / (double)iterations;
    std::cout << "Average range proof generation: " << avg_ms << " ms" << std::endl;

    // Should be < 50 ms per proof
    ASSERT_LT(avg_ms, 50.0);

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ < 50 ms per proof (~20 proofs/second)

### Benchmark 3: Range Proof Verification Speed

```cpp
TEST(Performance, RangeProofVerification) {
    secp256k1_context* ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);

    // Pre-generate proofs
    const int iterations = 100;
    std::vector<std::vector<uint8_t>> proofs;
    std::vector<secp256k1_pedersen_commitment> commits;

    for (int i = 0; i < iterations; i++) {
        unsigned char blind[32], nonce[32];
        secp256k1_rand256(blind);
        secp256k1_rand256(nonce);

        secp256k1_pedersen_commitment commit;
        secp256k1_pedersen_commit(ctx, &commit, blind, 100000000, secp256k1_generator_h);
        commits.push_back(commit);

        unsigned char proof[5134];
        size_t proof_len = 5134;
        secp256k1_rangeproof_sign(ctx, proof, &proof_len, 0, &commit, blind, nonce,
                                  0, 0, 100000000, NULL, 0, NULL, 0, secp256k1_generator_h);
        proofs.push_back(std::vector<uint8_t>(proof, proof + proof_len));
    }

    // Benchmark verification
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++) {
        uint64_t min_value, max_value;
        secp256k1_rangeproof_verify(ctx, &min_value, &max_value, &commits[i],
                                    proofs[i].data(), proofs[i].size(),
                                    NULL, 0, secp256k1_generator_h);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    double avg_ms = duration.count() / (double)iterations;
    std::cout << "Average range proof verification: " << avg_ms << " ms" << std::endl;

    // Should be < 10 ms per proof
    ASSERT_LT(avg_ms, 10.0);

    secp256k1_context_destroy(ctx);
}
```

**Expected:** ✅ < 10 ms per proof (~100 proofs/second)

---

## Running the Tests

### Build Tests

```bash
cd /Users/haydarevich/Documents/DineroCoin/build
cmake --build . --target test_zk_commitments
cmake --build . --target test_zk_rangeproofs
cmake --build . --target test_zk_integration
```

### Run All ZK Tests

```bash
# Run all tests
./test_zk_commitments
./test_zk_rangeproofs
./test_zk_integration

# Run specific test
./test_zk_commitments --gtest_filter=PedersenCommitments.CreateCommitment

# Run with verbose output
./test_zk_commitments --gtest_verbose
```

### Expected Output

```
[==========] Running 11 tests from 3 test suites.
[----------] Global test environment set-up.
[----------] 5 tests from PedersenCommitments
[ RUN      ] PedersenCommitments.CreateCommitment
[       OK ] PedersenCommitments.CreateCommitment (2 ms)
[ RUN      ] PedersenCommitments.SerializeDeserialize
[       OK ] PedersenCommitments.SerializeDeserialize (1 ms)
[ RUN      ] PedersenCommitments.VerifyBalanceValid
[       OK ] PedersenCommitments.VerifyBalanceValid (3 ms)
[ RUN      ] PedersenCommitments.VerifyBalanceInvalid
[       OK ] PedersenCommitments.VerifyBalanceInvalid (2 ms)
[ RUN      ] PedersenCommitments.NegativeAmountAttack
[       OK ] PedersenCommitments.NegativeAmountAttack (2 ms)
[----------] 5 tests from PedersenCommitments (10 ms total)

[----------] 4 tests from RangeProofs
[ RUN      ] RangeProofs.GenerateProof
[       OK ] RangeProofs.GenerateProof (45 ms)
[ RUN      ] RangeProofs.VerifyProof
[       OK ] RangeProofs.VerifyProof (50 ms)
[ RUN      ] RangeProofs.PreventNegativeAmount
[       OK ] RangeProofs.PreventNegativeAmount (48 ms)
[ RUN      ] RangeProofs.RewindProof
[       OK ] RangeProofs.RewindProof (52 ms)
[----------] 4 tests from RangeProofs (195 ms total)

[----------] 2 tests from Performance
[ RUN      ] Performance.CommitmentCreation
Average commitment creation: 45.3 µs
[       OK ] Performance.CommitmentCreation (453 ms)
[ RUN      ] Performance.RangeProofGeneration
Average range proof generation: 42.1 ms
[       OK ] Performance.RangeProofGeneration (4210 ms)
[----------] 2 tests from Performance (4663 ms total)

[==========] 11 tests from 3 test suites ran. (4868 ms total)
[  PASSED  ] 11 tests.
```

---

## Test Coverage Goals

**Phase A (Pedersen Commitments):**
- ✅ Unit tests: 100% coverage of commitment creation/verification
- ✅ Integration tests: Transaction validation with commitments
- ✅ Performance: < 100 µs per commitment

**Phase B (Range Proofs):**
- ✅ Unit tests: 100% coverage of proof generation/verification
- ✅ Integration tests: Full confidential transaction flow
- ✅ Performance: < 50 ms generation, < 10 ms verification

**Phase C (View Keys):**
- ✅ Unit tests: Proof rewinding with nonce
- ✅ Integration tests: RPC `zk.scanviewkey`
- ✅ End-to-end: Multi-party confidential payments

---

## Summary

**When Phase A complete:**
- Run Tests 1-5 to verify Pedersen commitments
- Expect Test 5 (NegativeAmountAttack) to PASS (demonstrates vulnerability)

**When Phase B complete:**
- Run Tests 6-9 to verify range proofs
- Expect Test 8 (PreventNegativeAmount) to PASS (vulnerability fixed!)
- Run all integration tests

**When Phase C complete:**
- Run all tests including view key scanning
- Performance benchmarks should all pass

This comprehensive test suite ensures ZK privacy is implemented correctly and securely!
