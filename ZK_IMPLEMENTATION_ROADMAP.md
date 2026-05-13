# ZK Privacy Implementation Roadmap

**Status:** Complete preparation - all documentation ready
**User Focus:** Implementing Phase A (Pedersen commitments)

---

## Documentation Index

All preparation work has been completed. Here's your complete documentation package:

| Document | Purpose | Status |
|----------|---------|--------|
| **ZK_LIBRARIES_STATUS.md** | Library inventory (all vendored!) | ✅ Complete |
| **ZK_API_CHEATSHEET.md** | Phase A API reference | ✅ Complete |
| **PHASE_B_RANGE_PROOFS.md** | Phase B implementation guide | ✅ Complete |
| **ZK_RPC_IMPLEMENTATION_GUIDE.md** | RPC usage guide | ✅ Complete |
| **ZK_RPC_INTEGRATION.md** | Integration steps (exact lines!) | ✅ Complete |
| **ZK_TRANSACTION_STRUCTURE.md** | TxOutput extension design | ✅ Complete |
| **ZK_TESTING_STRATEGY.md** | Comprehensive test plan | ✅ Complete |
| **ZK_IMPLEMENTATION_ROADMAP.md** | This file (summary) | ✅ Complete |

---

## Quick Start Guide

### Phase A: Pedersen Commitments (YOU ARE HERE)

**Goal:** Hide transaction amounts using Pedersen commitments

**Implementation file:** `/Users/haydarevich/Documents/DineroCoin/src/zk/confidential_tx.cpp`

**Key APIs (copy-paste ready in ZK_API_CHEATSHEET.md):**
```cpp
// Create commitment: C = amount·G + blind·H
secp256k1_pedersen_commit(ctx, &commit, blind, amount, secp256k1_generator_h);

// Verify balance: sum(inputs) == sum(outputs)
secp256k1_pedersen_verify_tally(ctx, inputs, input_count, outputs, output_count);

// Serialize to 33 bytes for blockchain storage
secp256k1_pedersen_commitment_serialize(ctx, serialized, &commit);
```

**Testing:**
- See `ZK_TESTING_STRATEGY.md` - Tests 1-5 for Phase A
- Run: `./test_zk_commitments`

**When complete:**
- ✅ Commitments create and verify correctly
- ⚠️  Test 5 (NegativeAmountAttack) will PASS - this is expected!
- 🚀 Ready to move to Phase B

---

### Phase B: Range Proofs (NEXT)

**Goal:** Prevent negative amounts using Bulletproof range proofs

**Implementation file:** Same (`src/zk/confidential_tx.cpp`)

**Key APIs (copy-paste ready in PHASE_B_RANGE_PROOFS.md):**
```cpp
// Generate range proof (prove 0 ≤ value < 2^64)
secp256k1_rangeproof_sign(ctx, proof, &proof_len, 0, &commit, blind, nonce,
                          0, 0, amount, NULL, 0, NULL, 0, secp256k1_generator_h);

// Verify range proof (validator doesn't learn amount)
secp256k1_rangeproof_verify(ctx, &min_value, &max_value, &commit,
                            proof, proof_len, NULL, 0, secp256k1_generator_h);

// Rewind proof with view key (receiver extracts amount)
secp256k1_rangeproof_rewind(ctx, recovered_blind, &recovered_value,
                            message, &message_len, nonce, &min_val, &max_val,
                            &commit, proof, proof_len, NULL, 0, secp256k1_generator_h);
```

**Testing:**
- See `ZK_TESTING_STRATEGY.md` - Tests 6-9 for Phase B
- Run: `./test_zk_rangeproofs`

**When complete:**
- ✅ Range proofs generate and verify correctly
- ✅ Negative amounts are prevented (Test 8 passes!)
- 🚀 Ready to integrate RPC methods

---

### RPC Integration (AFTER Phase B)

**Goal:** Expose ZK features to users via RPC

**Implementation steps (exact lines in ZK_RPC_INTEGRATION.md):**

1. **Add to CMakeLists.txt** (line 442):
   ```cmake
   src/rpc/zk_rpc_handlers_context.cpp  # zk.createtx, zk.verify, zk.scanviewkey
   ```

2. **Register in rpc_context_wiring.cpp** (lines 33 & 209):
   ```cpp
   void WireZkRpcContext();  // Forward declaration

   WireZkRpcContext();  // Register methods
   dinero::g_logger.info("[RPC Context] ✅ ZK privacy context-aware handlers registered");
   ```

3. **Implement RPC method bodies** in `src/rpc/zk_rpc_handlers_context.cpp`:
   - Replace `throw std::runtime_error(...)` with actual implementations
   - Use Phase A/B library functions

4. **Rebuild and test:**
   ```bash
   cmake --build . --target dinerod
   dinero-cli zk.createtx '{"inputs": [...], "outputs": [...]}'
   ```

**Testing:**
- See `ZK_TESTING_STRATEGY.md` - Test 11 for RPC integration
- Manual testing with `dinero-cli`

---

### Transaction Structure Integration (PARALLEL TO Phase B)

**Goal:** Extend `TxOutput` to support confidential transactions

**Design document:** `ZK_TRANSACTION_STRUCTURE.md`

**Key changes:**
```cpp
struct TxOutput {
    // Existing transparent fields
    uint64_t value;  // 0 if confidential
    std::vector<uint8_t> scriptPubKey;

    // NEW: Confidential fields
    bool is_confidential;
    std::vector<uint8_t> commitment;  // 33-byte Pedersen commitment
    std::vector<uint8_t> range_proof;  // Bulletproof (~5KB)

    // NEW: Confidential methods
    bool IsConfidential() const;
    bool GetCommitment(secp256k1_pedersen_commitment* out) const;
    bool VerifyRangeProof(secp256k1_context* ctx, ...) const;
};
```

**Files to modify:**
- `include/wallet/transaction.h` - Extend TxOutput structure
- `src/wallet/transaction.cpp` - Serialization (0xFF marker format)
- `src/consensus/tx_validation.cpp` - Add `CheckConfidentialTransaction()`

**Critical:** Implement blinding factor encryption in wallet database!

---

### Phase C: View Keys (OPTIONAL)

**Goal:** Allow receivers to extract amounts from confidential outputs

**Implementation:**
- Use `secp256k1_rangeproof_rewind()` to extract amount with nonce
- Implement `zk.scanviewkey` RPC method
- Add stealth address support (ECDH for automatic nonce derivation)

**Testing:**
- See `ZK_TESTING_STRATEGY.md` - Test 9 for view key rewinding

---

## Current Status Summary

### ✅ Completed (Preparation Work)

**Libraries verified:**
- ✅ secp256k1-zkp vendored with ZK modules enabled
- ✅ Pedersen commitment module ready
- ✅ Bulletproof range proof module ready
- ✅ Bulletproofs++ optimization available
- ✅ libwally-core for PSBT integration
- ✅ **ZERO new dependencies needed!**

**Documentation created:**
- ✅ Phase A API reference (ZK_API_CHEATSHEET.md)
- ✅ Phase B implementation guide (PHASE_B_RANGE_PROOFS.md)
- ✅ RPC integration guide (ZK_RPC_INTEGRATION.md)
- ✅ Transaction structure design (ZK_TRANSACTION_STRUCTURE.md)
- ✅ Comprehensive testing strategy (ZK_TESTING_STRATEGY.md)

**Code skeleton created:**
- ✅ RPC handlers file (`src/rpc/zk_rpc_handlers_context.cpp`)
- ✅ All 5 RPC methods stubbed (zk.createtx, zk.verify, etc.)
- ✅ Registration function `WireZkRpcContext()` ready
- ✅ Integration points documented (exact lines!)

**Build system ready:**
- ✅ `dinero_zk` library already configured in CMakeLists.txt
- ✅ Links against secp256k1-zkp
- ✅ Links against libwally-core
- ✅ Defines `HAVE_ZK_PRIVACY` and `HAVE_CONFIDENTIAL_TX`
- ✅ **Just add RPC file when ready (line 442)**

### ⏳ In Progress (User Working On This)

**Phase A: Pedersen Commitments**
- ⏳ Implementing core library in `src/zk/confidential_tx.cpp`
- ⏳ Commitment creation and verification
- ⏳ Balance verification logic

### 📋 Ready for Implementation (When Phase A Complete)

**Phase B: Range Proofs**
- 📋 Add range proof generation
- 📋 Add range proof verification
- 📋 Implement RPC method bodies
- 📋 Integrate with transaction validation

**Transaction Structure:**
- 📋 Extend TxOutput for confidential fields
- 📋 Implement serialization (0xFF marker)
- 📋 Add `CheckConfidentialTransaction()` validation
- 📋 Encrypt blinding factors in wallet DB

**RPC Integration:**
- 📋 Add to CMakeLists.txt (1 line)
- 📋 Register in rpc_context_wiring.cpp (2 lines)
- 📋 Implement `zk.createtx` (most important!)
- 📋 Test with dinero-cli

---

## File Locations Quick Reference

```
/Users/haydarevich/Documents/DineroCoin/
│
├── CMakeLists.txt                      # Add zk_rpc_handlers_context.cpp at line 442
│
├── src/
│   ├── daemon/
│   │   └── rpc_context_wiring.cpp      # Register ZK RPC at lines 33 & 209
│   │
│   ├── rpc/
│   │   └── zk_rpc_handlers_context.cpp # Skeleton ready (implement method bodies)
│   │
│   ├── zk/
│   │   └── confidential_tx.cpp         # Phase A/B implementation (user working)
│   │
│   ├── wallet/
│   │   └── transaction.cpp             # Extend for confidential TXs
│   │
│   └── consensus/
│       └── tx_validation.cpp           # Add CheckConfidentialTransaction()
│
├── include/
│   └── wallet/
│       └── transaction.h               # Extend TxOutput structure
│
├── third_party/
│   ├── secp256k1-zkp/                  # ✅ Already vendored with ZK modules!
│   │   └── include/
│   │       ├── secp256k1_generator.h   # Pedersen commitments
│   │       ├── secp256k1_rangeproof.h  # Bulletproofs
│   │       └── secp256k1_bppp.h        # Bulletproofs++ (optimized)
│   │
│   └── libwally-core/                  # ✅ Already vendored!
│
└── docs/ (documentation created)
    ├── ZK_LIBRARIES_STATUS.md          # Library status (all ready!)
    ├── ZK_API_CHEATSHEET.md            # Phase A API reference
    ├── PHASE_B_RANGE_PROOFS.md         # Phase B guide
    ├── ZK_RPC_IMPLEMENTATION_GUIDE.md  # RPC usage
    ├── ZK_RPC_INTEGRATION.md           # Integration steps
    ├── ZK_TRANSACTION_STRUCTURE.md     # TxOutput design
    ├── ZK_TESTING_STRATEGY.md          # Test plan
    └── ZK_IMPLEMENTATION_ROADMAP.md    # This file
```

---

## Key Takeaways

**NO libraries need to be vendored** - Everything is already there!

**Exact integration points documented** - Line numbers, file locations, code snippets all ready

**Comprehensive testing strategy** - 11+ tests covering all scenarios

**Non-interfering preparation** - User can continue Phase A work without conflicts

**Clear roadmap:**
1. ⏳ **YOU ARE HERE:** Finish Phase A (Pedersen commitments)
2. 📋 **NEXT:** Implement Phase B (Range proofs) using PHASE_B_RANGE_PROOFS.md
3. 📋 **THEN:** Integrate RPC methods using ZK_RPC_INTEGRATION.md
4. 📋 **FINALLY:** Extend transaction structure using ZK_TRANSACTION_STRUCTURE.md

---

## Performance Targets

**Phase A (Commitments):**
- ✅ < 100 µs per commitment creation
- ✅ < 1 ms per balance verification

**Phase B (Range Proofs):**
- ✅ < 50 ms per proof generation
- ✅ < 10 ms per proof verification
- ✅ ~5KB proof size (or ~450 bytes with Bulletproofs++)

**Transaction Overhead:**
- Transparent TX: ~141 vB (fee: ~141 una)
- Confidential TX (2 outputs): ~10,547 vB (fee: ~10,547 una)
- **Future optimized:** ~958 vB (fee: ~958 una) with aggregated proofs

---

## Security Checklist

**CRITICAL (Must implement):**
- [ ] Encrypt blinding factors at rest (Argon2id + AES-256-GCM)
- [ ] Secure memory wiping after using blinding factors
- [ ] Never log blinding factors
- [ ] Validate all input commitments parse correctly
- [ ] Verify range proofs in consensus validation

**Important (Should implement):**
- [ ] Fee calculation requires at least one transparent output
- [ ] Warn users about Phase A vulnerability (negative amounts)
- [ ] Document that Phase A+B must be deployed together
- [ ] Test negative amount prevention extensively

**Nice to have (Phase C):**
- [ ] Automatic nonce derivation (ECDH)
- [ ] Stealth address support
- [ ] View key scanning in wallet

---

## Next Steps

**When you complete Phase A:**
1. Run tests 1-5 from `ZK_TESTING_STRATEGY.md`
2. Verify Test 5 (NegativeAmountAttack) PASSES (expected vulnerability)
3. Move to Phase B using `PHASE_B_RANGE_PROOFS.md`

**When you complete Phase B:**
1. Run tests 6-9 from `ZK_TESTING_STRATEGY.md`
2. Verify Test 8 (PreventNegativeAmount) now prevents the attack
3. Integrate RPC methods using `ZK_RPC_INTEGRATION.md`

**When RPC integration complete:**
1. Test with `dinero-cli zk.createtx`
2. Create end-to-end confidential transaction
3. Deploy to testnet for user testing

---

## Summary

**All preparation work is complete!** You have:

✅ **No dependencies to vendor** - All ZK libraries already available
✅ **Complete API reference** - Copy-paste ready examples
✅ **Exact integration steps** - File locations and line numbers documented
✅ **Comprehensive test plan** - 11+ tests with expected outputs
✅ **Transaction design** - Full backward-compatible extension plan
✅ **RPC skeleton** - All methods stubbed and ready

**Focus on Phase A** - Everything else is documented and ready when you need it!

Good luck with your Pedersen commitment implementation! 🚀
