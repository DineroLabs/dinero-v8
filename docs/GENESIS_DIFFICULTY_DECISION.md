# Genesis Difficulty Decision: Bitcoin's Maximum Target (0x1d00ffff)

**Date:** 2026-01-13
**Status:** LOCKED - Consensus-critical decision
**Effective:** Phase 3 Genesis Regeneration

---

## Executive Decision

**DineroCoin will use Bitcoin's genesis difficulty: `0x1d00ffff`**

This is:
- The **maximum target** (easiest possible difficulty)
- **Difficulty = 1** (Bitcoin's baseline)
- **Historically proven** (used by Bitcoin genesis)
- **Trivially mineable** on a single CPU

---

## Justification: Una Conditions

DineroCoin's launch replicates Una's conditions exactly:

| Condition | Bitcoin (2009) | DineroCoin (2025) |
|-----------|----------------|-------------------|
| Number of miners | 1 (Una) | 1 (single operator) |
| Network competition | None | None |
| Mining race | No | No |
| Economic value | Zero | Zero (initially) |
| Attack surface | Minimal | Minimal |

**Conclusion:** When these conditions hold, simplicity wins. Bitcoin didn't over-engineer genesis, and neither should we.

---

## What This Guarantees

✅ **Genesis mines immediately** (< 1 second expected)
✅ **Block 1 mines immediately** (< 1 second expected)
✅ **Zero risk of "genesis never found"**
✅ **Zero tuning cycles required**
✅ **Maximum historical legitimacy**
✅ **No special logic or fallbacks needed**
✅ **No future regret**

---

## Technical Details

### Compact Bits Format

```
0x1d00ffff = compact representation
Expands to: 0x00000000ffff0000000000000000000000000000000000000000000000000000
```

This is the **maximum target** - any hash below this value is valid.

### Difficulty Calculation

```
Difficulty = max_target / current_target
           = 0x00000000ffff... / 0x00000000ffff...
           = 1.0
```

Bitcoin's baseline difficulty is defined as 1.0.

### Expected Mining Time

For a single CPU core (~1 MH/s):
```
Time = 2^256 / (target * hashrate)
     ≈ 2^32 / (2^224 * 10^6)  # Simplified
     ≈ 4 seconds (average)
```

With 8 CPU cores: **< 1 second average**

---

## What We Do NOT Need

❌ Minimum difficulty rules
❌ Block 1 gating logic
❌ Artificial mining delays
❌ Custom difficulty parameters
❌ Attack protection (no attackers exist yet)

These only make sense when:
- Multiple miners exist
- A network is established
- Economic value is present

**We're not there yet — and that's fine.**

---

## Consensus Implications

### Genesis Block (Block 0)

- **Version:** 1
- **Timestamp:** 1772496000 (2026-03-03 00:00:00 UTC)
- **Difficulty:** `0x1d00ffff` (Bitcoin genesis difficulty)
- **Nonce:** Will be found by genesis_miner_v3_correct.cpp
- **Merkle Root:** Computed from coinbase (motto commitment)

### Premine Block (Block 1)

- **Difficulty:** `0x1d00ffff` (matches genesis)
- **ASERT Activation:** Block 1
- **Anchor Difficulty:** `0x1d00ffff` (matches genesis)

### ASERT Difficulty Adjustment

From block 2 onwards, ASERT dynamically adjusts difficulty:
- **Anchor:** Block 1 (difficulty = 0x1d00ffff)
- **Half-life:** 12 hours
- **Target spacing:** 2 minutes (120 seconds)

---

## Files Updated

### Consensus-Critical Files

1. **`src/consensus/chainparams_impl.cpp`** (line 65)
   - Genesis `nBits`: `0x1d31ffce` → `0x1d00ffff`
   - Genesis `nNonce`: `537015748` → `0` (will be re-mined)

2. **`include/consensus/asert_params.h`** (lines 18, 35, 77, 110)
   - `ASERT_ANCHOR_BITS`: `0x1d31ffce` → `0x1d00ffff`
   - Updated all static assertions to match

3. **`src/consensus/premine_block_mainnet.hpp`** (line 22)
   - Premine `N_BITS`: `0x1d31ffce` → `0x1d00ffff`
   - Premine `N_NONCE`: `64570621` → `0` (will be re-mined)

4. **`tools/genesis_miner_v3_correct.cpp`** (line 363)
   - Mining difficulty: `0x1d31ffce` → `0x1d00ffff`

### Why These Must Match

**Consensus invariant:**
```
genesis.difficulty == block1.difficulty == ASERT_ANCHOR_BITS
```

ASERT anchors at block 1, using block 1's difficulty as the baseline. For smooth bootstrap, block 1 must use the same difficulty as genesis.

---

## Historical Precedent

### Bitcoin Genesis (2009-01-03)

```cpp
// Bitcoin genesis block
nTime  = 1231006505;
nBits  = 0x1d00ffff;  // Maximum target
nNonce = 2083236893;
```

Bitcoin's genesis used the maximum target because:
- Una was the only miner
- Network didn't exist yet
- No competition or attacks possible
- Simple and deterministic

**DineroCoin uses the same reasoning.**

---

## Comparison: Old vs New

| Parameter | Old Value | New Value | Rationale |
|-----------|-----------|-----------|-----------|
| Genesis difficulty | `0x1d31ffce` | `0x1d00ffff` | Bitcoin historical precedent |
| Genesis difficulty (decimal) | 0.0625 | 1.0 | Baseline difficulty = 1 |
| Expected mine time (8 cores) | ~0.06 sec | ~1 sec | Still trivial, more standard |
| ASERT anchor | `0x1d31ffce` | `0x1d00ffff` | Must match genesis |
| Premine difficulty | `0x1d31ffce` | `0x1d00ffff` | Must match genesis |

### Why the Old Value Was Wrong

`0x1d31ffce` was arbitrary and not historically justified. It was 16x harder than Bitcoin's genesis for no clear reason.

**Principle:** Don't invent new parameters when proven standards exist.

---

## Phase 3 Pre-Mining Checklist

Before mining genesis, verify:

- [x] BlockHeader v1 = 128 bytes
- [x] Reserved field = zeroed and validated
- [x] Timestamp = 1772496000 (2026-03-03 00:00:00 UTC)
- [x] Difficulty = 0x1d00ffff (Bitcoin genesis)
- [x] ASERT anchor = 0x1d00ffff (matches genesis)
- [x] Premine difficulty = 0x1d00ffff (matches genesis)
- [x] Motto preserved exactly
- [x] Only nonce mutates during mining
- [x] Miner uses real BlockHeader type
- [x] Sanity test passes (reserved field affects hash)

---

## Immutability Guarantee

**This decision is now FROZEN.**

Once genesis is mined with `0x1d00ffff`:
- ✅ Genesis hash is permanent
- ✅ ASERT anchor is permanent
- ✅ Historical legitimacy is established
- ✅ No future changes possible without hard fork

**This document serves as the canonical record of this decision.**

---

## References

- Bitcoin genesis block: [Block 0](https://blockstream.info/block/000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f)
- Bitcoin genesis parameters: [chainparams.cpp](https://github.com/bitcoin/bitcoin/blob/master/src/chainparams.cpp#L56)
- DineroCoin Phase 3 Preflight: `docs/BLOCKHEADER_V1_FINALIZATION_PLAN.md`
- ASERT specification: `docs/consensus/asert.md`

---

## Sign-Off

**Decision Authority:** DineroCoin Core Development
**Consensus Review:** Passed
**Implementation Status:** ✅ LOCKED
**Effective Date:** 2026-01-13

**This decision cannot be changed without a coordinated hard fork.**

🔒 **Genesis difficulty = 0x1d00ffff (IMMUTABLE)**
