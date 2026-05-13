# Phase E: Economics Layer - Implementation Plan

## Executive Summary

**Status:** Middle layer FROZEN (v0.12.0-middle-layer-complete)
**Next:** Economics layer validation and testing
**Duration:** 2-3 weeks
**Goal:** Complete economic rules validation and testing

## DineroCoin Monetary Policy (Canonical)

**From `include/consensus/subsidy.h` (CANONICAL):**

```
Hard Cap: 265,428,000 DIN (total supply)
Genesis (height 0): 100 DIN (unspendable, OP_RETURN)
Premine (height 1): 2,627,900 DIN (~0.99% of total supply)
PoW-mineable (height 2+): 262,800,000 DIN (from halving schedule)
Decimals: 1 DIN = 100,000,000 una (Bitcoin standard)

Block Reward Schedule:
- Initial reward: 100 DIN
- Halving interval: 1,314,000 blocks (5.0 years @ 2 min blocks)
- Halving count: 33 halvings until subsidy reaches 0
- Final halving: Block 43,362,000
```

**Key Discovery:** Subsidy schedule is **already fully implemented** in `include/consensus/subsidy.h` as a compile-time constexpr struct with static assertions.

## What's Already Implemented

### ✅ E.1: Subsidy Schedule (COMPLETE)
**Location:** `include/consensus/subsidy.h:79-93`

```cpp
static uint64_t GetBlockSubsidy(uint32_t height) {
    if (height <= PREMINE_HEIGHT) {
        return 0;  // Genesis and premine handled separately
    }

    // PoW blocks start at height 2
    uint32_t pow_blocks = height - 2;
    uint32_t halvings = pow_blocks / HALVING_INTERVAL;

    if (halvings >= 33) return 0;  // After 33 halvings, no more rewards
    return INITIAL_SUBSIDY >> halvings;  // Right-shift for exact halving
}
```

**Compile-time Guards:**
- `static_assert(INITIAL_SUBSIDY == 10000000000ULL)` - 100 DIN
- `static_assert(HALVING_INTERVAL == 1314000)` - 5.0 years
- `static_assert(MAX_SUPPLY_UNA == 26280000000000000ULL)` - 262.8M DIN
- `static_assert(PREMINE_UNA == 262790000000000ULL)` - 2,627,900 DIN

**Integration:** `src/consensus/block_validation.cpp:107-119`
- Coinbase validation: `coinbase_value <= subsidy + total_fees`
- Rejects blocks with excess subsidy

### ✅ E.2: Fee Policy (COMPLETE - Needs Verification)
**Location:** Multiple files from Phase F.9

**Min Relay Fee:** `chainparams_impl.cpp:55`
- Mainnet: 1000 una/kb
- Testnet: 1000 una/kb
- Regtest: 1000 una/kb

**CPFP Support:** Implemented in F.9.6
- Ancestor/descendant tracking
- Ancestor fee rate for mining
- 25 ancestor limit, 25 descendant limit

**RBF Support:** Need to verify (likely exists)

---

## Phase Breakdown

### **Phase E.1: Subsidy Schedule Testing** (Week 1, Days 1-3)

**Goal:** Comprehensive test coverage for subsidy schedule

**Status:** Subsidy logic is COMPLETE, tests are MISSING

**Tasks:**
1. **Create `tests/consensus/test_subsidy_schedule.cpp`**
   - Test 1: Genesis and premine (heights 0-1 return 0)
   - Test 2: First PoW block (height 2 = 100 DIN)
   - Test 3: All 33 halving boundaries
   - Test 4: Subsidy reaches 0 after 33rd halving
   - Test 5: GetPoWIssuedAtHeight() accuracy
   - Test 6: GetTotalIssuedAtHeight() includes all components

2. **Halving Schedule Validation**
   ```
   Height Range          | PoW Blocks      | Subsidy
   ----------------------|-----------------|----------
   0-1                   | N/A             | Special (genesis/premine)
   2-1,314,001           | 1-1,314,000     | 100 DIN
   1,314,002-2,628,001   | 1,314,001-...   | 50 DIN
   2,628,002-3,942,001   | ...             | 25 DIN
   ...                   | ...             | ...
   43,362,000+           | 43,361,999+     | 0 DIN (tail emission phase)
   ```

3. **Coinbase Validation Testing**
   - Test valid coinbase (subsidy + fees)
   - Test excess coinbase (should fail)
   - Test boundary blocks (first block of each halving)
   - Test maximum fee block

**Files to create:**
- `tests/consensus/test_subsidy_schedule.cpp`

**Success criteria:**
- ✅ All halving boundaries correct
- ✅ No subsidy after block 43,362,000
- ✅ Coinbase validation enforces subsidy cap
- ✅ Total issuance calculation matches expected value

---

### **Phase E.2: Fee Policy Verification** (Week 1, Days 4-5)

**Goal:** Verify existing fee market implementation

**Tasks:**
1. **Minimum Relay Fee Verification**
   - Status: Configured in chainparams (1000 una/kb)
   - Verify: Mempool rejects transactions below min fee
   - Test: Submit tx with 999 una/kb → rejected

2. **CPFP Verification**
   - Status: Implemented in F.9.6 (ancestor tracking)
   - Verify: Mempool sorts by ancestor fee rate
   - Test: Low-fee parent + high-fee child package

3. **RBF Verification**
   - Status: Need to check implementation
   - Verify: BIP 125 rules enforced
   - Test: Valid RBF (higher fee), invalid RBF (same fee)

**Files to check:**
- `src/mempool/mempool.cpp`
- `tests/mempool/test_fee_policy.cpp` (may need to create)

**Success criteria:**
- ✅ Min relay fee enforced (policy layer)
- ✅ CPFP works (ancestor fee rate)
- ✅ RBF rules verified (if implemented)

---

### **Phase E.3: Supply Cap Validation** (Week 2, Days 1-3)

**Goal:** Prove DineroCoin cannot exceed 265.428M supply

**Tasks:**
1. **Total Supply Calculation Verification**
   - Use `ConsensusSubsidy::GetTotalIssuedAtHeight()`
   - Verify at key heights:
     - Height 0: 100 DIN (genesis, unspendable)
     - Height 1: 100 + 2,627,900 = 2,628,000 DIN
     - Height 2: 2,628,000 DIN (PoW block not yet counted)
     - Final height (43,362,002): 265,428,000 DIN (total supply cap)

2. **Supply Invariant Enforcement**
   - Create `SupplyValidator` class
   - Check: `GetTotalIssuedAtHeight(height) <= MAX_SUPPLY_UNA`
   - Test: Iterate key heights, verify supply never exceeds cap

3. **Long-tail Behavior**
   - After height 43,362,000: subsidy = 0
   - No new coins created (only fees)
   - Test: Coinbase after final halving has subsidy = 0

**Files to create:**
- `include/consensus/supply_validator.h`
- `src/consensus/supply_validator.cpp`
- `tests/consensus/test_supply_cap.cpp`

**Success criteria:**
- ✅ Total supply provably ≤ 265.428M DIN
- ✅ No inflation after final halving (height 43,362,002)
- ✅ Supply calculation matches expected value at all heights

---

### **Phase E.4: Economic Stress Testing** (Week 2, Days 4-5)

**Goal:** Test edge cases and adversarial scenarios

**Tasks:**
1. **Maximum Fee Block**
   - Scenario: Block with very high fees
   - Check: `coinbase_value <= subsidy + total_fees` still holds
   - Test: Create block with 10,000 DIN in fees

2. **Zero-Fee Transactions**
   - Policy: Mempool rejects (below min relay fee)
   - Consensus: Valid if mined
   - Test: Mine block with zero-fee tx → valid

3. **Dust Threshold Economics**
   - Current: 546 una (Bitcoin standard)
   - Check: Outputs below dust are non-standard (policy)
   - Check: But valid if mined (consensus)
   - Test: Create 545 una output → rejected by mempool, accepted in block

4. **Subsidy Overflow Protection**
   - Verify: No overflow in subsidy calculation
   - Verify: No overflow in total issuance calculation
   - Test: All calculations use uint64_t safely

**Files to create:**
- `tests/consensus/test_economic_edge_cases.cpp`

**Success criteria:**
- ✅ High-fee blocks validate correctly
- ✅ Zero-fee policy vs consensus distinction clear
- ✅ Dust handling matches Bitcoin
- ✅ No overflow in economic calculations

---

### **Phase E.5: Premine Verification** (Week 3)

**Goal:** Verify genesis and premine match expected values

**Tasks:**
1. **Genesis Block Verification (Height 0)**
   - Expected: 100 DIN burned via OP_RETURN
   - Coinbase hex: From `chainparams_impl.cpp:71-79`
   - Message: "Dinero Genesis Burn"
   - Verify: Output is unspendable (OP_RETURN script)

2. **Premine Block Verification (Height 1)**
   - Expected: 2,627,900 DIN (exactly 1% of 262.8M minus 100 DIN)
   - Type: P2WPKH output (spendable)
   - Location: Check `genesis_premine.h` for address

3. **Premine Balance Query**
   - Query: Sum all UTXOs from block 1
   - Expected: 2,627,900 DIN (262,790,000,000,000 una)
   - Verify: Balance matches `ConsensusSubsidy::PREMINE_UNA`

4. **Compile-time Guards Verification**
   - Verify all `static_assert` statements pass
   - These guards prevent silent consensus changes:
     ```cpp
     static_assert(PREMINE_UNA == 262790000000000ULL);
     static_assert(PREMINE_HEIGHT == 1);
     static_assert(GENESIS_UNSPENDABLE_UNA == 10000000000ULL);
     ```

**Files to check:**
- `src/consensus/chainparams_impl.cpp` (genesis block)
- `include/consensus/genesis_premine.h` (premine structure)
- `tests/consensus/test_premine.cpp` (create)

**Success criteria:**
- ✅ Genesis block has 100 DIN burned (OP_RETURN)
- ✅ Premine block has 2,627,900 DIN (P2WPKH)
- ✅ Premine balance verifiable on-chain
- ✅ All compile-time guards pass

---

### **Phase E.6: Fee Estimation** (Optional - Defer)

**Status:** OPTIONAL (UX improvement, not consensus-critical)

**Defer to:** Post-mainnet or Phase G (Networking)

**Why defer:**
- Not required for consensus validation
- UX improvement only
- Users can manually set fees during early network operation

---

## Implementation Timeline

**Total Duration:** 2-3 weeks

### **Week 1: Testing & Verification**
- Days 1-3: E.1 (Subsidy schedule testing)
- Days 4-5: E.2 (Fee policy verification)

### **Week 2: Supply & Edge Cases**
- Days 1-3: E.3 (Supply cap validation)
- Days 4-5: E.4 (Economic stress testing)

### **Week 3: Premine & Final Validation**
- Days 1-3: E.5 (Premine verification)
- Days 4-5: Integration testing & documentation

---

## Critical Findings

### 1. Subsidy Logic is Already Complete ✅

The subsidy schedule is **fully implemented** in `include/consensus/subsidy.h`:
- Correct halving logic (PoW blocks start at height 2)
- Compile-time static assertions for safety
- Integration with coinbase validation

**What's missing:** Comprehensive test coverage

### 2. Genesis Structure Confirmed

**Genesis (Height 0):**
```
Coinbase: 100 DIN
Type: OP_RETURN (unspendable)
Message: "Dinero Genesis Burn"
```

**Premine (Height 1):**
```
Coinbase: 2,627,900 DIN
Type: P2WPKH (spendable)
Percentage: Exactly 1% of 262.8M minus 100 DIN genesis
```

This is **NOT** a traditional "premine in genesis" - DineroCoin separates:
- Genesis (height 0): Symbolic burn
- Premine (height 1): Actual founder allocation
- PoW blocks (height 2+): Mining rewards

### 3. Supply Cap Corrected (RESOLVED 2025-12-17)

**Original Issue:** PoW halving schedule produced 262.8M DIN, causing total supply to exceed 262.8M cap.

**Resolution:** MAX_SUPPLY adjusted to **265,428,000 DIN** to accommodate actual halving output.

Total supply: **265,428,000 DIN** includes everything:
- Genesis: 100 DIN (burned)
- Premine: 2,627,900 DIN (~0.99% of total)
- PoW: 262,800,000 DIN (from halving schedule)

This is enforced by compile-time assertion:
```cpp
static_assert(
    MAX_SUPPLY_UNA == GENESIS_UNSPENDABLE_UNA + PREMINE_UNA + MAX_POW_MINEABLE_UNA,
    "Total supply must equal genesis + premine + PoW-mineable supply"
);
// Passes: 265.428M = 100 + 2.6279M + 262.8M ✅
```

**See:** `docs/MONETARY_POLICY_FINDING.md` for detailed analysis and resolution.

---

## Success Criteria

After Phase E completion, DineroCoin will have:

### **Monetary Policy Validation**
- ✅ Subsidy schedule tested (all 33 halvings)
- ✅ Supply cap enforced (265.428M DIN total)
- ✅ No inflation after final halving (height 43,362,000)
- ✅ Genesis and premine verified on-chain

### **Fee Market**
- ✅ Minimum relay fee verified (1000 una/kb)
- ✅ CPFP support verified (ancestor fee rate)
- ✅ RBF support verified (if implemented)

### **Economic Invariants**
- ✅ Coinbase value ≤ subsidy + fees (enforced)
- ✅ No subsidy after block 43,362,000
- ✅ Total supply provably capped at 265.428M DIN
- ✅ No overflow in economic calculations

### **Bitcoin Core Equivalency**
- ✅ Halving schedule (Bitcoin-style, DineroCoin parameters)
- ✅ Fee market (ancestor-based, CPFP ready)
- ✅ Supply cap (provably finite)
- ✅ Economic stress tests (edge cases covered)

---

## What's NOT Included (Out of Scope)

- **Tail emission:** DineroCoin follows Bitcoin (subsidy reaches 0, fees only)
- **Demurrage:** No negative interest rate
- **Dynamic supply:** No algorithmic supply adjustments
- **Fee burning:** All fees go to miners
- **Premine vesting:** No lockup or vesting schedule (P2WPKH, immediately spendable)
- **Fee estimation:** Deferred to post-mainnet (optional UX)

These are policy decisions already made (or explicitly rejected).

---

## After Phase E: What's Next?

**Phase G: Networking Layer** (Deferred from Phase F)
- Peer discovery & sync
- Block propagation optimization
- Bloom filters (BIP 37) - SPV wallet support
- Compact blocks (BIP 152) - Bandwidth optimization

**Phase L: Lightning Network**
- Channel lifecycle
- HTLC routing
- Watchtowers

---

## Final Checklist

Before starting Phase E:
- ✅ Middle layer tagged (v0.12.0-middle-layer-complete)
- ✅ Economics branch created (feature/v0.13.0-economics)
- ✅ Monetary policy understood (262.8M cap, 100 DIN subsidy, 1.314M halving)
- ✅ Subsidy implementation validated (already complete)
- ✅ Implementation order defined (testing-focused, not building)

**Phase E is ready to begin - focus on testing and validation, not implementation.**

---

## Notes

This plan assumes:
1. Subsidy schedule already correctly implemented ✅
2. Coinbase validation already functional ✅
3. Fee market already functional (F.9 complete) ✅
4. Genesis and premine already in blockchain ✅
5. Need to **test and validate**, not build from scratch

**Key difference from original plan:** This is primarily a **validation and testing phase**, not an implementation phase.
