# T15 Bug Root Cause Analysis

**Date**: 2025-12-29
**Bug**: Mempool accepts immature coinbase spends
**Severity**: P0 - Consensus-Critical
**Status**: Root cause confirmed

---

## Root Cause Confirmed

**Location**: `src/mempool/mempool.cpp:365-500` (`submitTransaction()`)

**The Bug**: `MempoolSubmitMode::TEST_ONLY` **completely skips consensus validation**, including coinbase maturity checks.

---

## Code Evidence

### File: src/mempool/mempool.cpp

#### Lines 365-398: submitTransaction() with TEST_ONLY mode

```cpp
MempoolAcceptResult Mempool::submitTransaction(
    const Transaction& tx,
    const consensus::ChainStateView& coins_view,
    uint32_t current_height,
    uint64_t current_time,
    MempoolSubmitMode mode
) {
    // TEST_ONLY mode: Skip script validation for policy testing
    //
    // Still enforces:
    // - UTXO existence
    // - No double-spend
    // - Fee rules
    // - Ancestor/descendant limits
    // - Size limits
    // - Conflict detection
    //
    // Skips:
    // - Script verification
    // - Signature checks

    if (mode == MempoolSubmitMode::TEST_ONLY) {
        std::lock_guard<std::mutex> lock(mutex_);

        uint256 txid = tx.GetTxid();

        // 1. Check if already in mempool
        if (contains(txid)) {
            return MempoolAcceptResult::ALREADY_IN_MEMPOOL;
        }

        // ❌ BUG: Lines 397-398
        // 2. Skip consensus validation in TEST_ONLY mode
        //    (acceptTransaction would call validateTransaction here)

        // ← NO CALL TO validateTransaction()!
        // ← Coinbase maturity check is IN validateTransaction()
        // ← Therefore: TEST_ONLY mode BYPASSES maturity enforcement
```

**What's missing**: No call to `validateTransaction()` which contains the coinbase maturity check.

---

## What TEST_ONLY Actually Validates

Looking at lines 400-460, TEST_ONLY mode ONLY checks:

1. ✅ **UTXO existence** (lines 420-427)
   ```cpp
   auto coin_result = mempool_view.getCoin(outpoint);
   if (!coin_result.ok()) {
       return MempoolAcceptResult::MISSING_INPUTS;
   }
   ```

2. ✅ **Fee calculation** (lines 437-439)
   ```cpp
   uint64_t fee = total_in - total_out;
   size_t vsize = calculateVirtualSize(tx);
   double fee_rate = static_cast<double>(fee) / static_cast<double>(vsize);
   ```

3. ✅ **Minimum fee rate** (lines 442-444)
   ```cpp
   if (fee_rate < config_.min_fee_rate) {
       return MempoolAcceptResult::INSUFFICIENT_FEE;
   }
   ```

4. ✅ **Conflict detection** (lines 447-460)
   ```cpp
   if (!checkConflicts(tx, conflicts)) {
       // RBF logic
   }
   ```

---

## What TEST_ONLY **Bypasses** (The Problem)

By skipping `validateTransaction()`, TEST_ONLY mode also skips:

1. ❌ **Coinbase maturity check** (consensus/tx_validation.cpp:99-101)
   ```cpp
   if (coin.isCoinbase && !coin.isMature(ctx.block_height)) {
       return TxValidationResult::COINBASE_MATURITY_VIOLATION;
   }
   ```

2. ❌ **Duplicate input detection** (consensus/tx_validation.cpp:290-295)

3. ❌ **Output value validation** (consensus/tx_validation.cpp:308-312)

4. ❌ **Value balance check** (consensus/tx_validation.cpp:319-324)

5. ❌ **Sequence locks** (consensus/tx_validation.cpp:342-349)

---

## Comparison: NORMAL vs TEST_ONLY

### NORMAL Mode Path (lines 77-131)

```
submitTransaction(NORMAL)
  → acceptTransaction()
    → validateTransaction()  ← ✅ Calls consensus validation
      → consensus::validateTransaction()
        → validateInputs()
          → ❗ COINBASE MATURITY CHECK ❗
          → verifyScript() ← Script verification
```

**Result**: Coinbase maturity enforced ✅

### TEST_ONLY Mode Path (lines 365-500)

```
submitTransaction(TEST_ONLY)
  → ❌ SKIP validateTransaction() entirely
  → Only check:
    - UTXO exists
    - Fee rate
    - Conflicts
  → ❌ NO coinbase maturity check
  → ❌ NO other consensus checks
```

**Result**: Coinbase maturity NOT enforced ❌

---

## Why This Happened

### Original Intent (from comments)

```cpp
// Skips:
// - Script verification
// - Signature checks
```

**Intended**: Skip *expensive* operations (script execution, signature verification)

**Actual**: Skip *all* consensus validation (including cheap checks like coinbase maturity)

### Implementation Mistake

The code comment says "Skip script validation" but the implementation skips the entire `validateTransaction()` call, which includes:
- Script verification (INTENDED to skip)
- Signature checks (INTENDED to skip)
- Coinbase maturity (NOT intended to skip)
- Value checks (NOT intended to skip)
- Other consensus rules (NOT intended to skip)

---

## The Fix

### Option 1: Call validateTransaction() with skip_script_verification flag (RECOMMENDED)

**File**: `src/mempool/mempool.cpp:387-398`

**Current code**:
```cpp
if (mode == MempoolSubmitMode::TEST_ONLY) {
    // ... setup ...

    // 2. Skip consensus validation in TEST_ONLY mode  ← WRONG
    //    (acceptTransaction would call validateTransaction here)

    // ... fee checks ...
}
```

**Fixed code**:
```cpp
if (mode == MempoolSubmitMode::TEST_ONLY) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint256 txid = tx.GetTxid();

    if (contains(txid)) {
        return MempoolAcceptResult::ALREADY_IN_MEMPOOL;
    }

    // ✅ FIX: Call validateTransaction() but skip script verification
    consensus::TxValidationContext ctx;
    ctx.block_height = current_height;
    ctx.median_time_past = current_time;
    ctx.check_sequence_locks = true;
    ctx.skip_script_verification = true;  // ← Skip ONLY script/sig checks

    consensus::TxValidationResult validation_result;
    if (!validateTransaction(tx, coins_view, current_height, current_time, validation_result)) {
        // Map validation failure to mempool result
        if (validation_result == consensus::TxValidationResult::COINBASE_MATURITY_VIOLATION) {
            invalid_tx_cache_.add(txid, "Immature coinbase", current_time);
            return MempoolAcceptResult::INVALID_TX;
        }
        // ... other validation errors ...
        return MempoolAcceptResult::INVALID_TX;
    }

    // ... continue with fee checks, conflict detection ...
}
```

### Option 2: Remove TEST_ONLY mode entirely

**Rationale**: If there's no way to test mempool policy without a working signing system, don't have TEST_ONLY mode.

**Risk**: Breaks existing tests that rely on TEST_ONLY mode.

---

## Consensus Rules That MUST Be Enforced

Even in TEST_ONLY mode, these consensus rules MUST be checked:

1. ✅ **Coinbase maturity** (100 confirmations)
2. ✅ **Duplicate inputs**
3. ✅ **Output values** (> 0, <= MAX_MONEY)
4. ✅ **Value balance** (inputs >= outputs)
5. ✅ **Sequence locks** (BIP 68)
6. ✅ **Transaction size** (<= MAX_TX_SIZE)

**Only these should be skipped in TEST_ONLY**:
- ❌ Script execution (Phase 24)
- ❌ Signature verification

---

## Bitcoin Core Comparison

### Bitcoin Core: AcceptToMemoryPool()

Bitcoin Core does NOT have a "TEST_ONLY" mode. All consensus rules are ALWAYS enforced:

```cpp
// bitcoin/src/validation.cpp
static bool AcceptToMemoryPoolWorker(...) {
    // Coinbase check
    if (tx.IsCoinBase())
        return state.Invalid(...);

    // Consensus checks
    if (!ContextualCheckTransaction(tx, state, chainparams, nHeight, nLockTimeCutoff))
        return false;  // ← Includes coinbase maturity

    // Policy checks
    if (!CheckInputs(...))
        return false;

    // ... more checks ...
}
```

**Key point**: There is NO code path that skips coinbase maturity.

---

## Impact Assessment

### Security Impact

| Impact | Severity |
|--------|----------|
| Invalid transactions enter mempool | Critical |
| Invalid transactions relayed to network | Critical |
| Mempool pollution (DoS vector) | High |
| Wasted miner resources | Medium |
| Potential consensus divergence | Critical |

### Production Risk

**Q**: Is this exploitable in production?

**A**: Only if:
1. Production code uses `MempoolSubmitMode::TEST_ONLY`
2. Mempool API is exposed to external callers

**Likely scenario**: TEST_ONLY is only used in tests (as the name implies)

**However**: The existence of this bug indicates a fundamental misunderstanding of what "test mode" should mean. Defense in depth requires that consensus rules are NEVER bypassed.

---

## Test T15 Validation

### Why T15 Failed

```
Test: submitTransaction(tx, view, 50, time, MempoolSubmitMode::TEST_ONLY)
Mode: TEST_ONLY
Path: submitTransaction() → SKIP validateTransaction() → ACCEPT
Result: MempoolAcceptResult::OK ← Should be INVALID_TX
```

### Expected Behavior

```
Test: submitTransaction(tx, view, 50, time, MempoolSubmitMode::TEST_ONLY)
Mode: TEST_ONLY
Path: submitTransaction() → validateTransaction(skip_script=true) → REJECT
Reason: COINBASE_MATURITY_VIOLATION
Result: MempoolAcceptResult::INVALID_TX ← Correct
```

---

## Action Items

### Immediate

1. ✅ **Fix submitTransaction() TEST_ONLY mode**
   - Call `validateTransaction()` with `skip_script_verification=true`
   - Do NOT skip other consensus checks

2. ✅ **Re-run T15 to verify fix**

3. ✅ **Check all callers of TEST_ONLY mode**
   - Ensure they're only in tests
   - Ensure production never uses TEST_ONLY

### Follow-up

1. **Review all "test modes" in codebase**
   - Document exactly what each mode skips
   - Ensure consensus rules are never bypassed

2. **Add defensive checks**
   - Assert that TEST_ONLY is only used in test builds
   - Log warnings if TEST_ONLY used in production

3. **Bitcoin Core alignment**
   - Remove TEST_ONLY mode entirely
   - Require proper signatures for all mempool transactions

---

## Conclusion

**Root Cause**: `submitTransaction()` in TEST_ONLY mode completely skips `validateTransaction()`, bypassing coinbase maturity checks and other consensus rules.

**Fix**: Call `validateTransaction()` with `skip_script_verification=true` to enforce consensus rules while skipping expensive script execution.

**Impact**: P0 consensus bug that allows invalid transactions into mempool (but likely only exploitable in tests, not production).

**Credit**: T15 test successfully discovered this critical bug, validating the importance of comprehensive defense-in-depth testing.

---

**File**: `src/mempool/mempool.cpp`
**Function**: `submitTransaction()`
**Lines**: 365-500 (TEST_ONLY mode path)
**Fix Priority**: **IMMEDIATE**
