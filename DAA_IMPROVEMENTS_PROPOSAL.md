# Difficulty Adjustment Algorithm - Improvements Proposal

**Date:** 2025-10-14
**Status:** 📋 PROPOSAL (Awaiting User Approval)
**Suggested By:** Technical review feedback

---

## Executive Summary

This proposal improves DineroCoin's difficulty adjustment to handle extreme hashrate changes during Phase 1 and provides a smoother transition to Phase 2.

**Current Implementation:**
- Phase 1: Fixed difficulty (0x1d3fffff)
- Phase 2: Bitcoin-style DAA (adjusts every 2016 blocks)

**Proposed Improvements:**
1. **Phase 1 Guardrails:** Anti-stall and anti-runaway protections
2. **Phase 2 DAA:** Per-block adjustment (ASERT or LWMA) OR seeded Bitcoin DAA
3. **Consensus-level enforcement:** No manual overrides needed

---

## Problem Analysis

### Current Vulnerabilities

#### **Phase 1 Issues**

**A. Hashrate Stall (Miners Leave)**
```
Scenario: Network has 100 miners, 99 leave
Current behavior:
- Difficulty stays fixed at 0x1d3fffff
- Only 1 miner left → blocks take HOURS or DAYS
- Network is unusable until hashrate returns

Example:
- 100 miners: blocks every 2 minutes
- 1 miner: blocks every 200 minutes (3.3 hours)
- Network stuck for potentially DAYS
```

**B. Hashrate Surge (ASICs Join)**
```
Scenario: Network has 10 miners, 1000 join (100x spike)
Current behavior:
- Difficulty stays fixed at 0x1d3fffff
- Blocks come extremely fast (every 1-2 seconds)
- Timestamp manipulation possible
- Blockchain bloat

Example:
- 10 miners: blocks every 5 minutes
- 1000 miners: blocks every 3 seconds
- Phase 1 completes in HOURS instead of weeks/months
```

#### **Phase 2 Transition Issues**

**C. 2016-Block Adjustment Window**
```
Scenario: Transition from Phase 1 → Phase 2 at block 180,002
Current behavior:
- First adjustment at block 182,018 (2016 blocks later)
- If hashrate changed during Phase 1, network stuck for 2016 blocks

Example:
- Phase 1 ended with high hashrate (blocks every 30 sec)
- Phase 2 starts with same hashrate
- 2016 blocks × 30 sec = 16.8 hours of fast blocks
- Network unusable for almost a full day
```

---

## Proposed Solutions

### 1. Phase 1 Guardrails (Anti-Stall + Anti-Runaway)

#### **A. Anti-Stall Protection**

**Purpose:** Prevent network from stalling if miners leave

**Consensus Rule:**
```cpp
// src/consensus/pow.hpp
uint32_t GetNextWorkRequired_Phase1(const CBlockIndex* prev, const Consensus& c) {
    // Phase 1 base difficulty
    uint32_t base_bits = c.easyPhaseBits;  // 0x1d3fffff

    // Anti-stall: Allow easier mining if chain stalls
    int64_t time_since_prev = GetMedianTimePast(prev) - GetMedianTimePast(prev->pprev);
    int64_t stall_threshold = c.antiStallMultiplier * c.targetSpacingSec;
    //                       = 20 * 300 = 6000 seconds (100 minutes)

    if (time_since_prev >= stall_threshold) {
        // Emergency: Allow minimum difficulty for ONE block
        return c.minDifficultyBits;  // 0x1f00ffff (much easier)
    }

    // Normal: Return fixed Phase 1 difficulty
    return base_bits;
}
```

**Parameters:**
```cpp
// src/consensus/chainparams.h
struct Consensus {
    uint32_t antiStallMultiplier = 20;     // Trigger after 20 × 5min = 100 minutes
    uint32_t minDifficultyBits = 0x1f00ffff;  // Emergency difficulty floor
    uint32_t targetSpacingSec = 300;       // 5 minutes nominal
};
```

**Behavior:**
```
Normal operation:
- Difficulty: 0x1d3fffff (fixed)
- Blocks arrive every ~2-5 minutes (depends on hashrate)

Hashrate drops 99%:
- Block N arrives at time T
- Block N+1 not found after 100 minutes
- Block N+2 can use emergency difficulty (0x1f00ffff - MUCH easier)
- Block N+2 found within minutes
- Block N+3 returns to normal difficulty (0x1d3fffff)

Recovery: Automatic, one block at a time
```

#### **B. Anti-Runaway Protection**

**Purpose:** Prevent timestamp manipulation and blockchain bloat from hashrate surges

**Consensus Rules:**
```cpp
// src/consensus/block_validation.cpp
bool CheckBlockTimestamp(const CBlockHeader& block, const CBlockIndex* prev) {
    int64_t mtp_prev = GetMedianTimePast(prev);
    int64_t now = GetAdjustedTime();

    // Rule 1: Block time must advance past MTP
    if (block.nTime <= mtp_prev) {
        return error("Block timestamp too early (≤ MTP)");
    }

    // Rule 2: Block time cannot be more than 2 hours in the future
    if (block.nTime > now + 7200) {  // 2 hours
        return error("Block timestamp too far in future");
    }

    // Rule 3: Enforce minimum inter-block time (prevents spam)
    if (prev != nullptr) {
        int64_t time_since_prev = block.nTime - prev->nTime;
        if (time_since_prev < 1) {  // At least 1 second apart
            return error("Blocks too close in time");
        }
    }

    return true;
}
```

**Behavior:**
```
Hashrate surge (1000 miners join):
- Blocks arrive every 1-2 seconds
- BUT: Timestamps must still advance properly
- MTP prevents "rewinding" time
- 2-hour future limit prevents clock manipulation
- 1-second minimum prevents instant spam

Effect:
- Even with high hashrate, blocks are rate-limited by time
- Phase 1 duration: minimum ~180,000 seconds = 50 hours (not 3 hours)
- Prevents worst-case blockchain bloat
```

---

### 2. Phase 2 DAA Options (Choose ONE)

#### **Option A: ASERT (Anchor-based Smooth Elastic Retargeting)**

**Recommended ✅**

**Why ASERT:**
- Per-block adjustment (reacts immediately)
- Smooth difficulty changes (no sudden jumps)
- Proven in Bitcoin Cash and Zcash
- Simple exponential formula

**Formula:**
```cpp
// src/consensus/pow_asert.hpp
arith_uint256 CalculateASERT(
    const CBlockIndex* prev,
    const CBlockIndex* anchor,
    const Consensus& c
) {
    // Parameters
    int64_t T = c.targetSpacingSec;           // 300 seconds (5 minutes)
    int64_t half_life = c.asertHalfLife;      // 144 * 300 = 43,200 sec (12 hours)

    // Time and block deltas since anchor
    int64_t time_delta = prev->nTime - anchor->nTime;
    int64_t block_delta = prev->nHeight - anchor->nHeight;

    // Ideal time (if blocks came every T seconds)
    int64_t ideal_time = block_delta * T;

    // Excess time (positive = slow, negative = fast)
    int64_t excess_time = time_delta - ideal_time;

    // Exponential adjustment
    // new_target = anchor_target * 2^(excess_time / half_life)
    arith_uint256 anchor_target = arith_uint256().SetCompact(anchor->nBits);

    // Calculate exponent (fixed-point arithmetic)
    int64_t exponent_fp = (excess_time << 16) / half_life;  // 16-bit fractional
    arith_uint256 new_target = anchor_target;

    if (exponent_fp != 0) {
        // Apply 2^(exponent_fp / 2^16) using bit shifts
        new_target = (exponent_fp >= 0)
            ? anchor_target << (exponent_fp >> 16)   // Easier (slow blocks)
            : anchor_target >> ((-exponent_fp) >> 16);  // Harder (fast blocks)
    }

    // Clamp to powLimit
    if (new_target > c.powLimit) {
        new_target = c.powLimit;
    }

    return new_target;
}
```

**Anchor Configuration:**
```cpp
// src/consensus/chainparams.h
struct Consensus {
    // ASERT parameters
    uint32_t asertAnchorHeight = 180001;      // Last Phase 1 block
    uint32_t asertAnchorBits = 0x1d3fffff;    // Phase 1 fixed difficulty
    int64_t asertAnchorTime = 0;              // Set at runtime (block 180,001 timestamp)
    int64_t asertHalfLife = 144 * 300;        // 12 hours
};
```

**Implementation:**
```cpp
// src/consensus/pow.hpp
uint32_t GetNextWorkRequired(const CBlockIndex* prev, const Consensus& c) {
    uint32_t height = prev->nHeight + 1;

    // Phase 1: Fixed difficulty with anti-stall
    if (height <= c.easyPhaseHeight) {
        return GetNextWorkRequired_Phase1(prev, c);
    }

    // Phase 2: ASERT per-block adjustment
    const CBlockIndex* anchor = GetBlockIndex(c.asertAnchorHeight);
    arith_uint256 new_target = CalculateASERT(prev, anchor, c);
    return new_target.GetCompact();
}
```

**Benefits:**
- ✅ Immediate response to hashrate changes (every block)
- ✅ Smooth exponential adjustment (no oscillation)
- ✅ Continuous difficulty at Phase 1 → Phase 2 transition
- ✅ No "warm-up" period needed

**Drawbacks:**
- More complex math (exponentials)
- Requires careful parameter tuning (half_life)

---

#### **Option B: LWMA (Linearly Weighted Moving Average)**

**Alternative (Simpler)**

**Why LWMA:**
- Per-block adjustment (like ASERT)
- Simple integer arithmetic (no exponentials)
- Proven in multiple altcoins (Zawy's algorithm)
- Resistant to timestamp manipulation

**Formula:**
```cpp
// src/consensus/pow_lwma.hpp
uint32_t CalculateLWMA(const CBlockIndex* prev, const Consensus& c) {
    const int N = c.lwmaWindow;  // 144 blocks
    const int T = c.targetSpacingSec;  // 300 seconds

    // Collect last N blocks
    const CBlockIndex* block = prev;
    std::vector<int64_t> timestamps;
    std::vector<int64_t> cumulative_targets;

    for (int i = 0; i < N && block != nullptr; i++) {
        timestamps.push_back(block->nTime);

        arith_uint256 target;
        target.SetCompact(block->nBits);

        if (i == 0) {
            cumulative_targets.push_back(target.GetLow64());
        } else {
            cumulative_targets.push_back(cumulative_targets[i-1] + target.GetLow64());
        }

        block = block->pprev;
    }

    int actual_N = timestamps.size();
    if (actual_N < 2) {
        // Not enough blocks, use previous difficulty
        return prev->nBits;
    }

    // Calculate weighted average
    int64_t sum_weighted_solvetimes = 0;
    int64_t sum_weights = 0;

    for (int i = 1; i < actual_N; i++) {
        int64_t solvetime = timestamps[i-1] - timestamps[i];

        // Clamp solvetime to prevent timestamp manipulation
        solvetime = std::max(solvetime, (int64_t)(-6 * T));
        solvetime = std::min(solvetime, (int64_t)(7 * T));

        int64_t weight = i;  // Linearly increasing weight (recent blocks matter more)
        sum_weighted_solvetimes += solvetime * weight;
        sum_weights += weight;
    }

    int64_t avg_solvetime = sum_weighted_solvetimes / sum_weights;

    // Adjust target based on avg_solvetime vs T
    arith_uint256 prev_target;
    prev_target.SetCompact(prev->nBits);

    arith_uint256 new_target = prev_target * avg_solvetime / T;

    // Clamp to powLimit
    if (new_target > c.powLimit) {
        new_target = c.powLimit;
    }

    return new_target.GetCompact();
}
```

**Parameters:**
```cpp
// src/consensus/chainparams.h
struct Consensus {
    int lwmaWindow = 144;  // Use last 144 blocks (~12 hours if 5 min/block)
};
```

**Benefits:**
- ✅ Per-block adjustment
- ✅ Simple integer math (no exponentials)
- ✅ Weighted toward recent blocks (reacts quickly)
- ✅ Built-in solvetime clamping (anti-manipulation)

**Drawbacks:**
- Requires N blocks of history (bootstrapping issue at Phase 2 start)
- Slightly more oscillation than ASERT

---

#### **Option C: Seeded Bitcoin DAA (Minimal Change)**

**If you want to keep Bitcoin's 2016-block DAA:**

**Improvement:** Seed the initial Phase 2 difficulty based on recent Phase 1 block times

**Implementation:**
```cpp
// src/consensus/pow.hpp
uint32_t GetNextWorkRequired(const CBlockIndex* prev, const Consensus& c) {
    uint32_t height = prev->nHeight + 1;

    // Phase 1: Fixed difficulty with anti-stall
    if (height <= c.easyPhaseHeight) {
        return GetNextWorkRequired_Phase1(prev, c);
    }

    // Phase 2: Bitcoin DAA with seeded start
    if (height == c.easyPhaseHeight + 1) {
        // First Phase 2 block: Calculate seeded difficulty
        return CalculateSeededDifficulty(prev, c);
    }

    // Normal Bitcoin DAA (adjusts every 2016 blocks)
    return CalculateBitcoinDAA(prev, c);
}

uint32_t CalculateSeededDifficulty(const CBlockIndex* prev, const Consensus& c) {
    const int K = 144;  // Sample last 144 blocks
    const int T = c.targetSpacingSec;  // 300 seconds

    // Calculate average block time over last K blocks
    const CBlockIndex* first = prev;
    const CBlockIndex* last = prev;

    for (int i = 0; i < K && first->pprev != nullptr; i++) {
        first = first->pprev;
    }

    int64_t time_span = last->nTime - first->nTime;
    int block_count = last->nHeight - first->nHeight;

    if (block_count == 0) {
        return c.easyPhaseBits;  // Fallback
    }

    int64_t avg_time = time_span / block_count;

    // Clamp to prevent extreme adjustments
    avg_time = std::max(avg_time, (int64_t)(T / 4));   // Min 75 sec (4x harder)
    avg_time = std::min(avg_time, (int64_t)(20 * T));  // Max 100 min (20x easier)

    // Adjust Phase 1 difficulty based on average time
    arith_uint256 phase1_target;
    phase1_target.SetCompact(c.easyPhaseBits);

    arith_uint256 new_target = phase1_target * avg_time / T;

    // Clamp to powLimit
    if (new_target > c.powLimit) {
        new_target = c.powLimit;
    }

    return new_target.GetCompact();
}
```

**Behavior:**
```
Block 180,001 (last Phase 1):
- Difficulty: 0x1d3fffff (fixed)

Block 180,002 (first Phase 2):
- Sample last 144 Phase 1 blocks
- Calculate average block time: e.g., 120 seconds (2 min)
- Seed difficulty: 0x1d3fffff * (120 / 300) = easier (40% of Phase 1)
- Use seeded difficulty for blocks 180,002 - 182,017

Block 182,018:
- First Bitcoin DAA adjustment
- Continue normal 2016-block retargeting
```

**Benefits:**
- ✅ One-time jump matches current hashrate
- ✅ Minimal code change (keep Bitcoin DAA)
- ✅ No 2016-block "stuck" period

**Drawbacks:**
- ❌ Still adjusts only every 2016 blocks (slow response)
- ❌ First 2016 blocks could still be problematic if hashrate changes

---

## Recommended Implementation

### **Phase 1 (Heights 2 - 180,001)**

```cpp
uint32_t GetNextWorkRequired_Phase1(const CBlockIndex* prev, const Consensus& c) {
    // Anti-stall protection
    int64_t time_since_prev = GetMedianTimePast(prev) - GetMedianTimePast(prev->pprev);

    if (time_since_prev >= c.antiStallMultiplier * c.targetSpacingSec) {
        return c.minDifficultyBits;  // Emergency difficulty
    }

    return c.easyPhaseBits;  // Normal Phase 1 difficulty
}
```

**Plus timestamp validation (anti-runaway):**
```cpp
bool CheckBlockTimestamp(const CBlockHeader& block, const CBlockIndex* prev) {
    int64_t mtp_prev = GetMedianTimePast(prev);
    int64_t now = GetAdjustedTime();

    // Must advance past MTP
    if (block.nTime <= mtp_prev) return false;

    // Cannot be > 2 hours in future
    if (block.nTime > now + 7200) return false;

    // At least 1 second apart
    if (prev && block.nTime - prev->nTime < 1) return false;

    return true;
}
```

### **Phase 2 (Heights 180,002+)**

**Option A (Recommended): ASERT**
```cpp
uint32_t GetNextWorkRequired_Phase2(const CBlockIndex* prev, const Consensus& c) {
    const CBlockIndex* anchor = GetBlockIndex(c.asertAnchorHeight);
    arith_uint256 new_target = CalculateASERT(prev, anchor, c);
    return new_target.GetCompact();
}
```

**Parameters:**
- Anchor: Block 180,001 (last Phase 1 block)
- Half-life: 144 blocks (12 hours at 5 min/block)
- Target spacing: 300 seconds (5 minutes)

---

## Testing Requirements

### **Test 1: Phase 1 Anti-Stall**

```bash
#!/bin/bash
# test_anti_stall.sh

# Mine to Phase 1
./dinero-cli --regtest generatetoaddress 100 $ADDR

# Stop mining, advance system clock 110 minutes
# (simulates network stall)

# Next block should be allowed with emergency difficulty
RESULT=$(./dinero-cli --regtest getblock $(./dinero-cli --regtest getblockhash 101))
BITS=$(echo $RESULT | jq -r '.bits')

if [ "$BITS" = "0x1f00ffff" ]; then
    echo "✅ Anti-stall activated correctly"
else
    echo "❌ Anti-stall FAILED: expected 0x1f00ffff, got $BITS"
fi
```

### **Test 2: Phase 1 Anti-Runaway**

```bash
# Simulate hashrate surge
# Try to mine 1000 blocks in 1 hour
# Verify: Timestamps are properly spaced (MTP enforcement)
# Verify: Blocks rejected if timestamps violate rules
```

### **Test 3: Phase 2 ASERT Continuity**

```bash
# Mine to block 180,001
# Check difficulty at 180,002 (should be continuous, not sudden jump)
# Verify ASERT adjusts smoothly over next 100 blocks
```

### **Test 4: Adversarial Timestamp Manipulation**

```bash
# Try to set block.nTime = MTP - 1 (should be rejected)
# Try to set block.nTime = now() + 3 hours (should be rejected)
# Try to mine 2 blocks with same timestamp (should be rejected)
```

---

## Migration Plan

### **Step 1: Code Implementation (Pre-Deployment)**

1. Implement anti-stall in Phase 1 ✅
2. Implement anti-runaway (timestamp checks) ✅
3. Choose Phase 2 DAA (ASERT recommended) ✅
4. Write unit tests ✅
5. Write integration tests ✅

### **Step 2: Testnet Validation**

1. Deploy to regtest/testnet
2. Simulate hashrate stall scenarios
3. Simulate hashrate surge scenarios
4. Validate Phase 1 → Phase 2 transition
5. Verify difficulty converges to target

### **Step 3: Mainnet Deployment**

1. Release v2.0.0 with improved DAA
2. Set activation height (if not genesis)
3. Community upgrade timeline
4. Monitor network health post-activation

---

## Summary Table

| Feature | Current | Proposed (ASERT) | Proposed (Seeded BTC DAA) |
|---------|---------|-----------------|--------------------------|
| **Phase 1 Stall Protection** | ❌ None | ✅ Auto min-diff after 100 min | ✅ Auto min-diff after 100 min |
| **Phase 1 Surge Protection** | ❌ None | ✅ MTP + timestamp limits | ✅ MTP + timestamp limits |
| **Phase 2 Adjustment** | Every 2016 blocks | ✅ Every block (ASERT) | Every 2016 blocks |
| **Transition Smoothness** | ❌ Potential 2016-block pain | ✅ Immediate (continuous) | ⚠️ One-time seed (then 2016) |
| **Complexity** | Low | Medium (exponentials) | Low (simple seed + BTC DAA) |
| **Battle-tested** | ✅ Bitcoin (15 yrs) | ✅ BCH, Zcash (5 yrs) | ✅ Bitcoin (15 yrs) |

---

## Decision Points

### **Must Decide:**

1. **Phase 1 Guardrails:**
   - ✅ **Recommend: Implement anti-stall + anti-runaway** (protects against worst-case scenarios)
   - ❌ Alternative: Keep Phase 1 as-is (risky if hashrate is volatile)

2. **Phase 2 DAA:**
   - ✅ **Option A (Recommended): ASERT** - Per-block, smooth, proven
   - ⚠️ **Option B (Alternative): LWMA** - Per-block, simple math, slightly more oscillation
   - ⚠️ **Option C (Minimal Change): Seeded Bitcoin DAA** - Keep 2016-block epochs, seed initial difficulty

3. **Activation:**
   - If pre-launch: Activate at genesis (height 0)
   - If post-launch: Activate at specific height (hard fork)

---

**Status:** 📋 **AWAITING USER DECISION**
**Next Steps:**
1. User reviews proposal
2. User selects preferred Phase 2 DAA (ASERT / LWMA / Seeded BTC)
3. Implement chosen option
4. Test thoroughly
5. Deploy

**Last Updated:** 2025-10-14
