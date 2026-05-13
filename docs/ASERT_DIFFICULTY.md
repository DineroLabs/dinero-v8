# DineroCoin ASERT Difficulty Adjustment Algorithm

## Overview

DineroCoin uses a custom implementation of **ASERT** (Absolutely Scheduled Exponentially Weighted Target) for per-block difficulty adjustment. This ensures smooth, continuous difficulty retargeting with no oscillations or jumps.

## Origin and History

### Where ASERT Came From
- **Creator**: Jonathan Toomim
- **Year**: 2020
- **Context**: Bitcoin Cash difficulty adjustment overhaul
- **Purpose**: Replace chaotic per-block algorithms (DAA/DGW) with mathematically pure, continuous formula

### Why ASERT is Superior
1. **Deterministic** - No hysteresis or state-dependent behavior
2. **Smooth** - Uses exponential math with one anchor block
3. **Predictable** - No oscillations or difficulty jumps
4. **Fair** - Difficulty adjusts smoothly to hashrate changes

### DineroCoin's Implementation
- **Status**: Custom implementation (not inherited from Bitcoin Core)
- **Base**: BCH ASERT whitepaper and reference code
- **Timeline**: Implemented mid-October 2025
- **Integration**: Built directly into consensus and mining components

## Technical Specifications

### Core Parameters

```cpp
struct Consensus {
    // Easiest allowed difficulty (powLimit)
    uint32_t powLimitBits = 0x1f00ffff;

    // ASERT anchor difficulty (Phase 1 CPU-friendly)
    uint32_t asertAnchorBits = 0x1d3fffff;

    // Block target spacing (5 minutes = 300 seconds)
    int targetSpacing = 300;

    // ASERT half-life (2 days = 172,800 seconds)
    // Time for difficulty to halve/double in response to hashrate changes
    int64_t asertHalfLife = 172800;
};
```

### Difficulty Calculation Formula

```
difficulty = powLimit_target / current_target

Where:
- powLimit_target = expanded form of 0x1f00ffff
- current_target = expanded form of current nBits
- Higher target = easier mining = lower difficulty
- Lower target = harder mining = higher difficulty
```

### Example Calculations

**Phase 1 Difficulty (0x1d3fffff)**:
- Raw bits: 490,733,567 (decimal representation)
- Calculated difficulty: **1023.98**
- Interpretation: ~1024× harder than powLimit

**Phase 2 Difficulty (0x1d00ffff)**:
- Raw bits: 486,604,799 (decimal representation)
- Calculated difficulty: **65,536.0**
- Interpretation: Bitcoin-level difficulty

## Implementation Details

### Key Files and Functions

#### 1. Consensus Parameters
**File**: `include/consensus/consensus.hpp`
```cpp
struct Consensus {
    uint32_t powLimitBits;       // 0x1f00ffff
    uint32_t asertAnchorBits;    // 0x1d3fffff (Phase 1)
    int targetSpacing;            // 300 seconds
    int64_t asertHalfLife;       // 172,800 seconds
};
```

#### 2. Difficulty Helpers
**File**: `include/consensus/target_helpers.h`
```cpp
// Convert compact bits to target array
std::array<uint8_t,32> TargetFromBits(uint32_t nBits);

// Convert target to difficulty value
double DifficultyFromBits(uint32_t nBits, uint32_t powLimitBits);

// Convert target to double for calculations
double TargetToDouble(const std::array<uint8_t,32>& target);
```

#### 3. Chain Access Functions
**File**: `include/storage/chain_direct.h`
```cpp
// Get difficulty bits for given height
inline uint32_t GetDifficultyBits(ChainDB* db, uint32_t height) {
    // Phase 1: Heights 1-180,000 use fixed CPU-friendly difficulty
    if (height >= 1 && height <= 180'000) {
        return 0x1d3fffff; // CPU-friendly (1023.98 difficulty)
    }
    // Phase 2: Heights 180,001+ use Bitcoin-level difficulty
    return 0x1d00ffff; // Bitcoin-level (65,536.0 difficulty)
}

// Get calculated difficulty value
inline double GetDifficulty(ChainDB* db, uint32_t height) {
    uint32_t current_bits = GetDifficultyBits(db, height);
    uint32_t pow_limit_bits = 0x1f00ffff;
    return ::dinero::DifficultyFromBits(current_bits, pow_limit_bits);
}
```

#### 4. RPC Integration
**File**: `src/rpc/blockchain_rpc_handlers.cpp`
```cpp
din::Json rpc_getblockchaininfo(const ExecutionContext& ctx, const din::Json& params) {
    Consensus consensus;
    uint32_t current_bits = consensus.asertAnchorBits;
    uint32_t pow_limit_bits = consensus.powLimitBits;
    result["difficulty"] = DifficultyFromBits(current_bits, pow_limit_bits);
    // ...
}
```

## Mining Phases

### Phase 1: CPU-Friendly Mining
- **Heights**: 1 - 180,000
- **Duration**: ~347 days (at 5-minute blocks)
- **Difficulty**: 0x1d3fffff (~1024.0)
- **Purpose**: Fair distribution, accessible to CPU miners
- **Target Time**: 5 minutes per block

### Phase 2: Bitcoin-Level Security
- **Heights**: 180,001+
- **Difficulty**: 0x1d00ffff (65,536.0)
- **Purpose**: Long-term network security
- **Target Time**: 5 minutes per block

## ASERT Adjustment Mechanics

### Half-Life Concept
The ASERT half-life (172,800 seconds = 2 days) determines how quickly difficulty responds to hashrate changes:

```
If hashrate doubles:
- After 2 days: Difficulty → 2× original
- After 4 days: Difficulty → 4× original
- After 6 days: Difficulty → 8× original

If hashrate halves:
- After 2 days: Difficulty → 0.5× original
- After 4 days: Difficulty → 0.25× original
- After 6 days: Difficulty → 0.125× original
```

### Smooth Adjustment
Unlike Bitcoin's 2016-block retarget, ASERT adjusts **every single block** based on:
1. Time delta from anchor
2. Height delta from anchor
3. Target spacing (5 minutes)
4. Exponential curve defined by half-life

## Verification and Testing

### Manual Difficulty Check
```bash
# Query blockchain difficulty via RPC
curl -u 'dinerouser:Dinerosaur1447' -X POST http://172.93.160.131:20998 \
  -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo","params":[]}'
```

Expected output:
```json
{
  "result": {
    "difficulty": 1023.9846191369579,
    "blocks": 12,
    "chain": "main"
  }
}
```

### Difficulty Calculation Test
```cpp
// Test the calculation manually
uint32_t phase1_bits = 0x1d3fffff;
uint32_t pow_limit = 0x1f00ffff;

auto phase1_target = TargetFromBitsBE(phase1_bits);
auto limit_target = TargetFromBitsBE(pow_limit);

double phase1_double = TargetToDouble(phase1_target);
double limit_double = TargetToDouble(limit_target);

double difficulty = limit_double / phase1_double;
// Expected: ~1023.98
```

## Common Issues and Fixes

### Issue: Difficulty Shows as Raw Bits
**Symptom**: GUI shows `490733567` instead of `1023.98`

**Cause**: `GetDifficulty()` was returning `uint32_t` (raw compact bits) instead of `double` (calculated difficulty)

**Fix**: Use `DifficultyFromBits()` helper:
```cpp
// WRONG:
return 0x1d3fffff;  // Returns 490,733,567

// CORRECT:
return DifficultyFromBits(0x1d3fffff, 0x1f00ffff);  // Returns 1023.98
```

### Issue: Blocks Mining Too Fast
**Symptom**: 200-300 blocks in < 2 minutes (should be 5 minutes per block)

**Cause**: Same as above - difficulty displayed incorrectly, actual difficulty may be wrong

**Solution**: Verify difficulty calculation and ensure ASERT anchor is properly set

## Production Status

### Current Deployment
- **California Server (172.93.160.131)**: Height 12, Difficulty 1023.98 ✅
- **Virginia Server (173.249.195.59)**: Height 0, Difficulty 1023.98 ✅
- **Deployment Date**: October 25, 2025
- **Status**: Fully operational

### Files Deployed
1. `include/storage/chain_direct.h` - Fixed difficulty calculation
2. `include/consensus/target_helpers.h` - Complete helper functions
3. Both daemons rebuilt and restarted with authentication

## References

### External Resources
- [Bitcoin Cash ASERT Specification](https://gitlab.com/bitcoin-cash-node/bitcoin-cash-node/-/blob/master/doc/abc/asert.md)
- [Jonathan Toomim's ASERT Proposal](https://read.cash/@jtoomim/bch-upgrade-proposal-use-asert-as-the-new-daa-1d875696)

### Internal Documentation
- `include/consensus/consensus.hpp` - Consensus parameters
- `include/consensus/target_helpers.h` - Difficulty calculation helpers
- `include/storage/chain_direct.h` - Chain access functions
- `src/rpc/blockchain_rpc_handlers.cpp` - RPC difficulty reporting

## Maintenance Notes

### When to Update ASERT Parameters
1. **Changing block time**: Adjust `targetSpacing`
2. **Faster/slower adjustment**: Modify `asertHalfLife`
3. **Hardening/softening**: Change `asertAnchorBits`
4. **Phase transitions**: Update `GetDifficultyBits()` height thresholds

### Monitoring Checklist
- [ ] Verify difficulty calculation via RPC
- [ ] Check block times are averaging 5 minutes
- [ ] Monitor hashrate vs difficulty correlation
- [ ] Ensure no difficulty oscillations
- [ ] Validate against known test vectors

---

**Document Version**: 1.0
**Last Updated**: October 25, 2025
**Author**: DineroCoin Development Team
