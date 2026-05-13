# Phase C.3: Covenant Construction Helpers - Implementation Plan

**Date**: 2025-12-27
**Status**: Planning
**Foundation**: Built on Phase C.2 (covenant mempool policy)

---

## 🎯 Phase C.3 Objectives

**Goal**: Provide wallet-side helpers for constructing covenant transactions

**Scope**: Construction ONLY, never validation
- ✅ Build CTV templates
- ✅ Create CSFS delegations
- ✅ Assemble covenant transactions
- ✅ Generate valid test transactions
- ❌ NEVER validate covenant rules (that's consensus)

**Non-Negotiable Rule**:
```
Wallet constructs → Mempool filters → Consensus validates
                    (policy)         (authority)
```

---

## 🧱 Architectural Constraints

| Rule | Status |
|------|--------|
| Wallet ONLY constructs transactions | ✅ |
| Wallet NEVER validates covenants | ✅ |
| Use consensus helpers for hash computation | ✅ |
| No covenant logic duplication | ✅ |
| Construction helpers are OPTIONAL | ✅ |

**Phase C.1 taught us**: Wallet must NEVER call `consensus::VerifyCTV()`

**Phase C.3 allows**: Wallet CAN call `consensus::ComputeCTVHash()` for construction

---

## 🧩 Phase C.3.A: CTV Template Builder

### Goal
Provide helpers for creating BIP-119 CTV templates and funding transactions.

### Implementation

**File**: `src/wallet/covenant_builders.cpp` (NEW)

**Functions**:
```cpp
namespace dinero {
namespace wallet {

/**
 * Build a CTV template from desired outputs
 *
 * Phase C.3: CONSTRUCTION ONLY - does not validate
 *
 * @param outputs       List of outputs to commit to
 * @param locktime      Transaction locktime
 * @param version       Transaction version (default: 2)
 * @return              Template with computed hash
 */
CTVTemplateBuilder buildCTVTemplate(
    const std::vector<CTVOutput>& outputs,
    uint32_t locktime = 0,
    int32_t version = 2
);

/**
 * Create a P2WSH script that commits to a CTV template
 *
 * Format: OP_SHA256 <template_hash> OP_EQUAL
 * Or for Taproot: <internal_key> OP_CHECKTEMPLATEVERIFY
 *
 * @param template_hash  32-byte CTV template hash
 * @param use_taproot    Use Taproot instead of P2WSH (default: true)
 * @return               ScriptPubKey for the covenant output
 */
std::vector<uint8_t> createCTVScript(
    const std::array<uint8_t, 32>& template_hash,
    bool use_taproot = true
);

/**
 * Build a transaction that SPENDS a CTV output
 *
 * Phase C.3: CONSTRUCTION - builds the spending tx matching template
 * Consensus will validate the match during script execution
 *
 * @param template      The CTV template to satisfy
 * @param funding_utxo  The CTV-locked UTXO to spend
 * @return              Transaction ready for signing
 */
Transaction buildCTVSpendingTx(
    const CTVTemplate& ctv_template,
    const CanonicalWalletUTXO& funding_utxo
);

} // namespace wallet
} // namespace dinero
```

**Key Principle**:
- Wallet computes hashes for construction (allowed)
- Wallet builds transactions matching templates (allowed)
- Wallet NEVER checks if transactions are valid (forbidden)

---

## 🧩 Phase C.3.B: CSFS Delegation Builder

### Goal
Provide helpers for creating CheckSigFromStack delegations.

### Implementation

**File**: `src/wallet/covenant_builders.cpp` (same file)

**Functions**:
```cpp
/**
 * Create a CSFS delegation
 *
 * Phase C.3: CONSTRUCTION - prepares message for signing
 * Does NOT verify signatures (that's consensus)
 *
 * @param pubkey   32-byte x-only Schnorr pubkey
 * @param message  Arbitrary message to be signed
 * @param purpose  Human-readable purpose ("oracle", "delegation", etc.)
 * @return         CSFS delegation (unsigned)
 */
CSFSDelegation createCSFSDelegation(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::string& purpose = "delegation"
);

/**
 * Sign a CSFS delegation with a private key
 *
 * Phase C.3: SIGNING - creates Schnorr signature over message
 * Does NOT verify the signature (consensus does that)
 *
 * @param delegation  Unsigned delegation
 * @param privkey     32-byte private key
 * @return            Signed delegation
 */
CSFSDelegation signCSFSDelegation(
    const CSFSDelegation& delegation,
    const std::vector<uint8_t>& privkey
);

/**
 * Create a P2TR script with CSFS constraint
 *
 * Format: <pubkey> <message> OP_CHECKSIGFROMSTACKVERIFY <normal_script>
 *
 * @param pubkey       32-byte x-only pubkey
 * @param message      Message that must be signed
 * @param continuation_script  Script to execute after CSFS check
 * @return             Complete Tapscript with CSFS
 */
std::vector<uint8_t> createCSFSScript(
    const std::vector<uint8_t>& pubkey,
    const std::vector<uint8_t>& message,
    const std::vector<uint8_t>& continuation_script = {}
);
```

---

## 🧩 Phase C.3.C: Transaction Assembly Helpers

### Goal
Higher-level helpers for assembling complete covenant transactions.

### Implementation

**File**: `src/wallet/covenant_builders.cpp`

**Functions**:
```cpp
/**
 * Create a transaction that FUNDS a covenant
 *
 * Phase C.3: Standard transaction builder
 * Creates outputs locked by covenant scripts
 *
 * @param inputs           UTXOs to spend
 * @param covenant_outputs Covenant-locked outputs to create
 * @param change_address   Address for change
 * @param fee_rate         Fee rate in sat/vB
 * @return                 Funding transaction (needs signing)
 */
Transaction createCovenantFundingTx(
    const std::vector<CanonicalWalletUTXO>& inputs,
    const std::vector<CovenantOutput>& covenant_outputs,
    const std::string& change_address,
    uint64_t fee_rate = 1
);

/**
 * Estimate witness size for covenant spending
 *
 * Phase C.3: Fee estimation helper
 * Used for calculating fees when spending covenant UTXOs
 *
 * @param covenant_type  Type of covenant (CTV, CSFS, etc.)
 * @param template_size  Size of template/message (if applicable)
 * @return               Estimated witness size in vbytes
 */
size_t estimateCovenantWitnessSize(
    CovenantType covenant_type,
    size_t template_size = 0
);
```

---

## 🧩 Phase C.3.D: Test Transaction Generators

### Goal
Generate VALID covenant transactions for integration testing.

### Implementation

**File**: `tests/covenant/covenant_test_utils.cpp` (NEW)

**Functions**:
```cpp
/**
 * Create a simple valid CTV transaction for testing
 *
 * Generates:
 * 1. Funding tx that creates CTV-locked output
 * 2. Spending tx that satisfies the CTV template
 *
 * Both transactions are VALID and ready for mempool/consensus testing
 *
 * @return Pair of (funding_tx, spending_tx)
 */
std::pair<Transaction, Transaction> createValidCTVTestTx();

/**
 * Create a simple valid CSFS transaction for testing
 *
 * Generates:
 * 1. Funding tx that creates CSFS-locked output
 * 2. Spending tx with valid signature
 *
 * @return Pair of (funding_tx, spending_tx)
 */
std::pair<Transaction, Transaction> createValidCSFSTestTx();

/**
 * Create an INVALID covenant transaction for testing
 *
 * Generates transaction that will fail covenant validation
 * Used to test consensus rejection
 *
 * @param failure_type  Type of covenant failure to simulate
 * @return              Invalid transaction
 */
Transaction createInvalidCovenantTx(CovenantFailureType failure_type);
```

---

## 🧩 Phase C.3.E: Integration Tests

### Goal
End-to-end tests: wallet → mempool → consensus → mining

### Test Matrix

**File**: `tests/covenant/test_covenant_integration.cpp` (NEW)

**Tests**:
1. ✅ Valid CTV tx: wallet builds → mempool accepts → consensus validates → mining includes
2. ✅ Invalid CTV tx: wallet builds → mempool rejects OR consensus rejects
3. ✅ Valid CSFS tx: full flow with signature verification
4. ✅ Covenant ancestor policy: mempool enforces parent rules
5. ✅ Covenant fee estimation: accurate witness size prediction
6. ✅ Mixed transaction: covenant + standard inputs
7. ✅ Covenant RBF: policy enforcement (if enabled)

---

## 🚫 Explicitly Out of Scope

**Phase C.3 will NOT include**:
- ❌ Covenant validation logic in wallet
- ❌ Checking if covenant rules are satisfied
- ❌ Contract state machines (CCV - deferred to future phase)
- ❌ Vault patterns (deferred)
- ❌ Lightning covenant integration (separate phase)
- ❌ RPC endpoints for covenant management (Phase C.4)
- ❌ Persistent covenant tracking (policy-only, in-memory)

---

## Implementation Order

### Stage 1: Core Builders (2-3 days)
1. Create `src/wallet/covenant_builders.cpp`
2. Implement CTV template builder
3. Implement CTV script creation
4. Implement CTV spending tx builder
5. Add unit tests

### Stage 2: CSFS Support (1-2 days)
1. Implement CSFS delegation builder
2. Implement CSFS signing helper
3. Implement CSFS script creation
4. Add unit tests

### Stage 3: Test Infrastructure (2-3 days)
1. Create `tests/covenant/covenant_test_utils.cpp`
2. Implement valid transaction generators
3. Implement invalid transaction generators
4. Verify transactions pass consensus

### Stage 4: Integration Tests (2-3 days)
1. Create `tests/covenant/test_covenant_integration.cpp`
2. Test full flow: wallet → mempool → consensus
3. Test policy enforcement
4. Test fee estimation

### Stage 5: Documentation (1 day)
1. Update covenant_builders.h with usage examples
2. Create covenant construction guide
3. Document boundary rules
4. Sign off Phase C.3

**Total Duration**: 8-12 days

---

## Critical Files

**New Files**:
- `src/wallet/covenant_builders.cpp` - Construction helpers
- `include/wallet/covenant_builders.h` - Public API
- `tests/covenant/covenant_test_utils.cpp` - Test utilities
- `tests/covenant/test_covenant_integration.cpp` - Integration tests

**Modified Files**:
- `include/wallet/covenant_wallet.h` - Already has structures, may need helpers
- `src/wallet/covenant_wallet.cpp` - May need to call builders

---

## Boundary Rules (Critical)

### ✅ ALLOWED in Phase C.3

**Construction**:
- ✅ Call `consensus::ComputeCTVHash()` to build templates
- ✅ Call `consensus::ComputeTxHash()` for TXHASH
- ✅ Create scripts with covenant opcodes
- ✅ Build transactions matching templates
- ✅ Estimate fees and witness sizes
- ✅ Generate test transactions

**Rationale**: These are CONSTRUCTION operations, not VALIDATION

### ❌ FORBIDDEN in Phase C.3

**Validation**:
- ❌ Call `consensus::VerifyCTV()` to check validity
- ❌ Call `consensus::VerifySignatureFromStack()` to verify sigs
- ❌ Call `consensus::VerifyContractTransition()`
- ❌ Implement any covenant validation logic
- ❌ Check if transactions satisfy covenant rules
- ❌ Return "valid/invalid" based on covenant checks

**Rationale**: Validation is CONSENSUS-ONLY (single source of truth)

### Comment Markers

Every use of consensus functions must have:
```cpp
// Phase C.3: CONSTRUCTION ONLY - computing hash for template building
// NOT validation - consensus will validate during script execution
auto hash = consensus::ComputeCTVHash(tx, 0);
```

---

## Mechanical Gates

### Update Existing Gates

**`scripts/check_covenant_boundaries.sh`**:
- Already allows `ComputeCTVHash` with "ALLOWED" marker
- Already forbids `VerifyCTV`, `VerifySignatureFromStack`
- No changes needed - Phase C.3 complies

**`scripts/check_covenant_policy.sh`**:
- No changes needed

### New Gate (Optional)

**`scripts/check_covenant_construction.sh`**:
```bash
# Verify construction helpers don't validate
# Check for forbidden patterns in covenant_builders.cpp

# Pattern 1: No "valid" or "invalid" return values based on covenant checks
# Pattern 2: All consensus function calls have CONSTRUCTION marker
# Pattern 3: No duplication of consensus logic
```

---

## Success Criteria

Phase C.3 is complete when:

- ✅ CTV template builder implemented
- ✅ CTV spending tx builder implemented
- ✅ CSFS delegation builder implemented
- ✅ Test transaction generators work
- ✅ Integration tests pass (wallet → consensus)
- ✅ All boundary gates pass
- ✅ No validation logic in wallet
- ✅ Documentation complete

---

## Risk Mitigation

**Risk 1**: Accidentally adding validation logic
- **Mitigation**: Mechanical gates, code review, explicit markers

**Risk 2**: Construction helpers too complex
- **Mitigation**: Keep helpers simple, defer advanced features

**Risk 3**: Test transactions don't pass consensus
- **Mitigation**: Iterate with consensus team, use known-good patterns

---

## Documentation Requirements

**covenant_builders.h header**:
```cpp
/**
 * Phase C.3: Covenant Construction Helpers
 *
 * BOUNDARY RULE: These helpers CONSTRUCT covenant transactions.
 * They NEVER validate covenant rules.
 *
 * Validation happens EXCLUSIVELY in consensus::ScriptInterpreter.
 *
 * Allowed operations:
 *   ✅ Compute hashes for template building
 *   ✅ Create covenant scripts
 *   ✅ Assemble transactions
 *   ✅ Estimate fees
 *
 * Forbidden operations:
 *   ❌ Verify covenant validity
 *   ❌ Check template matches
 *   ❌ Validate signatures
 *   ❌ Return "valid/invalid"
 */
```

---

## Next Immediate Action

**Start with Phase C.3.A**: Implement CTV template builder

1. Create `include/wallet/covenant_builders.h`
2. Create `src/wallet/covenant_builders.cpp`
3. Implement `buildCTVTemplate()` function
4. Add unit test
5. Verify boundary gates pass

Once CTV builder works, proceed to CSFS (C.3.B).

---

## Phase Dependencies

**Requires**:
- ✅ Phase C.1 (covenant consensus audit) - COMPLETE
- ✅ Phase C.2 (covenant mempool policy) - COMPLETE
- ✅ Phase M.3 (wallet unification) - COMPLETE

**Enables**:
- Phase C.4 (RPC endpoints for covenants)
- Advanced covenant patterns (vaults, channels)
- Lightning covenant integration

---

**Plan Status**: ✅ Ready for implementation
