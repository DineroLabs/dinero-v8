# Difficulty Rules

This document describes DineroCoin's difficulty adjustment algorithm.

## Overview

DineroCoin uses a hybrid difficulty adjustment system:
1. **Bootstrap phase** (blocks 0-720): Gradual ramp with clamped adjustments
2. **ASERT phase** (blocks ≥721): aserti3-2d algorithm (same as Bitcoin Cash)

## Constants

| Parameter | Value | Description |
|-----------|-------|-------------|
| `powLimitBits` | `0x1d31ffce` | Difficulty floor (50× easier than Bitcoin) |
| `bootstrapBits` | `0x1d31ffce` | Starting difficulty |
| `bootstrapWindow` | 720 blocks | Bootstrap phase duration (~1 day) |
| `asertAnchorHeight` | 200002 | ASERT reference height |
| `asertHalfLife` | 172800 seconds | ASERT half-life (2 days) |

## Block-by-Block Rules

### Block 0 (Genesis)

- **nBits**: `0x1d31ffce` (hardcoded in chainparams)
- **Source**: `dinero::Params().genesis.nBits`
- **Validation**: Self-validating (defines the chain)

### Block 1 (Premine)

- **nBits**: Computed via `GetNextWorkRequired(1, ...)`
- **Source**: Same as genesis (bootstrap hasn't kicked in)
- **Validation**: Must match `GetNextWorkRequired()` output

### Blocks 2-720 (Bootstrap)

- **Algorithm**: Clamped adjustment
- **Clamps**: +10% max increase, -20% max decrease
- **Purpose**: Gradual difficulty ramp during early mining

### Blocks ≥721 (ASERT)

- **Algorithm**: aserti3-2d (exponential moving average)
- **Target**: 10-minute blocks
- **Half-life**: 2 days (172800 seconds)
- **Reference**: Anchor at height 200002

## Who Enforces What

| Component | Responsibility |
|-----------|----------------|
| `GetNextWorkRequired()` | Canonical difficulty calculation |
| `block_acceptor.cpp` | Validates incoming blocks |
| `block_assembler.cpp` | Sets nBits in new block templates |
| `mining_coordinator.cpp` | Uses correct difficulty for mining |
| `methods_mining_vnext.cpp` | Stratum template difficulty |

## Critical Invariant

**All difficulty values MUST come from `GetNextWorkRequired()`.**

No hardcoded values are allowed in:
- Block validation
- Mining
- Stratum templates
- RPC responses

The only exceptions are:
- Genesis block (defines the chain)
- Display/info-only RPCs (non-consensus)

## Validation Flow

```
Block received
    ↓
Extract header.nBits
    ↓
Compute expected = GetNextWorkRequired(height, prev_bits, ...)
    ↓
if (header.nBits != expected)
    → REJECT "bad-diffbits"
    ↓
Accept block
```

## Testing

The `test_bad_diffbits.sh` test permanently locks this behavior:

```bash
./tests/functional/test_bad_diffbits.sh
```

This proves that blocks with invalid nBits are **always rejected**.

## Code References

| File | Purpose |
|------|---------|
| `consensus/pow.hpp` | `GetNextWorkRequired()` implementation |
| `consensus/consensus.hpp` | Consensus parameters |
| `consensus/chainparams.h` | Genesis block constants |
| `validation/block_acceptor.cpp` | Difficulty validation |
| `mining/block_assembler.cpp` | Template difficulty |

## Change Policy

**Difficulty rules are consensus-frozen.**

Any change requires:
1. Hard fork coordination
2. Network-wide upgrade
3. Extensive testing

Do not modify without full consensus review.
