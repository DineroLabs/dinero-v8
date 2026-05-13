# T15 Test Results: Mempool Rejects Immature Spend (S.3)

**Test**: T15 - Mempool Rejects Immature Spend
**Invariant**: S.3 - Mempool Immature Coinbase Rejection
**Phase**: F.8 - Wallet Spending Rules
**Date**: 2025-12-29
**Status**: ❌ **FAIL** - **CRITICAL BUG DISCOVERED**

---

## Executive Summary

**CRITICAL BUG**: The mempool **ACCEPTS** transactions spending immature coinbase outputs (< 100 confirmations), violating Bitcoin consensus rules.

**Severity**: **P0 - Consensus-Critical**
**Impact**: Network-wide consensus violation, potential chain splits
**Layer**: Defense in Depth Layer 3 (Mempool Validation)

---

## Test Summary

Validates that the mempool rejects transactions spending coinbase outputs with fewer than 100 confirmations at the consensus layer, preventing invalid transactions from entering the network.

**Expected**: Mempool rejects transaction with `COINBASE_MATURITY_VIOLATION`
**Actual**: Mempool **ACCEPTED** transaction (result = `OK`)
**Result**: ❌ **FAIL** - Consensus validation NOT enforced

---

## Test Method

**Approach**: Standalone C++ test with mempool integration
**Binary**: `build/bin/standalone_test_t15`
**Source**: `tests/wallet_persistence/standalone_test_t15.cpp`

---

## Test Procedure

1. **Setup**: Create temporary test directories (wallet + coins DB)
2. **Create wallet**: Initialize wallet with test address
3. **Mine immature coinbase**: Create block 1 with 50 DIN coinbase (height 1)
4. **Advance blockchain**: Mine blocks 2-50 (coinbase has 50 confirmations - immature)
5. **Setup consensus DB**: Create CoinsDB and add immature coinbase UTXO
6. **Craft spending transaction**: Manually create transaction spending immature coinbase
7. **Submit to mempool**: Call `submitTransaction()` with `TEST_ONLY` mode
8. **Verify rejection**: Check that mempool rejected the transaction

---

## Test Results

```
[T15.7] Creating transaction that spends immature coinbase...
✓ Transaction created
  Transaction TXID: 2a79cae4ac0e576f7cf4446ad4921fb7ae673d12adccdc2ec07259af831c17af
  Input: 0c55545198d600605b6eb8ae5a86b111435b3ad621356ead95a221ef6ef855c6:0 (immature coinbase)
  Output: 49.9999 DIN
  Fee: 0.0001 DIN

[T15.8] Submitting transaction to mempool...
  Mempool result: OK  ← ❌ SHOULD BE: INVALID_TX

[T15.9] Verifying mempool rejection...

Verification:
  [✗] Transaction rejected by mempool
  [✗] Rejection reason: INVALID_TX (COINBASE_MATURITY_VIOLATION)
  [✗] Transaction not in mempool
  [✗] Mempool empty (count = 1)

❌ FAIL - Mempool did NOT reject immature coinbase spend (S.3 VIOLATED)

Failure reason: Transaction ACCEPTED by mempool (should be REJECTED for immature coinbase)
```

---

## Bug Analysis

### What Went Wrong

The mempool accepted a transaction spending an immature coinbase UTXO:
- **UTXO height**: 1
- **Current height**: 50
- **Confirmations**: 50 (< 100 required)
- **Expected**: `COINBASE_MATURITY_VIOLATION` rejection
- **Actual**: Transaction accepted (`MempoolAcceptResult::OK`)

### Root Cause Investigation

#### Hypothesis 1: TEST_ONLY Mode Skips Maturity Validation

**Theory**: `MempoolSubmitMode::TEST_ONLY` bypasses coinbase maturity checks

**Evidence**:
```cpp
// From mempool.h:126-129
enum class MempoolSubmitMode {
    NORMAL,      // Full validation (requires valid signatures)
    TEST_ONLY    // Skip script validation (policy testing only)
};
```

**Documentation says**: "Skip script/signature validation for policy testing"

**Analysis**: TEST_ONLY should only skip script verification, NOT consensus rules like coinbase maturity.

**Verification needed**: Check `mempool.cpp::submitTransaction()` implementation

#### Hypothesis 2: Missing Maturity Check in Mempool Validation

**Theory**: The mempool's `validateTransaction()` doesn't check `UTXOEntry::isMature()`

**Expected flow**:
```
submitTransaction()
  → validateTransaction()
    → validateInputs()  ← Should check coin.isMature()
      → IF coinbase AND !isMature()
        → RETURN COINBASE_MATURITY_VIOLATION
```

**Verification needed**: Check if `validateInputs()` calls `coin.isMature(ctx.block_height)`

#### Hypothesis 3: CoinsViewCache Not Passing Maturity Info

**Theory**: `CoinsViewCache::getCoin()` doesn't preserve `isCoinbase` or `height` fields

**Expected**:
```cpp
UTXOEntry coin;
coin.isCoinbase = true;  // From database
coin.height = 1;          // From database
coin.isMature(50);        // Should return false
```

**Verification needed**: Check `CoinsViewCache` implementation

---

## Defense Layer Status

### Layer 1: Wallet UTXO Selection ✅ (T12)
- **Status**: Working correctly
- **Mechanism**: `WalletUTXO.spendable = false` for immature coinbase
- **Test**: T12 validates immature coinbase marked as non-spendable
- **Protection**: Prevents wallet from selecting immature coinbase

### Layer 2: Wallet Transaction Creation ⏳ (Not Yet Tested)
- **Status**: Not validated
- **Mechanism**: Transaction builder should check `spendable` before using UTXO
- **Protection**: Second line of defense

### Layer 3: Mempool Validation ❌ (T15 - THIS TEST)
- **Status**: **FAILING** - **CRITICAL BUG**
- **Mechanism**: Consensus validation should enforce `COINBASE_MATURITY`
- **Test**: T15 **FAILS** - Mempool accepts immature spend
- **Protection**: **BROKEN** - Invalid transactions can enter network

### Layer 4: Block Validation ❓ (Unknown)
- **Status**: Assumed working (same consensus code as mempool)
- **Risk**: If mempool validation is broken, block validation might be too
- **Verification needed**: Test block validation with immature coinbase spend

---

## Security Impact

### Immediate Risks

1. **Mempool Pollution**:
   - Invalid transactions can enter local mempool
   - Wastes memory on unmineable transactions
   - DoS vector: Attacker floods mempool with invalid txs

2. **Network Relay**:
   - If mempool accepts, transaction gets relayed to peers
   - Wastes bandwidth across entire network
   - Amplifies DoS attack

3. **Miner Risk**:
   - Miners could include invalid transaction in block
   - Block would be rejected by network
   - Wasted mining power, lost block reward

4. **Consensus Divergence**:
   - If some nodes accept and others reject:
     - Network splits into incompatible views
     - Temporary chain fork (resolved when invalid block rejected)
   - Risk of consensus failure

---

## Consensus Alignment

### Bitcoin Core Behavior

Bitcoin Core **ALWAYS** rejects immature coinbase spends:

**From Bitcoin Core validation code**:
```cpp
// bitcoin/src/validation.cpp
if (coin.IsCoinBase()) {
    if (nSpendHeight - coin.nHeight < COINBASE_MATURITY) {
        return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND,
            "bad-txns-premature-spend-of-coinbase");
    }
}
```

**Result**: `TX_PREMATURE_SPEND` (consensus rejection)

### DineroCoin Expected Behavior

**From consensus/tx_validation.cpp:99-101**:
```cpp
// Check coinbase maturity (100 confirmations)
if (coin.isCoinbase && !coin.isMature(ctx.block_height)) {
    return TxValidationResult::COINBASE_MATURITY_VIOLATION;
}
```

**Expected**: `COINBASE_MATURITY_VIOLATION`
**Actual**: Not called or not working ❌

---

## Test Design Validation

### Was the Test Correct?

**Question**: Did we set up the test scenario correctly?

**Verification**:
1. ✅ Coinbase marked correctly: `coinbase_coin.isCoinbase = true`
2. ✅ Height set correctly: `coinbase_coin.height = 1`
3. ✅ Current height correct: `current_height = 50`
4. ✅ Confirmations: 50 - 1 + 1 = 50 (< 100) ✅
5. ✅ UTXO added to view: `view.addCoin(coinbase_outpoint, coinbase_coin)`
6. ✅ Transaction spends correct outpoint: `input.prevout.txid = coinbase_txid`

**Conclusion**: Test setup is correct. The bug is real.

---

## Comparison to T12 (Wallet Layer)

### T12: Wallet UTXO Selection (Layer 1) ✅

```
[T12] Immature Coinbase Rejection
  Blockchain height: 50
  Confirmations: 50
  is_mature: false ✓
  spendable: false ✓
  Result: PASS ✓
```

**T12 validated**: Wallet correctly prevents selection

### T15: Mempool Validation (Layer 3) ❌

```
[T15] Mempool Rejects Immature Spend
  Blockchain height: 50
  Confirmations: 50
  Mempool result: OK ✗
  Transaction in mempool: true ✗
  Result: FAIL ✗
```

**T15 discovered**: Mempool does NOT enforce maturity

### Key Insight

The wallet prevents the user from creating invalid transactions (Layer 1 works), but if an attacker manually crafts an invalid transaction and submits it directly to the mempool API, it will be accepted (Layer 3 broken).

**Defense in depth is critical**: Layer 1 compensates for Layer 3 failure, but this is not acceptable for consensus rules.

---

## Required Fixes

### Fix 1: Ensure Consensus Validation is Called

**File**: `src/mempool/mempool.cpp`
**Function**: `Mempool::submitTransaction()`

**Verify**:
```cpp
MempoolAcceptResult Mempool::submitTransaction(..., MempoolSubmitMode mode) {
    // ...

    // CRITICAL: validateTransaction() must be called for ALL modes
    bool valid = validateTransaction(tx, coins_view, current_height, current_time, result);

    // TEST_ONLY should only skip script verification, NOT consensus rules
    if (mode == MempoolSubmitMode::TEST_ONLY) {
        // Skip script verification (Phase 24)
        // BUT: coinbase maturity must still be checked!
    }

    if (!valid) {
        return MempoolAcceptResult::INVALID_TX;  // Map TxValidationResult to MempoolAcceptResult
    }
    // ...
}
```

### Fix 2: Verify validateTransaction() Checks Maturity

**File**: `src/mempool/mempool.cpp`
**Function**: `Mempool::validateTransaction()`

**Ensure**:
```cpp
bool Mempool::validateTransaction(
    const Transaction& tx,
    const consensus::ChainStateView& coins_view,
    uint32_t current_height,
    uint64_t current_time,
    consensus::TxValidationResult& result
) {
    // Create validation context
    consensus::TxValidationContext ctx(current_height, current_time);

    // CRITICAL: skip_script_verification should NOT skip coinbase maturity
    ctx.skip_script_verification = false;  // or true for TEST_ONLY, but maturity separate

    // Call consensus validation (includes coinbase maturity check)
    auto validation_result = consensus::validateTransaction(tx, view, ctx);

    if (validation_result.result != TxValidationResult::OK) {
        result = validation_result.result;
        return false;  // ← Should trigger INVALID_TX rejection
    }

    return true;
}
```

### Fix 3: Map TxValidationResult to MempoolAcceptResult

**File**: `src/mempool/mempool.cpp`

**Add mapping**:
```cpp
if (result == TxValidationResult::COINBASE_MATURITY_VIOLATION) {
    return MempoolAcceptResult::INVALID_TX;  // Map to INVALID_TX
}
```

---

## Recommended Next Steps

### Immediate Actions

1. **Investigate `mempool.cpp:submitTransaction()`**:
   - Check if `validateTransaction()` is called in TEST_ONLY mode
   - Verify that maturity checks are NOT skipped

2. **Check `mempool.cpp:validateTransaction()`**:
   - Verify it calls `consensus::validateTransaction()`
   - Confirm maturity checks are executed

3. **Review `CoinsViewCache` implementation**:
   - Ensure `isCoinbase` and `height` fields are preserved
   - Verify `getCoin()` returns complete UTXO data

4. **Test with NORMAL mode**:
   - Retry T15 with `MempoolSubmitMode::NORMAL` (requires valid signatures)
   - See if behavior changes

### Additional Testing

1. **T15b: Test with NORMAL Mode**:
   - Create properly signed transaction
   - Submit with full validation
   - Verify rejection

2. **T15c: Test Block Validation**:
   - Manually create block with immature coinbase spend
   - Submit to `BlockValidator`
   - Verify block rejection

3. **T15d: Test with Different Confirmation Counts**:
   - Test at 0, 1, 50, 99, 100, 101 confirmations
   - Map out exact maturity boundary behavior

---

## Lessons Learned

### Critical Findings

1. **TEST_ONLY Mode is Dangerous**:
   - Designed to skip script verification for policy testing
   - May inadvertently skip consensus rules
   - Should be clearly documented what is/isn't validated

2. **Defense in Depth is Essential**:
   - Layer 1 (wallet) prevents user error ✅
   - Layer 3 (mempool) should prevent network pollution ❌
   - One layer failure is tolerable, but not for consensus

3. **Consensus Rules Must Be Universal**:
   - Coinbase maturity is a consensus rule (not policy)
   - MUST be enforced at ALL validation points:
     - Mempool acceptance
     - Block validation
     - Reorg validation
   - NO exceptions for testing modes

### Testing Value

**T15 demonstrated**:
- Integration testing catches bugs unit tests might miss
- Consensus layer testing is critical
- Defense in depth must be validated at each layer
- Test modes must be carefully designed to not bypass consensus

---

## S.3 Invariant Status

**Invariant**: S.3 - Mempool Immature Coinbase Rejection

> "The mempool MUST reject transactions spending coinbase outputs with fewer
> than 100 confirmations at the consensus layer (COINBASE_MATURITY_VIOLATION)."

**Status**: ❌ **VIOLATED**

**Evidence**:
- Mempool accepted immature coinbase spend
- Rejection code: `OK` (should be `INVALID_TX`)
- Transaction entered mempool (count = 1)

**Fix Required**: Enforce `COINBASE_MATURITY` in mempool validation

---

## Conclusion

**Test Result**: ❌ **FAIL** - Critical bug discovered

**Bug Summary**:
- Mempool accepts transactions spending immature coinbase
- Consensus validation not enforced at mempool layer
- S.3 invariant violated
- Network security at risk

**Severity**: P0 - Consensus-critical

**Priority**: **IMMEDIATE FIX REQUIRED**

**Next Steps**:
1. Investigate mempool validation code path
2. Fix consensus validation enforcement
3. Re-run T15 to verify fix
4. Add additional test cases for edge conditions
5. Review other consensus rules for similar issues

---

**Test Execution Time**: < 1 second
**Exit Code**: 1 (failure)
**Test File**: `tests/wallet_persistence/standalone_test_t15.cpp`
**Build**: `standalone_test_t15` target in CMake

**Phase F.8 Progress**: 3/5 P0 tests passing (60%)

- [x] T14: Spendable Balance Calculation ✅
- [x] T12: Immature Coinbase Rejection (Wallet Layer) ✅
- [x] T13: Mature Coinbase Spending ✅
- [x] T15: Mempool Rejects Immature ❌ **BUG FOUND**
- [ ] T16: Reorg Impact on Spendability ⏳

---

## Appendix: Complete Test Output

```
[T15.8] Submitting transaction to mempool...
  Mempool result: OK

[T15.9] Verifying mempool rejection...

Verification:
  [✗] Transaction rejected by mempool
  [✗] Rejection reason: INVALID_TX (COINBASE_MATURITY_VIOLATION)
  [✗] Transaction not in mempool
  [✗] Mempool empty (count = 1)

❌ FAIL - Mempool did NOT reject immature coinbase spend (S.3 VIOLATED)

Failure reason: Transaction ACCEPTED by mempool (should be REJECTED for immature coinbase); Wrong rejection reason - expected INVALID_TX (COINBASE_MATURITY_VIOLATION), got OK; Transaction found in mempool (should not be present); Mempool not empty (count=1)

S.3 Invariant VIOLATED:
"The mempool MUST reject transactions spending coinbase
 outputs with fewer than 100 confirmations at the
 consensus layer (COINBASE_MATURITY_VIOLATION)."

Security Impact:
- Invalid transactions can enter mempool
- Invalid transactions relayed to network (DoS vector)
- Mempool pollution with unmineable transactions
- Consensus violation (could cause chain split)
```
