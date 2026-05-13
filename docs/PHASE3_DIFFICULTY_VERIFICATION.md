# Phase 3: Difficulty Consistency Verification

**Date:** 2026-01-13
**Status:** ✅ VERIFIED - All values aligned
**Difficulty:** 0x1d00ffff (Bitcoin genesis difficulty)

---

## Critical Invariant

```
genesis.difficulty == premine.difficulty == ASERT_ANCHOR_BITS == 0x1d00ffff
```

**Why this matters:**
- Genesis sets the initial difficulty
- ASERT anchors at block 1 (premine)
- If these diverge → silent consensus fork

---

## Verification Results

### 1️⃣ Genesis Miner (tools/genesis_miner_v3_correct.cpp)

**Line 363:**
```cpp
const uint32_t difficulty = 0x1d00ffff;  // Bitcoin genesis difficulty
```

✅ **Verified:** Genesis miner will mine with 0x1d00ffff

---

### 2️⃣ ASERT Anchor (include/consensus/asert_params.h)

**Line 36:**
```cpp
static constexpr uint32_t ASERT_ANCHOR_BITS = 0x1d00ffff;
```

**Line 78 (static assertion):**
```cpp
static_assert(ASERT_ANCHOR_BITS == 0x1d00ffff,
    "ASERT anchor bits must match genesis difficulty");
```

**Line 111 (consensus guard):**
```cpp
static_assert(ASERT_ANCHOR_BITS == 0x1d00ffff,
    "🔒 CONSENSUS VIOLATION: ASERT anchor bits changed! Network will fork.");
```

✅ **Verified:** ASERT will anchor at 0x1d00ffff (enforced at compile-time)

---

### 3️⃣ Genesis Block Header (src/consensus/chainparams_impl.cpp)

**Line 65:**
```cpp
.nBits = 0x1d00ffff,  // Bitcoin genesis difficulty (maximum target, difficulty = 1)
.nNonce = 0,  // Will be mined with BlockHeader v1 (128 bytes)
```

✅ **Verified:** Genesis block will be created with 0x1d00ffff

---

### 4️⃣ Genesis Initialization (src/daemon/genesis_init.cpp)

**Line 74:**
```cpp
genesis.header.difficulty = params.genesis.nBits;  // Reads 0x1d00ffff from chainparams
```

**Line 82-85 (NEW - Runtime assertion):**
```cpp
// 🧪 PHASE 3 SAFETY ASSERTION: Verify genesis difficulty matches ASERT anchor
// This prevents silent consensus divergence between genesis and ASERT
assert(genesis.header.difficulty == ASERTConsensus::ASERT_ANCHOR_BITS &&
       "FATAL: Genesis difficulty must match ASERT anchor bits (0x1d00ffff)");
```

**Line 168-176 (Premine block):**
```cpp
premine.header.difficulty = params.genesis.nBits;  // Reads 0x1d00ffff

// 🧪 PHASE 3 SAFETY ASSERTION: Verify premine difficulty matches ASERT anchor
assert(premine.header.difficulty == ASERTConsensus::ASERT_ANCHOR_BITS &&
       "FATAL: Premine (block 1) difficulty must match ASERT anchor bits (0x1d00ffff)");
```

✅ **Verified:** Runtime assertions will catch any divergence immediately

---

## Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│ PHASE 3 DIFFICULTY CONSISTENCY                                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  chainparams_impl.cpp                                          │
│  ┌─────────────────────────────────────────┐                  │
│  │  genesis.nBits = 0x1d00ffff            │                  │
│  └──────────────┬──────────────────────────┘                  │
│                 │                                              │
│                 ▼                                              │
│  genesis_init.cpp                                             │
│  ┌─────────────────────────────────────────┐                  │
│  │  genesis.header.difficulty =            │                  │
│  │    params.genesis.nBits (0x1d00ffff)   │                  │
│  │                                         │                  │
│  │  assert(genesis.header.difficulty ==    │                  │
│  │         ASERT_ANCHOR_BITS)              │ ◄───┐           │
│  └─────────────────────────────────────────┘     │           │
│                                                    │           │
│  asert_params.h                                   │           │
│  ┌─────────────────────────────────────────┐     │           │
│  │  ASERT_ANCHOR_BITS = 0x1d00ffff        │ ────┘           │
│  │                                         │                  │
│  │  static_assert(                         │                  │
│  │    ASERT_ANCHOR_BITS == 0x1d00ffff)    │                  │
│  └─────────────────────────────────────────┘                  │
│                                                                 │
│  genesis_miner_v3_correct.cpp                                  │
│  ┌─────────────────────────────────────────┐                  │
│  │  const uint32_t difficulty = 0x1d00ffff │                  │
│  │                                         │                  │
│  │  header.difficulty = difficulty         │                  │
│  └─────────────────────────────────────────┘                  │
│                                                                 │
│  ✅ ALL VALUES MATCH: 0x1d00ffff                               │
└─────────────────────────────────────────────────────────────────┘
```

---

## Failure Modes (Now Prevented)

### Scenario 1: Genesis/ASERT Mismatch
```cpp
// If someone accidentally changed one but not the other:
genesis.nBits = 0x1d00ffff
ASERT_ANCHOR_BITS = 0x1d31ffce  // ❌ WRONG
```

**Prevention:**
- Runtime assertion in genesis_init.cpp will **immediately abort**
- Static assertion in asert_params.h prevents compilation if value changed

### Scenario 2: Miner Uses Wrong Difficulty
```cpp
// If genesis_miner used different difficulty than chainparams:
miner: 0x1d31ffce
chainparams: 0x1d00ffff
```

**Result:**
- Mined genesis hash won't match expected hash in chainparams
- Block validation will reject the genesis block
- Node won't start

### Scenario 3: Premine Difficulty Diverges
```cpp
// If premine block used different difficulty:
genesis: 0x1d00ffff
premine: 0x1d31ffce  // ❌ WRONG
ASERT_ANCHOR_BITS: 0x1d00ffff
```

**Prevention:**
- Runtime assertion in CreatePremineBlock() will **immediately abort**
- ASERT will fail at block 2 (anchor mismatch)

---

## Phase 3 Pre-Mining Checklist

Before mining genesis, verify:

- [x] **Genesis miner difficulty:** 0x1d00ffff ✅
- [x] **Chainparams genesis.nBits:** 0x1d00ffff ✅
- [x] **ASERT_ANCHOR_BITS constant:** 0x1d00ffff ✅
- [x] **Compile-time assertions:** Present and passing ✅
- [x] **Runtime assertions:** Added to genesis_init.cpp ✅
- [x] **Premine difficulty:** Will match genesis ✅

**Command to verify:**
```bash
# All three values should show 0x1d00ffff
grep -rn "0x1d00ffff" \
  tools/genesis_miner_v3_correct.cpp \
  src/consensus/chainparams_impl.cpp \
  include/consensus/asert_params.h
```

**Expected output:**
```
tools/genesis_miner_v3_correct.cpp:363:    const uint32_t difficulty = 0x1d00ffff;
src/consensus/chainparams_impl.cpp:65:        .nBits = 0x1d00ffff,
include/consensus/asert_params.h:36:    static constexpr uint32_t ASERT_ANCHOR_BITS = 0x1d00ffff;
include/consensus/asert_params.h:78:    static_assert(ASERT_ANCHOR_BITS == 0x1d00ffff,
include/consensus/asert_params.h:111:    static_assert(ASERT_ANCHOR_BITS == 0x1d00ffff,
```

---

## When to Remove Assertions

**After Phase 3 is complete:**

1. Genesis block mined and verified
2. Premine block mined and verified
3. Both blocks accepted by consensus
4. ASERT activated at block 2 without issues

**Then remove:**
- `assert(genesis.header.difficulty == ASERTConsensus::ASERT_ANCHOR_BITS)` (genesis_init.cpp line ~83)
- `assert(premine.header.difficulty == ASERTConsensus::ASERT_ANCHOR_BITS)` (genesis_init.cpp line ~175)

**Keep forever:**
- All `static_assert` statements in asert_params.h (compile-time guards)
- GENESIS_DIFFICULTY_DECISION.md (historical record)

---

## Compile-Time Verification

Run this command to verify all assertions compile:

```bash
g++ -std=c++20 -I include -I src -c src/daemon/genesis_init.cpp -o /tmp/test.o
echo $?  # Should be 0 (success)
```

If compilation fails with "ASERT anchor bits must match genesis difficulty" → values diverged.

---

## Sign-Off

**Verification Date:** 2026-01-13
**Verified By:** Automated consistency check
**All Values:** 0x1d00ffff ✅
**Runtime Assertions:** ✅ Added
**Compile-Time Guards:** ✅ Enforced

**Status:** SAFE TO MINE GENESIS

🔒 **This verification ensures Phase 3 cannot silently produce a consensus fork.**
