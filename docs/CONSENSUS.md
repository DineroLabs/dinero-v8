# DineroCoin Consensus Rules

This document describes the consensus-critical rules that all DineroCoin nodes must follow to maintain network agreement.

## Table of Contents
- [Difficulty Adjustment](#difficulty-adjustment)
- [Block Validation](#block-validation)
- [Transaction Validation](#transaction-validation)
- [Network Parameters](#network-parameters)

---

## Difficulty Adjustment

### Overview
DineroCoin uses a two-phase difficulty adjustment algorithm:
- **Phase 1** (Heights 1-180,000): Fixed easy difficulty for network bootstrap
- **Phase 2** (Heights 180,001+): ASERT (aserti3-2d) dynamic adjustment

### Authoritative Difficulty Selection

**CRITICAL RULE**: All components (validation, mining, RPC) MUST obtain compact difficulty bits via the canonical selector function:

```cpp
uint32_t GetNextWorkRequired(
    uint32_t height,
    uint32_t prevBits,
    int64_t prevMTP,          // Median Time Past of previous block
    int64_t currentMTP,       // Median Time Past of current block
    int64_t anchorTime,       // Timestamp of anchor block (height 180,001)
    const Consensus& params
);
```

**Location**: `include/consensus/pow.hpp`

### Consensus Coupling Rules

1. **RPC Handlers** must NOT compute or special-case difficulty
   - Format the selector's result as an 8-character lowercase hex string (BIP22 standard)
   - Never hardcode fallback values (e.g., `0x1d00ffff`, `0x1d3fffff`)
   - Pass real chain data: `prevBits` from previous block header, MTPs from last 11 blocks

2. **Block Validation** must use the same selector
   - Validate `nBits` in block header matches `GetNextWorkRequired()` output
   - Reject blocks with incorrect difficulty

3. **Mining / Block Assembly** must use the same selector
   - Set `nBits` field in block template from `GetNextWorkRequired()`
   - No divergent difficulty calculation paths

### Phase Transitions

Phase transitions (e.g., ASERT activation at height 180,001) are enforced **within the selector**.
Callers require no changes at cutover heights.

**Example**:
```cpp
// WRONG: Hardcoded phase logic in RPC
if (height <= 180000) {
    bits = 0x1d3fffff;  // ❌ Creates fork risk
} else {
    bits = ComputeASERT(...);
}

// CORRECT: Single source of truth
bits = GetNextWorkRequired(height, prevBits, prevMTP, currentMTP, anchorMTP, consensus);
```

### BIP22 Compliance

The `getblocktemplate` RPC MUST format `bits` as:
- **8 characters** lowercase hexadecimal
- **No prefix** (no "0x")
- **Leading zeros preserved**

Example: `0x1d3fffff` → `"1d3fffff"`

Implementation:
```cpp
char bits_hex[9];
std::snprintf(bits_hex, sizeof(bits_hex), "%08x", bits);
```

### Phase 1: Fixed Easy Difficulty

- **Heights**: 1 - 180,000
- **Target Bits**: `0x1d3fffff` (490,733,567 decimal)
- **Target**: `00000000ffff0000000000000000000000000000000000000000000000000000`
- **Purpose**: Allow network bootstrap without hashpower competition

### Phase 2: ASERT Algorithm

- **Activation**: Height 180,001
- **Algorithm**: aserti3-2d (ASERT with 2-day half-life)
- **Target Spacing**: 120 seconds (2 minutes)
- **Anchor Block**: Height 180,001
- **Reference**: [ASERT Specification](https://gitlab.com/bitcoin-cash-node/bchn-sw/bitcoin-cash-node/-/blob/master/doc/asert-spec.md)

**Key Properties**:
- Adjusts based on **actual block times** vs **target times**
- Uses **exponential adjustment** with 2-day half-life
- **No oscillation**: Smooth difficulty curve
- **DOS resistant**: Cannot be manipulated by timestamp gaming

### Median Time Past (MTP)

Consensus uses **Median Time Past** (BIP113) instead of block timestamps directly:

```cpp
int64_t GetMedianTimePast(const BlockIndex* pindex) {
    std::vector<int64_t> timestamps;
    for (int i = 0; i < 11 && pindex; i++) {
        timestamps.push_back(pindex->nTime);
        pindex = pindex->pprev;
    }
    std::sort(timestamps.begin(), timestamps.end());
    return timestamps[timestamps.size() / 2];
}
```

**Purpose**: Prevents timestamp manipulation attacks

---

## Block Validation

### Required Checks

1. **Block Header**:
   - Version ≥ 1
   - Previous block hash matches chain tip
   - Merkle root matches transaction hashes
   - **Difficulty bits match `GetNextWorkRequired()`**
   - Block hash meets difficulty target
   - Timestamp ≤ network-adjusted time + 2 hours
   - Timestamp > median of last 11 blocks (MTP)

2. **Coinbase Transaction**:
   - First transaction must be coinbase
   - Exactly 1 coinbase per block
   - Coinbase maturity: 100 blocks
   - Subsidy amount matches consensus schedule

3. **Transactions**:
   - No duplicate transactions in block
   - All non-coinbase transactions valid
   - No double-spends within block
   - Sum(inputs) ≥ Sum(outputs) + fees (for each tx)

---

## Transaction Validation

### Standard Transaction Rules

1. **Version**: Must be 1
2. **Inputs**: At least 1 input (coinbase: 0 inputs)
3. **Outputs**: At least 1 output
4. **Size**: ≤ 100,000 bytes
5. **Fees**: Must be positive
6. **Scripts**: Must validate correctly

### Mempool Acceptance

- Minimum fee rate: 1 una/byte
- No dust outputs: Outputs ≥ 546 una
- No conflicts with mempool transactions
- No double-spends of confirmed transactions

---

## Network Parameters

### Mainnet

```cpp
consensus.easyPhaseEnd = 180000;
consensus.easyPhaseBits = 0x1d3fffff;
consensus.asertAnchorHeight = 180001;
consensus.asertTargetSpacing = 120;  // 2 minutes
consensus.asertHalfLife = 172800;    // 2 days in seconds
```

### Block Rewards

- **Phase 1** (Heights 1-180,000): 100 DIN per block
- **Phase 2** (Heights 180,001+): Decreasing subsidy with halvings every 800,000 blocks

### Network

- **P2P Port**: 20999
- **RPC Port**: 20998
- **Magic Bytes**: `0xd9b4bef9`

---

## Testing & Verification

### Unit Tests

- `src/test/gbt_bits_format_test.cpp`: Validates BIP22 bits formatting
- `src/test/daa_golden_vectors_test.cpp`: Verifies Phase 1 & ASERT transitions

### Integration Tests

```bash
# Test Phase 1 difficulty
curl --user "$COOKIE" -d '{"method":"getblocktemplate"}' http://localhost:20998/
# Expected: "bits": "1d3fffff"

# Mine to height 180,000, verify bits stay constant
# Mine to height 180,001, verify bits change (ASERT active)
```

### CI Validation

Automated grep checks prevent consensus regressions:
```bash
# Fail if difficulty is assigned outside selector
rg '\bbits\s*=' src | rg -v 'GetNextWorkRequired|snprintf'

# Fail if legacy constants re-appear
rg '0x1d00ffff' src | rg -v 'historical|test|comment'
```

---

## References

- [Bitcoin Difficulty Adjustment](https://en.bitcoin.it/wiki/Difficulty)
- [ASERT Algorithm (BCH)](https://gitlab.com/bitcoin-cash-node/bchn-sw/bitcoin-cash-node/-/blob/master/doc/asert-spec.md)
- [BIP22: getblocktemplate](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki)
- [BIP113: Median Time Past](https://github.com/bitcoin/bips/blob/master/bip-0113.mediawiki)

---

## Maintenance Notes

### Adding New Consensus Rules

1. Implement in `consensus/` directory
2. Add unit tests with golden vectors
3. Update this document
4. Deploy with version bit activation (if soft fork)

### Modifying Difficulty Algorithm

**DO NOT** modify `GetNextWorkRequired()` without:
1. Network-wide consensus
2. Hard fork activation plan
3. Testnet validation
4. Replay protection

**Remember**: Any divergence in difficulty calculation creates a chain split.
