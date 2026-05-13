# Difficulty Adjustment Algorithm - Final Implementation

**Date:** 2025-10-14
**Status:** ✅ IMPLEMENTED (Phase 1 Guardrails + ASERT Phase 2)

---

## Executive Summary

DineroCoin uses a **hybrid difficulty adjustment system** with liveness guarantees:

- **Phase 1** (Heights 1-180,000): Fixed CPU-friendly difficulty with anti-stall protection
- **Phase 2** (Heights 180,001+): ASERT per-block adjustment anchored at block 180,000

---

## Consensus Parameters (CANONICAL)

```cpp
// src/consensus/consensus.hpp

static constexpr int64_t COIN = 1'000'000;  // 6 decimals

struct Consensus {
    // ========== PHASE 1: CPU-FRIENDLY ==========
    uint32_t easyPhaseStart      = 1;           // Right after genesis
    uint32_t easyPhaseBlocks     = 180'000;     // 180,000 blocks
    uint32_t easyPhaseEnd        = 180'000;     // Last Phase 1 block
    uint32_t easyPhaseBits       = 0x1d3fffff;  // Fixed difficulty

    uint32_t targetSpacingSec    = 300;         // 5 minutes

    // ========== PHASE 1 GUARDRAILS ==========
    uint32_t antiStallMultiplier = 20;          // Rescue after 100 min
    uint32_t minDifficultyBits   = 0x1f00ffff;  // Emergency floor
    uint32_t powLimitBits        = 0x1f00ffff;  // Absolute max target

    // ========== PHASE 2: ASERT ==========
    DAAType  daaType             = DAAType::ASERT;
    uint32_t asertAnchorHeight   = 180'000;     // Last Phase 1 block
    int64_t  asertHalfLifeSec    = 43'200;      // 12 hours (144 blocks)

    // ========== SUBSIDY ==========
    // Genesis (height 0): 99 DIN (unspendable) + 1M DIN (spendable)
    int64_t genesisSubsidy       = 99 * COIN;
    int64_t premineSubsidy       = 1'000'000 * COIN;
    int64_t phase1Subsidy        = 100 * COIN;        // Heights 1-180,000
    int64_t phase2InitialSubsidy = 50 * COIN;         // Heights 180,001+
    uint32_t halvingIntervalBlk  = 800'000;           // ~7.6 years @ 5min
};
```

---

## Block Height Structure (CORRECTED)

| Height | Type | Reward | Difficulty | DAA |
|--------|------|--------|-----------|-----|
| **0** | Genesis | 99 DIN (unspendable) + 1M DIN (spendable) | 0x1d3fffff | N/A |
| **1** | Phase 1 Start | 100 DIN | 0x1d3fffff (fixed) | None |
| **2-180,000** | Phase 1 | 100 DIN | 0x1d3fffff (fixed) | Anti-stall only |
| **180,000** | Phase 1 End / ASERT Anchor | 100 DIN | 0x1d3fffff (fixed) | None |
| **180,001** | Phase 2 Start | 50 DIN | ASERT (from anchor) | ASERT per-block |
| **180,002+** | Phase 2 | 50 DIN → halves | ASERT | ASERT per-block |
| **980,001** | First Halving | 25 DIN | ASERT | ASERT per-block |

**CRITICAL INVARIANTS:**
- ✅ Phase 1: Heights 1-180,000 (exactly 180,000 blocks)
- ✅ ASERT anchor: Block 180,000 (last Phase 1 block)
- ✅ Phase 2 start: Block 180,001 (first ASERT calculation)
- ✅ Genesis subsidy: 99 + 1,000,000 DIN (both in height 0 coinbase)
- ✅ NO separate premine at height 1 (already in genesis)

---

## Implementation Files

### 1. Consensus Parameters
**File:** `src/consensus/consensus.hpp`
- Defines all canonical constants
- Phase boundaries: 1-180,000 (Phase 1), 180,001+ (Phase 2)
- ASERT anchor: Block 180,000

### 2. ASERT Algorithm
**File:** `src/consensus/pow_asert.hpp`
- Integer-only fixed-point ASERT implementation
- Anchored at block 180,000
- Per-block difficulty adjustment
- Formula: `new_target = anchor_target × 2^(excess_time / half_life)`

### 3. Difficulty Selection
**File:** `src/consensus/pow.hpp`
- `GetNextWorkRequired()`: Main difficulty function
- Phase 1: Returns fixed `0x1d3fffff` with anti-stall rescue
- Phase 2: Calls ASERT with anchor at block 180,000

### 4. Timestamp Validation
**File:** `src/consensus/timestamp_validation.hpp`
- MTP+1 requirement (blocks must advance past median time)
- Future limit: ≤ now() + 2 hours
- Minimum spacing: ≥ 1 second between blocks

---

## Phase 1: Fixed Difficulty + Anti-Stall

### Normal Operation (Heights 1-180,000)

```cpp
uint32_t GetNextWorkRequired(uint32_t height, ...) {
    if (height <= 180'000) {
        // Normal: Fixed difficulty
        return 0x1d3fffff;
    }
    ...
}
```

### Anti-Stall Protection

```cpp
if (height <= 180'000) {
    int64_t stallThreshold = 20 × 300; // 6000 sec (100 minutes)

    if ((currentMTP - prevMTP) >= stallThreshold) {
        // Emergency: One-block rescue
        return 0x1f00ffff;  // minDifficultyBits
    }

    return 0x1d3fffff;  // Normal fixed
}
```

**Behavior:**
- Normal blocks: Difficulty = 0x1d3fffff (fixed)
- If no block for ≥100 minutes: Next block allowed at 0x1f00ffff (much easier)
- Following block returns to 0x1d3fffff (normal)
- **Liveness guarantee:** Network never stalls permanently

---

## Phase 2: ASERT Per-Block Adjustment

### Anchor Setup (Block 180,000)

```cpp
// Block 180,000 properties:
anchor_height = 180'000
anchor_bits = 0x1d3fffff  // Fixed Phase 1 difficulty
anchor_time = <block 180,000 timestamp>
```

### First ASERT Block (180,001)

```cpp
height = 180'001
currentTime = <block 180,001 timestamp>

// ASERT calculation:
timeDelta = currentTime - anchor_time
heightDelta = 180'001 - 180'000 = 1
idealTime = 1 × 300 = 300 seconds
excessTime = timeDelta - idealTime

// If timeDelta = 305 (5 sec late):
excessTime = 5 (blocks slightly slow)
new_difficulty ≈ anchor_difficulty × 2^(5 / 43200) ≈ anchor × 1.00008
// Difficulty slightly decreases (easier)

// If timeDelta = 295 (5 sec early):
excessTime = -5 (blocks slightly fast)
new_difficulty ≈ anchor_difficulty × 2^(-5 / 43200) ≈ anchor × 0.99992
// Difficulty slightly increases (harder)
```

### Continuous Adjustment (Block 180,002+)

```cpp
// Every block uses the same anchor (180,000)
for (height = 180'002; height < MAX_HEIGHT; height++) {
    timeDelta = currentTime - anchor_time
    heightDelta = height - 180'000
    idealTime = heightDelta × 300
    excessTime = timeDelta - idealTime

    new_difficulty = anchor_difficulty × 2^(excessTime / 43200)
}
```

**Convergence:**
- Half-life = 12 hours (43,200 seconds)
- If blocks consistently 10% fast → difficulty doubles in ~83 hours
- If blocks consistently 10% slow → difficulty halves in ~83 hours
- Smooth exponential convergence (no oscillation)

---

## Timestamp Rules (Anti-Runaway)

### Validation Checks

```cpp
bool ValidateBlockTimestamp(int64_t blockTime, int64_t prevTime, int64_t MTP, int64_t now) {
    // Rule 1: Must advance past MTP
    if (blockTime <= MTP) {
        return false;  // Prevents time-warp attacks
    }

    // Rule 2: Cannot be >2 hours in future
    if (blockTime > now + 7200) {
        return false;  // Prevents future-timestamp abuse
    }

    // Rule 3: Must be ≥1 second after previous block
    if (blockTime < prevTime + 1) {
        return false;  // Prevents instant spam
    }

    return true;
}
```

**Effect:**
- Even with 1000x hashrate spike, blocks rate-limited by time
- Phase 1 minimum duration: ~50 hours (not 3 hours)
- Prevents blockchain bloat

---

## Subsidy Schedule (CORRECTED)

### Genesis (Height 0)

```cpp
// Genesis coinbase contains TWO outputs:
vout[0]: 99 DIN (unspendable, OP_RETURN or burn script)
vout[1]: 1,000,000 DIN (spendable, P2WPKH to premine address)

Total: 1,000,099 DIN
```

### Phase 1 (Heights 1-180,000)

```cpp
uint64_t GetBlockSubsidy(uint32_t height) {
    if (height == 0) {
        return 99 * COIN + 1'000'000 * COIN;  // 1,000,099 DIN
    }

    if (height >= 1 && height <= 180'000) {
        return 100 * COIN;  // 100 DIN per block
    }

    ...
}
```

**Phase 1 Total Issuance:**
```
Genesis: 1,000,099 DIN
Blocks 1-180,000: 180,000 × 100 = 18,000,000 DIN
Phase 1 Total: 19,000,099 DIN
```

### Phase 2 (Heights 180,001+)

```cpp
if (height >= 180'001) {
    uint32_t blocks_since_phase2 = height - 180'001;
    uint32_t halving_epoch = blocks_since_phase2 / 800'000;

    uint64_t subsidy = 50 * COIN;  // Initial
    for (uint32_t i = 0; i < halving_epoch; i++) {
        subsidy /= 2;
    }
    return subsidy;
}
```

**Phase 2 Schedule:**
```
Blocks 180,001 - 980,000:   800,000 × 50 DIN  = 40,000,000 DIN
Blocks 980,001 - 1,780,000: 800,000 × 25 DIN  = 20,000,000 DIN
Blocks 1,780,001 - 2,580,000: 800,000 × 12.5 DIN = 10,000,000 DIN
...
```

**Total Supply:**
```
Phase 1:  19,000,099 DIN
Phase 2:  ~81,000,000 DIN (asymptotic to 100M total)
Max:      ~100,000,099 DIN
```

---

## Testing Requirements

### Unit Tests (Essential)

**File:** `tests/test_daa_phase_transition.cpp`

```cpp
TEST(DAA, Phase1_FixedDifficulty) {
    Consensus c;

    for (uint32_t h = 1; h <= 180'000; h++) {
        uint32_t bits = GetNextWorkRequired(h, 0, 0, 0, 0, c);
        ASSERT_EQ(bits, 0x1d3fffff);
    }
}

TEST(DAA, Phase1_AntiStall) {
    Consensus c;
    int64_t stallTime = 6000;  // 100 minutes

    // Normal: No stall
    uint32_t bits1 = GetNextWorkRequired(100, 0, 0, 300, 300, c);
    ASSERT_EQ(bits1, 0x1d3fffff);

    // Stalled: MTP gap ≥ 100 min
    uint32_t bits2 = GetNextWorkRequired(100, 0, 0, 0, stallTime, c);
    ASSERT_EQ(bits2, 0x1f00ffff);  // Emergency
}

TEST(DAA, Phase2_AnchorAt180000) {
    Consensus c;
    ASSERT_EQ(c.asertAnchorHeight, 180'000);
    ASSERT_EQ(c.easyPhaseEnd, 180'000);
}

TEST(DAA, Phase2_FirstASERTBlock) {
    Consensus c;
    int64_t anchor_time = 1000000;
    int64_t current_time = anchor_time + 300;  // Exactly 5 min later

    uint32_t bits = GetNextWorkRequired(
        180'001,            // First Phase 2 block
        0x1d3fffff,         // Prev bits
        anchor_time - 300,  // Prev MTP
        current_time,       // Current MTP
        anchor_time,        // Anchor time
        c
    );

    // Should be close to anchor difficulty (no excess time)
    ASSERT_NEAR(bits, 0x1d3fffff, 100);  // Allow small deviation
}

TEST(DAA, Subsidy_Genesis) {
    uint64_t subsidy = GetBlockSubsidy(0);
    ASSERT_EQ(subsidy, 1'000'099 * COIN);  // 99 + 1M DIN
}

TEST(DAA, Subsidy_Phase1) {
    for (uint32_t h = 1; h <= 180'000; h++) {
        uint64_t subsidy = GetBlockSubsidy(h);
        ASSERT_EQ(subsidy, 100 * COIN);
    }
}

TEST(DAA, Subsidy_Phase2) {
    uint64_t subsidy1 = GetBlockSubsidy(180'001);
    ASSERT_EQ(subsidy1, 50 * COIN);

    uint64_t subsidy2 = GetBlockSubsidy(980'001);  // First halving
    ASSERT_EQ(subsidy2, 25 * COIN);
}
```

### Integration Tests

**File:** `tests/integration/test_phase_transition_e2e.sh`

```bash
#!/bin/bash
# End-to-end phase transition test

# 1. Start regtest node
./dinerod --regtest &
sleep 2

ADDR=$(./dinero-cli --regtest getnewaddress)

# 2. Mine to Phase 1 end (height 180,000)
./dinero-cli --regtest generatetoaddress 179999 $ADDR

# Check block 180,000
BLOCK_180000=$(./dinero-cli --regtest getblock $(./dinero-cli --regtest getblockhash 180000))
BITS_180000=$(echo $BLOCK_180000 | jq -r '.bits')
REWARD_180000=$(echo $BLOCK_180000 | jq -r '.tx[0].vout[0].value')

echo "Block 180,000 (last Phase 1):"
echo "  Bits: $BITS_180000 (expected: 0x1d3fffff)"
echo "  Reward: $REWARD_180000 (expected: 100 DIN)"

# 3. Mine first Phase 2 block (height 180,001)
./dinero-cli --regtest generatetoaddress 1 $ADDR

BLOCK_180001=$(./dinero-cli --regtest getblock $(./dinero-cli --regtest getblockhash 180001))
BITS_180001=$(echo $BLOCK_180001 | jq -r '.bits')
REWARD_180001=$(echo $BLOCK_180001 | jq -r '.tx[0].vout[0].value')

echo "Block 180,001 (first Phase 2):"
echo "  Bits: $BITS_180001 (expected: ASERT-calculated)"
echo "  Reward: $REWARD_180001 (expected: 50 DIN)"

# 4. Verify ASERT adjusts per block
./dinero-cli --regtest generatetoaddress 10 $ADDR

for h in {180002..180011}; do
    BITS=$(./dinero-cli --regtest getblock $(./dinero-cli --regtest getblockhash $h) | jq -r '.bits')
    echo "Block $h: bits=$BITS"
done
```

---

## Deployment Checklist

### Pre-Launch

- [x] ✅ Consensus parameters defined (consensus.hpp)
- [x] ✅ ASERT algorithm implemented (pow_asert.hpp)
- [x] ✅ Difficulty selector implemented (pow.hpp)
- [x] ✅ Timestamp validation implemented (timestamp_validation.hpp)
- [x] ✅ Genesis subsidy corrected (99 + 1M DIN at height 0)
- [x] ✅ Phase boundaries corrected (1-180,000 for Phase 1)
- [x] ✅ ASERT anchor set to block 180,000
- [ ] Unit tests written and passing
- [ ] Integration tests written and passing
- [ ] Testnet deployment and validation
- [ ] Documentation updated

### Post-Launch

- [ ] Monitor Phase 1 duration (actual vs predicted)
- [ ] Verify anti-stall triggers correctly if hashrate drops
- [ ] Monitor Phase 1 → Phase 2 transition at block 180,001
- [ ] Verify ASERT converges to 5-minute target
- [ ] Monitor first halving at block 980,001

---

## Summary of Corrections

### What Was Wrong

1. ❌ Phase 1 started at height 2 (should be 1)
2. ❌ easyPhaseEnd = 180,001 (should be 180,000)
3. ❌ asertAnchorHeight = 180,001 (should be 180,000)
4. ❌ Premine treated as separate block at height 1 (it's in genesis)
5. ❌ Off-by-one errors throughout

### What's Correct Now

1. ✅ Phase 1: Heights 1-180,000 (exactly 180,000 blocks)
2. ✅ easyPhaseEnd = 180,000 (last Phase 1 block)
3. ✅ asertAnchorHeight = 180,000 (anchor at last Phase 1 block)
4. ✅ Genesis: 99 + 1M DIN in single coinbase at height 0
5. ✅ Phase 2 start: Height 180,001 (first ASERT calculation)
6. ✅ No double premine
7. ✅ All invariants aligned with on-chain reality

---

**Status:** ✅ **READY FOR TESTING**
**Last Updated:** 2025-10-14
