# ASERT Canonical Integration Checklist

**Purpose:** Step-by-step guide to replace floating-point ASERT with canonical implementation
**Target:** DineroCoin developers
**Estimated Time:** 2-4 hours (testing included)

---

## Phase 1: Verification ⏱️ 15 minutes

### ✅ Step 1.1: Compile Test Program

```bash
cd /Users/haydarevich/Documents/DineroCoin

g++ -std=c++17 \
    -I include \
    src/consensus/asert_canonical.cpp \
    test_asert_canonical.cpp \
    -o test_asert
```

**Expected:** No compilation errors

- [ ] Compiles successfully
- [ ] No warnings
- [ ] Binary created: `test_asert`

### ✅ Step 1.2: Run Tests

```bash
./test_asert
```

**Expected output:**
```
═══════════════════════════════════════════════════════════════
CANONICAL ASERT IMPLEMENTATION TEST SUITE
═══════════════════════════════════════════════════════════════

Testing Bitcoin Cash Node canonical cubic polynomial:
  c1 = 195,766,423,245,049
  c2 = 971,821,376
  c3 = 5,127
  radix = 65,536 (16-bit fixed-point)

[... test cases run ...]

✅ All tests completed successfully!
```

- [ ] All tests pass
- [ ] No crashes
- [ ] Reasonable difficulty values printed
- [ ] Polynomial calculations complete

### ✅ Step 1.3: Review Implementation

```bash
# Check that coefficients match BCH Node
grep -n "195766423245049" src/consensus/asert_canonical.cpp
grep -n "971821376" src/consensus/asert_canonical.cpp
grep -n "5127" src/consensus/asert_canonical.cpp
```

**Expected:** All three coefficients found

- [ ] c1 = 195,766,423,245,049 ✓
- [ ] c2 = 971,821,376 ✓
- [ ] c3 = 5,127 ✓

---

## Phase 2: Build System Integration ⏱️ 10 minutes

### ✅ Step 2.1: Add to CMakeLists.txt

**File:** `CMakeLists.txt`

**Find the section** with consensus source files (search for "asert.cpp"):

```cmake
# Consensus sources
set(CONSENSUS_SOURCES
    src/consensus/pow.cpp
    src/consensus/asert.cpp        # Old floating-point version
    # ... other files ...
)
```

**Add the new file:**

```cmake
# Consensus sources
set(CONSENSUS_SOURCES
    src/consensus/pow.cpp
    src/consensus/asert.cpp         # Old (to be deprecated)
    src/consensus/asert_canonical.cpp  # NEW: Canonical integer-only ASERT
    # ... other files ...
)
```

- [ ] Added `src/consensus/asert_canonical.cpp` to build
- [ ] Kept old `asert.cpp` for now (safe transition)

### ✅ Step 2.2: Build the Project

```bash
cmake --build build --target dinerod -j$(nproc)
```

**Expected:** Clean build with no errors

- [ ] Build succeeds
- [ ] No compilation errors
- [ ] `bin/dinerod` binary created

---

## Phase 3: Code Integration ⏱️ 30 minutes

### ✅ Step 3.1: Find ASERT Call Sites

```bash
# Find where the old ASERT function is called
grep -rn "CalculateNextWork_ASERT_Production" src/
```

**Common locations:**
- `src/consensus/pow.cpp`
- `src/validation/block_validation.cpp`
- Anywhere difficulty is calculated

- [ ] Found all call sites
- [ ] Documented file paths: _________________

### ✅ Step 3.2: Update Includes

**In each file that calls ASERT:**

**OLD:**
```cpp
#include "consensus/asert.h"
```

**NEW:**
```cpp
#include "consensus/asert.h"          // Keep for now (backwards compat)
#include "consensus/asert_canonical.h" // NEW: Add this
```

- [ ] Updated includes in all files

### ✅ Step 3.3: Update Function Calls

**OLD:**
```cpp
uint32_t next_bits = CalculateNextWork_ASERT_Production(
    prev_height,
    prev_median_time_past,
    prev_bits,
    candidate_time,
    anchor_time,
    anchor_bits,
    anchor_height,
    pow_limit_bits,
    target_spacing
);
```

**NEW:**
```cpp
// Get half-life parameter
AsertParams params = GetAsertParamsForHeight(next_height);
int64_t half_life_seconds = (int64_t)params.half_life_sec;

// Use canonical implementation
uint32_t next_bits = CalculateNextWork_ASERT_Canonical(
    prev_height,
    prev_median_time_past,
    prev_bits,
    candidate_time,
    anchor_time,
    anchor_bits,
    anchor_height,
    pow_limit_bits,
    target_spacing,
    half_life_seconds  // NEW: Added parameter
);
```

- [ ] Updated all call sites
- [ ] Added half_life_seconds parameter
- [ ] Verified parameter order matches

### ✅ Step 3.4: Rebuild

```bash
cmake --build build --target dinerod -j$(nproc)
```

- [ ] Build succeeds with new implementation
- [ ] No linker errors
- [ ] Binary created successfully

---

## Phase 4: Testing ⏱️ 1-2 hours

### ✅ Step 4.1: Clean Testnet

```bash
# Backup current testnet data (if needed)
mv ~/.dinero_test ~/.dinero_test_backup_$(date +%s)

# Or just remove
rm -rf ~/.dinero_test/blocks ~/.dinero_test/chainstate
```

- [ ] Testnet data cleaned
- [ ] Fresh start ready

### ✅ Step 4.2: Start Testnet Node

```bash
./bin/dinerod --testnet --daemon

# Wait for startup
sleep 5

# Check it's running
curl -s --user dinero:password \
  --data-binary '{"jsonrpc":"1.0","id":"test","method":"getblockchaininfo","params":[]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:18332/ | jq '.result.blocks'
```

**Expected:** Returns 0 (genesis block)

- [ ] Node started successfully
- [ ] RPC responding
- [ ] At genesis block

### ✅ Step 4.3: Mine Test Blocks

```bash
# Get a test address
ADDRESS=$(curl -s --user dinero:password \
  --data-binary '{"jsonrpc":"1.0","id":"test","method":"getnewaddress","params":[]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:18332/ | jq -r '.result')

echo "Mining to address: $ADDRESS"

# Mine 200 blocks
curl -s --user dinero:password \
  --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"test\",\"method\":\"generatetoaddress\",\"params\":[200,\"$ADDRESS\"]}" \
  -H 'content-type: text/plain;' http://127.0.0.1:18332/
```

- [ ] Mined 200 blocks successfully
- [ ] No crashes
- [ ] No errors in debug.log

### ✅ Step 4.4: Verify Difficulty Adjustment

```bash
# Check difficulty of blocks 1, 50, 100, 150, 200
for height in 1 50 100 150 200; do
  echo -n "Block $height: "
  curl -s --user dinero:password \
    --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"test\",\"method\":\"getblockhash\",\"params\":[$height]}" \
    -H 'content-type: text/plain;' http://127.0.0.1:18332/ | jq -r '.result' | \
  xargs -I {} curl -s --user dinero:password \
    --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"test\",\"method\":\"getblockheader\",\"params\":[\"{}\"]}"\
    -H 'content-type: text/plain;' http://127.0.0.1:18332/ | jq -r '.result.difficulty'
done
```

**Expected:**
- Difficulty values are reasonable (not 0, not infinite)
- Difficulty changes over time (not static)
- Smooth adjustments (no wild jumps)

- [ ] Difficulty values look correct
- [ ] No extreme values
- [ ] ASERT is adjusting as expected

### ✅ Step 4.5: Check Logs

```bash
# Check for canonical ASERT logs
tail -100 ~/.dinero_test/debug.log | grep -i "asert"
```

**Expected log messages:**
```
[ASERT-Canonical] height_diff=... time_diff=... offset=...
[ASERT-Canonical] exponent_q16=... (float: ...)
[ASERT-Canonical] shifts=... frac=...
[ASERT-Canonical] factor=... (2^frac approximation)
[ASERT-Canonical] Result: height=... bits=...
```

- [ ] Canonical ASERT logs present
- [ ] No error messages
- [ ] Calculations completing successfully

### ✅ Step 4.6: Stress Test (Optional but Recommended)

```bash
# Mine 1000 blocks rapidly to test adjustment
curl -s --user dinero:password \
  --data-binary "{\"jsonrpc\":\"1.0\",\"id\":\"test\",\"method\":\"generatetoaddress\",\"params\":[1000,\"$ADDRESS\"]}" \
  -H 'content-type: text/plain;' http://127.0.0.1:18332/
```

- [ ] Node handles 1000+ blocks
- [ ] No crashes or hangs
- [ ] Difficulty adjusts smoothly

---

## Phase 5: Validation ⏱️ 30 minutes

### ✅ Step 5.1: Compare with Old Implementation (Optional)

To verify the new implementation produces similar results:

```bash
# Build with old implementation (temporarily)
# 1. Revert function call to CalculateNextWork_ASERT_Production
# 2. Rebuild
# 3. Mine 200 blocks on clean testnet
# 4. Record difficulty values
# 5. Switch back to canonical implementation
# 6. Mine 200 blocks on clean testnet
# 7. Compare difficulty values

# Differences should be small (within floating-point rounding)
```

- [ ] Old vs new comparison done
- [ ] Results are very close (within 0.1%)
- [ ] Documented any significant differences

### ✅ Step 5.2: Multi-Platform Testing

Test on different platforms if available:
- [ ] Linux x86_64
- [ ] Linux ARM64
- [ ] macOS Intel
- [ ] macOS Apple Silicon
- [ ] Windows (if applicable)

**Critical:** All platforms must produce IDENTICAL difficulty values!

### ✅ Step 5.3: Code Review

- [ ] Reviewed by at least 2 developers
- [ ] Coefficients verified against Bitcoin Cash Node source
- [ ] Integer arithmetic verified (no floating-point)
- [ ] Edge cases tested (overflow, underflow, clamping)

---

## Phase 6: Deployment Preparation ⏱️ 1 hour

### ✅ Step 6.1: Set Activation Height

**File:** `src/consensus/params.h` or similar

```cpp
// ASERT canonical activation height
// All blocks >= this height use canonical integer ASERT
// All blocks < this height use old floating-point ASERT (for compatibility)
static constexpr int ASERT_CANONICAL_ACTIVATION_HEIGHT = 50000;
```

**Update ASERT call:**
```cpp
if (next_height >= ASERT_CANONICAL_ACTIVATION_HEIGHT) {
    // Use canonical integer implementation
    next_bits = CalculateNextWork_ASERT_Canonical(...);
} else {
    // Use old floating-point implementation (backwards compatibility)
    next_bits = CalculateNextWork_ASERT_Production(...);
}
```

- [ ] Activation height set
- [ ] Conditional logic implemented
- [ ] Backwards compatibility maintained

### ✅ Step 6.2: Update Tests

Add consensus tests for canonical ASERT:

**File:** `src/test/consensus_tests.cpp` or similar

```cpp
TEST(AsertCanonical, ZeroOffset) {
    uint32_t result = CalculateNextWork_ASERT_Canonical(
        9999, 1762072333, 0x1d31ffce, 1762072333,
        1762072333, 0x1d31ffce, 9999, 0x1f00ffff, 120, 43200
    );
    EXPECT_EQ(result, 0x1d31ffce);  // Should match anchor
}

TEST(AsertCanonical, PositiveOffset) {
    // Test late blocks (easier difficulty)
    // ...
}

TEST(AsertCanonical, NegativeOffset) {
    // Test early blocks (harder difficulty)
    // ...
}
```

- [ ] Unit tests added
- [ ] Tests pass
- [ ] Coverage includes edge cases

### ✅ Step 6.3: Documentation

Update project documentation:

- [ ] README.md: Mention ASERT uses canonical integer algorithm
- [ ] CHANGELOG.md: Document the change
- [ ] Consensus docs: Explain activation and rationale
- [ ] Developer docs: Link to ASERT_CANONICAL_FIX.md

### ✅ Step 6.4: Network Announcement

**Before activating on mainnet:**

- [ ] Announce upgrade to node operators
- [ ] Provide upgrade deadline
- [ ] Explain importance (consensus safety)
- [ ] Publish activation height
- [ ] Give at least 2 weeks notice

---

## Phase 7: Mainnet Deployment ⏱️ Ongoing

### ✅ Step 7.1: Pre-Activation

- [ ] All nodes upgraded to version with canonical ASERT
- [ ] Activation height announced and known
- [ ] Monitoring systems ready
- [ ] Rollback plan documented (though shouldn't be needed)

### ✅ Step 7.2: Activation Day

- [ ] Monitor blocks approaching activation height
- [ ] Watch for any anomalies
- [ ] Verify all nodes converging on same chain
- [ ] Check difficulty values are reasonable

### ✅ Step 7.3: Post-Activation (First 24 Hours)

- [ ] Monitor first 100 blocks after activation
- [ ] Verify no chain splits
- [ ] Check node convergence
- [ ] Document any unexpected behavior

### ✅ Step 7.4: Long-Term (First Week)

- [ ] Continue monitoring difficulty adjustments
- [ ] Verify network health
- [ ] Collect feedback from node operators
- [ ] Address any issues immediately

---

## Phase 8: Cleanup ⏱️ 30 minutes

### ✅ Step 8.1: Remove Old Code

**After activation and verification (at least 1000 blocks):**

```bash
# Remove old floating-point implementation
rm src/consensus/asert.cpp
rm include/consensus/asert.h
```

**Update CMakeLists.txt:**
```cmake
# Remove old file
# src/consensus/asert.cpp  # REMOVED - replaced by asert_canonical.cpp
```

**Update code:**
```cpp
// Remove conditional logic
// if (next_height >= ASERT_CANONICAL_ACTIVATION_HEIGHT) { ... }

// Just use canonical implementation everywhere
next_bits = CalculateNextWork_ASERT_Canonical(...);
```

- [ ] Old files removed
- [ ] Conditional logic removed
- [ ] Build still succeeds
- [ ] Tests still pass

### ✅ Step 8.2: Final Documentation

- [ ] Update CHANGELOG
- [ ] Mark old implementation as deprecated/removed
- [ ] Document activation date and height
- [ ] Archive old documentation

---

## Summary Checklist

### Critical Items (Must Complete Before Mainnet)

- [ ] ✅ Canonical implementation compiles
- [ ] ✅ Test program passes
- [ ] ✅ Coefficients verified against BCH Node
- [ ] ✅ Integrated into build system
- [ ] ✅ Tested on testnet (200+ blocks)
- [ ] ✅ Multi-platform testing completed
- [ ] ✅ Code reviewed by 2+ developers
- [ ] ✅ Activation height set
- [ ] ✅ Network upgrade announced
- [ ] ✅ All nodes upgraded before activation

### Nice-to-Have (Recommended)

- [ ] 📊 Performance benchmarking
- [ ] 📊 Comparison with old implementation
- [ ] 📊 Stress testing (1000+ blocks)
- [ ] 📊 Multiple testnet deployments
- [ ] 📖 Blog post explaining the fix
- [ ] 📖 Technical writeup for community

---

## Quick Reference Commands

```bash
# Build and test
cd /Users/haydarevich/Documents/DineroCoin
g++ -std=c++17 -I include src/consensus/asert_canonical.cpp test_asert_canonical.cpp -o test_asert
./test_asert

# Integrate and build
cmake --build build --target dinerod -j$(nproc)

# Test on testnet
rm -rf ~/.dinero_test/blocks ~/.dinero_test/chainstate
./bin/dinerod --testnet --daemon
curl -s --user dinero:password --data-binary '{"jsonrpc":"1.0","id":"test","method":"generatetoaddress","params":[200,"<address>"]}' -H 'content-type: text/plain;' http://127.0.0.1:18332/

# Check logs
tail -f ~/.dinero_test/debug.log | grep -i asert
```

---

## Support Resources

- **Full Documentation:** `ASERT_CANONICAL_FIX.md`
- **Audit Report:** `ASERT_AUDIT_REPORT.md`
- **Quick Summary:** `ASERT_FIX_SUMMARY.md`
- **Bitcoin Cash Node:** https://github.com/bitcoin-cash-node/bitcoin-cash-node
- **ASERT Spec:** `bitcoin-cash-node/doc/asert.md`

---

**Status:** Ready for integration
**Confidence:** HIGH (based on Bitcoin Cash Node canonical spec)
**Priority:** 🔴 CRITICAL (consensus safety issue)

**Good luck with the deployment!** 🚀
