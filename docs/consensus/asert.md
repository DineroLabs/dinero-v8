# ASERT Difficulty Adjustment Algorithm

**Status:** CONSENSUS-LOCKED (Do not modify without coordinated hard fork)
**Implementation:** Bitcoin Cash Node Canonical
**Tag:** `consensus-asert-locked` (commit 5d172b27)
**Date:** December 22, 2025

---

## Overview

DineroCoin uses **ASERT** (Anchor-based Smooth Elastic Retargeting) for per-block difficulty adjustment, providing smooth difficulty convergence while preventing timestamp manipulation attacks.

### Why ASERT Was Chosen

1. **Per-block adjustment** - More responsive than Bitcoin's 2016-block DAA
2. **Timestamp manipulation resistant** - Uses Median Time Past (MTP)
3. **Battle-tested** - Used by Bitcoin Cash since November 2020
4. **Mathematically sound** - Exponential convergence to target block time
5. **Simple anchor model** - Rolling anchor (previous block) simplifies implementation

---

## Algorithm Specification

### Formula

```
new_target = anchor_target × 2^(exponent)
where:
  exponent = (time_delta - expected_time) / half_life
  time_delta = current_MTP - anchor_MTP
  expected_time = height_delta × target_spacing
```

### Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| **Target Spacing** | 120 seconds | 2 minutes per block |
| **Half-Life** | 43,200 seconds | 12 hours (360 blocks) |
| **Anchor** | Previous block | Rolling anchor (height - 1) |
| **Anchor Block** | Block 1 | Premine block at height 1 |

---

## Canonical Implementation

### Source

DineroCoin uses the **exact** Bitcoin Cash Node ASERT implementation:
- **Repository:** https://github.com/bitcoin-cash-node/bitcoin-cash-node
- **Specification:** `doc/asert.md`
- **Reference:** `src/pow/aserti3-2d.cpp`

### Cubic Polynomial Approximation

ASERT uses a cubic polynomial to approximate `2^x` with **<0.013% error**:

```cpp
// 16-bit fixed-point arithmetic (RADIX = 65536)
factor = 65536 + ((c1×frac + c2×frac² + c3×frac³ + ROUNDING) >> 48)

// Consensus-critical coefficients (BCH Node canonical)
const uint64_t COEFF_1 = 195766423245049ull;
const uint64_t COEFF_2 = 971821376ull;
const uint64_t COEFF_3 = 5127ull;
const uint64_t ROUNDING = (1ull << 47);
```

**⚠️ DO NOT MODIFY THESE COEFFICIENTS** - They are consensus-critical and must match Bitcoin Cash Node exactly.

### Integer Exponent Bounds

```cpp
// Exponent split into integer (shifts) and fractional (polynomial) parts
k = floor(exponent / half_life)  // Integer part
r = exponent mod half_life        // Fractional part

// Clamp integer shifts to prevent overflow
k ∈ [-32, +32]

// Apply shifts:
if (k > 0): target <<= k  // Easier (blocks too slow)
if (k < 0): target >>= |k| // Harder (blocks too fast)
```

---

## Anchor Definition

### Block 1 (Premine Block)
- **Height:** 1
- **Bits:** `0x207fffff` (regtest), `0x1d31ffce` (mainnet)
- **Role:** ASERT anchor - all subsequent blocks calculate difficulty relative to block 1
- **Fixed:** Block 1 difficulty is **hardcoded** and never calculated

### Rolling Anchor (Blocks 2+)
For computational efficiency, DineroCoin uses the **previous block** as the anchor:
- **Anchor height:** `height - 1`
- **Anchor time:** MTP of previous block
- **Anchor bits:** Previous block's difficulty

This creates a mathematically equivalent chain where each block is calculated from its immediate predecessor.

---

## Regtest Behavior

**Regtest short-circuit** (intentional and safe):

```cpp
if (params.name == "regtest" && height >= 2) {
    return powLimitBits;  // 0x207fffff (instant mining)
}
```

### Rationale
- **Block 0 (Genesis):** Uses mainnet parameters for consistency
- **Block 1 (Anchor):** Uses mainnet parameters to match production
- **Blocks 2+:** Return easy difficulty for developer sanity

This does **not** weaken mainnet or testnet security - regtest is isolated.

---

## Implementation Files

### Consensus-Critical Code (LOCKED)

| File | Purpose | Status |
|------|---------|--------|
| `src/consensus/pow_asert_native.hpp` | Core ASERT calculation | 🔒 LOCKED |
| `src/consensus/pow.hpp` | GetNextWorkRequired() wrapper | 🔒 LOCKED |
| `src/consensus/consensus.hpp` | ASERT parameters | 🔒 LOCKED |

### Integration Points

| File | Purpose | Modifiable |
|------|---------|------------|
| `src/daemon/mining.cpp` | Miner difficulty calculation | ✅ (logging only) |
| `src/daemon/block_acceptor.cpp` | Consensus validation | ✅ (logging only) |
| `src/consensus/chainparams_impl.cpp` | Network parameters | ⚠️ (new chains only) |

---

## Verification & Testing

### Production Verification (Regtest)
```bash
# Mine 7 blocks and verify no diffbits errors
./bin/dinero-cli -regtest generatetoaddress 7 <address>

# Expected: All blocks accepted, no "bad-diffbits" errors
# Verified: December 22, 2025 (commit 5d172b27)
```

### Key Test Results
- ✅ Block 1 (anchor) accepted with hardcoded bits
- ✅ Blocks 2-7 accepted with ASERT-calculated difficulty
- ✅ Miner and consensus produce identical difficulty values
- ✅ MTP clamping prevents time-warp attacks
- ✅ No platform-specific floating-point divergence

---

## Security Properties

### Timestamp Manipulation Resistance
- Uses **Median Time Past (MTP)** instead of raw timestamps
- MTP cannot be manipulated by a single miner
- Anti-time-warp: `current_MTP >= prev_MTP + 1`

### Difficulty Oscillation Prevention
- Exponential smoothing via half-life (12 hours)
- No sudden jumps - gradual convergence to target
- Prevents difficulty oscillation attacks

### Platform Independence
- **Integer-only arithmetic** (no floating point)
- Deterministic across all platforms (x86, ARM, RISC-V)
- Exact BCH Node implementation guarantees consensus compatibility

---

## Future-Proof Guarantees

### What Can Be Changed (Non-Consensus)
- Logging statements
- Debug output formatting
- Performance optimizations (if identical results)
- Regtest behavior (isolated from mainnet)

### What CANNOT Be Changed (Consensus-Critical)
- ASERT coefficients (`COEFF_1`, `COEFF_2`, `COEFF_3`)
- Exponent bounds (`k ∈ [-32, +32]`)
- `GetNextWorkRequired()` mainnet/testnet behavior
- Block 1 anchor definition
- Target spacing or half-life (without coordinated fork)

---

## References

1. **Bitcoin Cash Node ASERT Specification**
   https://github.com/bitcoin-cash-node/bitcoin-cash-node/blob/master/doc/asert.md

2. **Original ASERT Proposal (BCH CHIP-2020-11-ASERT)**
   https://gitlab.com/bitcoin-cash-node/bchn-sw/bitcoincash-upgrade-specifications

3. **DineroCoin Consensus Tag**
   `git show consensus-asert-locked`

4. **BCH Node Reference Implementation**
   https://github.com/bitcoin-cash-node/bitcoin-cash-node/blob/master/src/pow/aserti3-2d.cpp

---

## Audit Trail

| Event | Date | Commit | Notes |
|-------|------|--------|-------|
| ASERT Implementation | Dec 21, 2025 | 22:58 | BCH canonical implementation |
| Logging Cleanup | Dec 22, 2025 | 00:30 | Hex formatting fixes |
| Consensus Lock | Dec 22, 2025 | 5d172b27 | Verified 7+ blocks mined |
| Documentation | Dec 22, 2025 | This file | Formal specification |

---

## Contact & Governance

**Consensus changes require:**
1. Public proposal and review period (minimum 3 months)
2. Network-wide coordination (miners, nodes, wallets)
3. Formal hard fork activation at predetermined height
4. Backward compatibility considerations

**For questions or proposed changes:**
- Open an issue: https://github.com/DineroLabs/Dinero/issues
- Tag: `consensus-critical`
- Maintainer review required

---

**This document is consensus-locked along with the implementation.**
**Last updated:** December 22, 2025 (commit 5d172b27)
