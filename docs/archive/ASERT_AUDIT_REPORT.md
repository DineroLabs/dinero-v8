# ASERT Implementation Audit Report

**Date:** 2025-12-21
**Auditor:** Analysis of DineroCoin ASERT vs Bitcoin Cash Node Canonical Specification
**Status:** ⚠️ **CRITICAL DEVIATIONS FOUND**

---

## Executive Summary

DineroCoin has **THREE different ASERT implementations**, none of which match the **canonical Bitcoin Cash Node ASERT specification**. The most critical issue is the use of **floating-point arithmetic** in the production implementation, which violates consensus safety requirements.

### Severity Levels

- 🔴 **CRITICAL:** Consensus-breaking, will cause chain forks
- 🟡 **HIGH:** Incorrect math, may cause difficulty oscillation
- 🟢 **LOW:** Implementation style, but functionally equivalent

---

## Canonical ASERT Specification (Bitcoin Cash Node)

### Core Requirements

1. **Pure Integer Arithmetic** - NO floating-point operations
2. **16-bit Fixed-Point** - Radix = 65,536 (2^16)
3. **Cubic Polynomial Approximation** for 2^x where x ∈ [0, 1)
4. **Polynomial Coefficients** (consensus-critical):
   - c1 = 195,766,423,245,049
   - c2 = 971,821,376
   - c3 = 5,127
   - offset = 2^47
   - shift = 48 bits
5. **Truncating Division** - NOT floor division
6. **Arithmetic Right Shifts** - Sign-extending for negative values
7. **Anchor Timestamp** - Use anchor's PARENT timestamp
8. **No Emergency Rules** - On mainnet (testnet only: 20-minute rule)

### Formula

```
next_target = anchor_target × 2^(excess_time / halflife)

Where:
  excess_time = time_delta - ideal_time
  time_delta = current_time - anchor_parent_time
  ideal_time = (current_height - anchor_height) × target_spacing
```

### Implementation

```cpp
// Exponent in Q16 fixed-point
exponent = ((time_delta - ideal_time) * 65536) / halflife

// Decompose into integer and fractional parts
shifts = exponent >> 16
frac = exponent & 0xFFFF

// Cubic polynomial approximation of 2^(frac/65536)
factor = 65536 +
    ((195766423245049 * frac +
      971821376 * frac * frac +
      5127 * frac * frac * frac +
      (1 << 47)) >> 48)

// Apply factor
next_target = anchor_target * factor
next_target >>= 16  // Normalize

// Apply integer shifts
if (shifts > 0)
    next_target <<= shifts  // with overflow check
else
    next_target >>= -shifts

// Clamp to powLimit
if (next_target > powLimit)
    next_target = powLimit
if (next_target == 0)
    next_target = 1
```

---

## DineroCoin Implementation Analysis

### File 1: `src/consensus/asert.cpp`

**Function:** `CalculateNextWork_ASERT_Production()`

#### 🔴 CRITICAL ISSUES

1. **Floating-Point Math** (Line 149-156)
   ```cpp
   // WRONG: Uses floating-point
   double exponent = (double)time_offset / params.half_life_sec;
   exponent = std::clamp(exponent, -10.0, 10.0);
   const double scale = std::exp2(exponent);  // ❌ FLOATING-POINT!
   uint64_t next_target = (uint64_t)(anchor_target * scale);
   ```

   **Bitcoin Cash Node:**
   ```cpp
   // CORRECT: Pure integer arithmetic with polynomial
   int64_t exponent = ((time_delta - ideal_time) * 65536) / halflife;
   int64_t shifts = exponent >> 16;
   uint64_t frac = exponent & 0xFFFF;

   uint32_t factor = 65536 +
       ((195766423245049ull * frac +
         971821376ull * frac * frac +
         5127ull * frac * frac * frac +
         (1ull << 47)) >> 48);
   ```

   **Impact:** 🔴 **CONSENSUS VIOLATION** - Floating-point results are non-deterministic across platforms/compilers

2. **Bootstrap Phase** (Lines 43-49)
   ```cpp
   // WRONG: Not in canonical ASERT
   if (next_height >= BOOTSTRAP_START && next_height < BOOTSTRAP_END) {
       return BOOTSTRAP_BITS;
   }
   ```

   **Bitcoin Cash Node:** No bootstrap phase - ASERT starts immediately at anchor

   **Impact:** 🟡 **NON-STANDARD** - DineroCoin-specific fork, but deterministic

3. **Per-Block Clamps** (Lines 165-179)
   ```cpp
   // WRONG: Not in canonical ASERT
   double max_hard_factor = std::pow(1.0 / params.max_up_per_block, height_diff);
   double max_ease_factor = std::pow(1.0 / params.max_down_per_block, height_diff);
   ```

   **Bitcoin Cash Node:** No per-block clamps - ASERT is self-correcting

   **Impact:** 🟡 **NON-STANDARD** - May cause oscillation

4. **Emergency Ease** (Lines 139-144, 182-184)
   ```cpp
   // WRONG: Different from BCH testnet emergency rule
   const int64_t emergency_threshold = 12 * 3600; // 12 hours
   if (since_last > emergency_threshold) {
       next_target += next_target / 4;  // +25%
   }
   ```

   **Bitcoin Cash Node (testnet only):**
   ```cpp
   // BCH testnet: Allow powLimit after 20 minutes (2× spacing)
   if (block_time > prev_time + 2 * spacing) {
       return powLimit;
   }
   ```

   **Impact:** 🟡 **NON-STANDARD** - Different emergency behavior

#### ✅ CORRECT ASPECTS

- Uses MedianTimePast for timestamps ✅
- Clamps to powLimit ✅
- Prevents zero target ✅

---

### File 2: `src/consensus/pow_asert.hpp`

**Function:** `CalculateASERT_Target()`

#### 🔴 CRITICAL ISSUES

1. **Linear Approximation** (Lines 120-127)
   ```cpp
   // WRONG: Linear approximation instead of cubic polynomial
   // Simple linear approximation for now:
   // newTarget *= (1 + r/65536)
   // = newTarget + newTarget * r / 65536
   ```

   **Bitcoin Cash Node:**
   ```cpp
   // CORRECT: Cubic polynomial (3rd order)
   factor = 65536 +
       ((195766423245049ull * frac +
         971821376ull * frac * frac +
         5127ull * frac * frac * frac +
         (1ull << 47)) >> 48);
   ```

   **Impact:** 🔴 **INCORRECT MATH** - Linear approximation has >1% error, cubic has <0.013%

2. **Manual Byte-Array Bit Shifting** (Lines 96-117)
   ```cpp
   // WRONG: Manual bit manipulation on byte arrays
   for (int64_t i = 0; i < k; ++i) {
       uint8_t carry = 0;
       for (int j = 31; j >= 0; --j) {
           uint16_t temp = (static_cast<uint16_t>(newTarget[j]) << 1) | carry;
           // ...
       }
   }
   ```

   **Bitcoin Cash Node:**
   ```cpp
   // CORRECT: Uses arith_uint256 class
   arith_uint256 nextTarget = refTarget * factor;
   nextTarget >>= 16;
   if (shifts > 0)
       nextTarget <<= shifts;
   else
       nextTarget >>= -shifts;
   ```

   **Impact:** 🟡 **INEFFICIENT** - Functionally equivalent but error-prone

3. **Wrong Fractional Formula** (Lines 152-178)
   ```cpp
   // WRONG: Adds/subtracts fractional part linearly
   if (excessTime > 0) {
       // Add fractional part
   } else {
       // Subtract fractional part
   }
   ```

   **Bitcoin Cash Node:**
   ```cpp
   // CORRECT: Multiplies by factor (always positive)
   nextTarget = refTarget * factor;
   nextTarget >>= 16;
   ```

   **Impact:** 🔴 **INCORRECT MATH** - Wrong sign handling

#### ✅ CORRECT ASPECTS

- Integer-only arithmetic ✅
- 16-bit fixed-point radix ✅
- Clamps to powLimit ✅

---

### File 3: `src/consensus/pow_asert_native.hpp`

**Function:** `CalculateASERT()`

#### 🔴 CRITICAL ISSUES

1. **Taylor Series Instead of Polynomial** (Lines 100-119)
   ```cpp
   // WRONG: 2nd-order Taylor series for 2^x
   // Taylor series: 2^x ≈ 1 + ln(2)×x + (ln(2))^2/2 × x^2

   const int64_t LN2_Q16 = 45426;    // ln(2) × 65536
   const int64_t LN2_2_Q16 = 15744;  // (ln(2))^2/2 × 65536

   int64_t factorQ16 = S + term1 + term2;
   ```

   **Bitcoin Cash Node:**
   ```cpp
   // CORRECT: Cubic polynomial approximation
   factor = 65536 +
       ((195766423245049ull * frac +
         971821376ull * frac * frac +
         5127ull * frac * frac * frac +
         (1ull << 47)) >> 48);
   ```

   **Impact:** 🔴 **WRONG COEFFICIENTS** - Different approximation, different results

2. **Modulo Arithmetic** (Lines 72, 87)
   ```cpp
   // WRONG: Uses integer division and modulo
   int64_t k = (excessTime / halfLife);
   const int64_t r = (excessTime % halfLife);
   ```

   **Bitcoin Cash Node:**
   ```cpp
   // CORRECT: Fixed-point division, then decompose
   int64_t exponent = (excessTime * 65536) / halfLife;
   int64_t shifts = exponent >> 16;
   uint64_t frac = exponent & 0xFFFF;
   ```

   **Impact:** 🟡 **DIFFERENT ROUNDING** - May cause ±1 bit differences

#### ✅ CORRECT ASPECTS

- Uses arith_uint256 class ✅
- Integer-only arithmetic ✅
- Q16 fixed-point ✅
- No floating-point ✅

---

## Comparison Matrix

| Requirement | Bitcoin Cash Node | asert.cpp | pow_asert.hpp | pow_asert_native.hpp |
|-------------|-------------------|-----------|---------------|----------------------|
| **Integer-only** | ✅ YES | 🔴 NO (std::exp2) | ✅ YES | ✅ YES |
| **16-bit radix** | ✅ 65536 | 🔴 N/A (float) | ✅ 65536 | ✅ 65536 |
| **Cubic polynomial** | ✅ YES | 🔴 NO (exp2) | 🔴 NO (linear) | 🔴 NO (Taylor) |
| **Correct coefficients** | ✅ 195.7T, 971M, 5127 | 🔴 N/A | 🔴 N/A | 🔴 45426, 15744 |
| **Truncating division** | ✅ YES | 🟡 N/A | ✅ YES | ✅ YES |
| **Arithmetic shifts** | ✅ YES | 🟡 N/A | ✅ YES | ✅ YES |
| **Anchor parent time** | ✅ YES | 🟢 Uses anchor time | 🟢 Flexible | 🟢 Flexible |
| **No emergency (mainnet)** | ✅ YES | 🔴 Has 12hr rule | 🟢 No rule | 🟢 No rule |
| **Bootstrap phase** | 🔴 NO | 🟡 Has (2-9999) | 🟢 NO | 🟢 NO |
| **Per-block clamps** | 🔴 NO | 🟡 Has clamps | 🟢 NO | 🟢 NO |

---

## Which Implementation is Active?

**Critical Question:** Which file is actually used in production?

Need to check:
- CMakeLists.txt linking
- Header includes in mining code
- Function calls in block_assembler.cpp

---

## Test Vector Comparison

### Bitcoin Cash Node Test Vectors

From: https://gitlab.com/bitcoin-cash-node/bchn-sw/qa-assets/-/tree/master/test_vectors/aserti3-2d

**Example Test Case:**
```
anchor_height: 661647
anchor_time: 1605447844
anchor_bits: 0x1804dafe
current_height: 661648
current_time: 1605448549
halflife: 172800 (2 days)
target_spacing: 600 (10 minutes)

Expected result: 0x1804d963
```

**Need to verify:** Does DineroCoin produce the same result?

---

## Recommendations

### 🚨 IMMEDIATE ACTION REQUIRED

1. **Replace floating-point implementation** (`asert.cpp`)
   - **Risk:** Current implementation will cause consensus failures
   - **Action:** Use integer-only arithmetic with canonical polynomial

2. **Implement Bitcoin Cash Node polynomial**
   - **Coefficients:**
     ```cpp
     195766423245049ull  // c1
     971821376ull        // c2
     5127ull             // c3
     (1ull << 47)        // offset
     >> 48               // shift
     ```

3. **Remove non-standard features** (for BCH compatibility)
   - Bootstrap phase (or clearly document as DineroCoin-specific)
   - Per-block clamps (or prove they don't cause oscillation)
   - Emergency ease (or align with BCH testnet rule)

4. **Test against BCH test vectors**
   - Download: https://gitlab.com/bitcoin-cash-node/bchn-sw/qa-assets/-/tree/master/test_vectors/aserti3-2d
   - Verify bit-for-bit match

### ✅ CORRECT CANONICAL IMPLEMENTATION

Create new file: `src/consensus/pow_asert_canonical.cpp`

```cpp
#include "consensus/pow_asert_canonical.h"
#include "primitives/arith_uint256.h"
#include <cassert>

/**
 * Canonical ASERT implementation matching Bitcoin Cash Node
 * Reference: https://gitlab.com/bitcoin-cash-node/bitcoin-cash-node/-/blob/master/src/pow.cpp
 */

uint32_t CalculateASERT_Canonical(
    const arith_uint256& refTarget,
    const int64_t nPowTargetSpacing,  // 600 seconds (10 minutes)
    const int64_t nTimeDiff,
    const int64_t nHeightDiff,
    const arith_uint256& powLimit,
    const int64_t nHalfLife) noexcept    // 172800 seconds (2 days)
{
    // Validate inputs
    assert(refTarget > 0 && refTarget <= powLimit);
    assert((powLimit >> 224) == 0);  // At least 32 leading zero bits
    assert(nHeightDiff >= 0);

    // Prevent overflow in exponent calculation
    assert(llabs(nTimeDiff - nPowTargetSpacing * nHeightDiff) < (1ll << (63 - 16)));

    // Calculate exponent in Q16 fixed-point
    // The +1 in (nHeightDiff + 1) accounts for the block being calculated
    const int64_t exponent =
        ((nTimeDiff - nPowTargetSpacing * (nHeightDiff + 1)) * 65536) / nHalfLife;

    // Verify arithmetic right shift support
    static_assert(int64_t(-1) >> 1 == int64_t(-1),
                  "ASERT needs arithmetic shift support");

    // Decompose exponent into integer and fractional parts
    int64_t shifts = exponent >> 16;  // Integer part
    const uint64_t frac = uint16_t(exponent);  // Fractional part [0, 65535]
    assert(exponent == (shifts * 65536) + (int64_t)frac);

    // Polynomial approximation of 2^(frac/65536)
    // Error < 0.013% over range [0, 1)
    const uint32_t factor = 65536 +
        ((+195766423245049ull * frac
          + 971821376ull * frac * frac
          + 5127ull * frac * frac * frac
          + (1ull << 47))
         >> 48);

    // Multiply reference target by factor
    arith_uint256 nextTarget = refTarget * factor;

    // Adjust for the factor's built-in 65536 multiplier
    shifts -= 16;

    // Apply integer shifts
    if (shifts <= 0) {
        // Right shift (harder difficulty)
        nextTarget >>= -shifts;
    } else {
        // Left shift (easier difficulty)
        const auto nextTargetShifted = nextTarget << shifts;

        // Check for overflow
        if ((nextTargetShifted >> shifts) != nextTarget) {
            // Overflow - clamp to powLimit
            nextTarget = powLimit;
        } else {
            nextTarget = nextTargetShifted;
        }
    }

    // Boundary clamping
    if (nextTarget == 0) {
        nextTarget = arith_uint256(1);
    } else if (nextTarget > powLimit) {
        nextTarget = powLimit;
    }

    return nextTarget.GetCompact();
}
```

---

## Testing Strategy

### 1. Unit Tests

Create `test/asert_canonical_test.cpp`:

```cpp
#include "consensus/pow_asert_canonical.h"
#include <gtest/gtest.h>

TEST(AsertCanonical, BitcoinCashTestVector1) {
    // From BCH test vectors
    arith_uint256 refTarget;
    refTarget.SetCompact(0x1804dafe);

    arith_uint256 powLimit;
    powLimit.SetCompact(0x1d00ffff);

    uint32_t result = CalculateASERT_Canonical(
        refTarget,
        600,      // targetSpacing
        705,      // timeDiff (1605448549 - 1605447844)
        1,        // heightDiff (661648 - 661647)
        powLimit,
        172800    // halfLife (2 days)
    );

    EXPECT_EQ(result, 0x1804d963);
}

// Add all BCH test vectors...
```

### 2. Integration Test

```bash
# Download BCH test vectors
wget https://gitlab.com/bitcoin-cash-node/bchn-sw/qa-assets/-/raw/master/test_vectors/aserti3-2d/asert_test_vectors.json

# Run against all test cases
./test/asert_canonical_test --vectors=asert_test_vectors.json
```

### 3. Mainnet Verification

```cpp
// Verify against actual BCH mainnet blocks
// Compare difficulty calculations for blocks 661648-661800
```

---

## Conclusion

DineroCoin's ASERT implementations **do not match** the canonical Bitcoin Cash Node specification. The most critical issue is the use of **floating-point arithmetic** in production code, which violates consensus safety.

### Action Items

| Priority | Task | Status |
|----------|------|--------|
| 🔴 **P0** | Replace floating-point with integer polynomial | ❌ NOT DONE |
| 🔴 **P0** | Implement canonical cubic polynomial | ❌ NOT DONE |
| 🔴 **P0** | Test against BCH test vectors | ❌ NOT DONE |
| 🟡 **P1** | Remove or document non-standard features | ❌ NOT DONE |
| 🟡 **P1** | Add consensus test suite | ❌ NOT DONE |
| 🟢 **P2** | Optimize with arith_uint256 | ⚠️ PARTIAL |

### Risk Assessment

**Current Risk:** 🔴 **HIGH**
- Floating-point math will cause consensus failures
- Non-deterministic across platforms
- Will fork from any BCH-compatible implementation

**Recommended Timeline:**
- **Immediate:** Disable `asert.cpp` in production
- **Week 1:** Implement canonical polynomial
- **Week 2:** Test against all BCH test vectors
- **Week 3:** Deploy to testnet
- **Week 4:** Audit and mainnet deployment

---

**Audit Status:** ⚠️ **INCOMPLETE - CRITICAL ISSUES FOUND**
**Next Step:** Implement canonical ASERT following Bitcoin Cash Node specification
