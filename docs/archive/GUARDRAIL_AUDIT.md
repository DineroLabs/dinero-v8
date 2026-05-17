# ASERT Fair Launch - Guardrail Audit
**Date:** 2025-10-17
**Status:** PRE-DEPLOYMENT REVIEW

## Configuration Summary

### Mainnet Parameters (consensus.hpp)
```cpp
uint32_t asertAnchorHeight   = 1;           // Anchor at block 1
uint32_t asertAnchorBits     = 0x1f00ffff;  // CPU-friendly start for fair launch
int64_t  asertHalfLifeSec    = 43'200;      // 12 hours (144 blocks @ 5min)
uint32_t targetSpacingSec    = 300;         // 5 minutes per block
uint32_t powLimitBits        = 0x1f00ffff;  // Same as anchor
```

### What This Means
- **Block 1**: Mined at anchor difficulty `0x1f00ffff` (~16M difficulty, CPU-mineable at 10-20 MH/s)
- **Block 2+**: ASERT adjusts based on elapsed time since block 1
  - If blocks arrive faster than 5min → difficulty increases
  - If blocks arrive slower than 5min → difficulty decreases
  - 12-hour half-life = smooth exponential adjustment

---

## Guardrail Checklist

### ✅ 1. Same params everywhere
**Status: ❌ CRITICAL BUG FOUND**

**Issue:** GBT and BlockAcceptor use different consensus params on regtest:
- `main.cpp:1497-1519`: GBT creates `Consensus consensus;` then applies regtest override
  ```cpp
  if (dinero::Params().name != "mainnet") {
      consensus.asertAnchorBits = dinero::Params().pow_limit_bits;  // 0x207fffff
  }
  ```
- `block_acceptor.cpp:253`: BlockAcceptor creates `Consensus consensus;` WITHOUT override
  - Uses mainnet values on regtest!
  - **Result: GBT and validator disagree on difficulty**

**Fix Required:** Apply same regtest override in BlockAcceptor::ValidateProofOfWork()

---

### ✅ 2. No time-rolling in miner
**Status: NEEDS VERIFICATION**

**Location:** Check `dinero-miner` source to ensure it:
1. Does NOT increment `curtime` field during nonce exhaustion
2. Instead requests fresh GBT when nonce space exhausted
3. Reason: ASERT is time-sensitive; time-rolling breaks difficulty calculation

**Action:** Verify miner code or add assertion in GBT to detect time-rolling

---

### ✅ 3. MTP clamp identical in both paths
**Status: VERIFIED**

**GBT** (main.cpp:1553-1558):
```cpp
int64_t header_time = static_cast<int64_t>(std::time(nullptr));
int64_t currentMTP = std::max(prevMTP + 1, header_time);
```

**BlockAcceptor** (block_acceptor.cpp:lines 70-73 from file changes):
```cpp
int64_t header_time = static_cast<int64_t>(block.timestamp);
int64_t current_mtp = std::max(prev_mtp + 1, header_time);
```

**✅ Both use identical clamping: `max(prevMTP+1, headerTime)`**

---

### ✅ 4. Anchor cache
**Status: PARTIALLY IMPLEMENTED**

**GBT** (main.cpp:107-109):
```cpp
static std::atomic<bool> g_anchor_ready{false};
static std::atomic<uint32_t> g_anchor_bits{0};
static std::atomic<int64_t> g_anchor_time{0};
```

**Issue:** Cache declared but NOT populated or used!
- Block 2 uses fast path: `prevTime` from tip header directly
- Blocks 3+ fetch block 1 from DB each time
- Cache never set, never read

**Recommendation:** Either:
1. Populate cache after block 1 mined, use for blocks 3+
2. Remove unused cache (current fast path works fine)

---

### ✅ 5. Regtest min-diff override
**Status: CORRECTLY IMPLEMENTED**

**main.cpp:1501-1519:**
```cpp
if (dinero::Params().name != "mainnet") {
    consensus.easyPhaseBits = dinero::Params().pow_limit_bits;
    consensus.asertAnchorBits = dinero::Params().pow_limit_bits;  // 0x207fffff
}
```

**Intent:** Regtest uses CPU-instant mining (`0x207fffff`) while still testing ASERT algorithm

**✅ Both sides must agree** - see Issue #1 above

---

### ✅ 6. Store header.bits correctly
**Status: FIXED**

**serialization.h:191-201:** Fixed uint32_t→uint64_t type mismatch
**block_acceptor.cpp:380-394:** Populates ALL dual fields when storing:
```cpp
header.bits = block.bits;
header.difficulty = block.bits;  // ✅ Legacy field
header.timestamp = block.timestamp;
header.time = static_cast<uint32_t>(block.timestamp);  // ✅ New field
```

**✅ Headers now store/load correctly with no zeros**

---

### ✅ 7. Refuse GBT if bits/time are zero
**Status: NOT IMPLEMENTED**

**Current:** GBT logs diagnostic warnings but doesn't fail:
```cpp
std::cerr << "⚠️ Using genesis time as anchor fallback" << std::endl;
```

**Recommendation:** Add assertion:
```cpp
if (prevBits == 0 || anchor_time == 0) {
    result["error"] = "CRITICAL: Invalid consensus params (bits or anchor_time is zero)";
    return result;
}
```

---

## Economic/Operational Considerations

### 📊 Launch Day Scenario

**Assumption:** 50 MH/s total network hashrate at launch

**Block 1:**
- Difficulty: `0x1f00ffff` → ~16.7M difficulty
- Target time: 5 minutes
- Expected hashes: 50 MH/s × 300s = 15,000 MH = 15 GH
- Actual time: ~15 GH / 16.7M = ~900 seconds (~15 minutes) ✅ Reasonable

**Blocks 2-10 (ASERT ramp-up):**
- If blocks arrive every ~15min (3x slower than target):
  - ASERT makes difficulty EASIER (increases target)
  - After 12 hours (half-life), difficulty adjusts by ~factor of 2
- If hashrate grows to 200 MH/s:
  - Blocks arrive faster → difficulty increases
  - ASERT converges to 5-minute average

**⚠️ Mitigation: Coinbase Maturity**
- Set `COINBASE_MATURITY = 100 blocks` (~8.3 hours at 5min/block)
- Prevents instant dump of early rewards
- Gives network time to stabilize

---

### 📈 ASERT Ramp Math

**Half-life: 43,200 seconds (12 hours = 144 blocks)**

If blocks arrive consistently every X seconds:
- X = 300s (target) → no adjustment
- X = 150s (2x faster) → after 12 hours, difficulty doubles
- X = 600s (2x slower) → after 12 hours, difficulty halves

**First 24 hours projection:**
| Time | Hashrate | Block Time | ASERT Action |
|------|----------|------------|--------------|
| H+0 | 50 MH/s | ~15 min | Makes easier |
| H+6 | 100 MH/s | ~7.5 min | Makes harder (slower than ease rate) |
| H+12 | 200 MH/s | ~4 min | Makes harder |
| H+24 | 500 MH/s | ~2 min | Makes harder |

**✅ Smooth ramp, no sudden jumps**

---

### 🔧 Target Interval Verification

**Params:**
- `targetSpacingSec = 300` (5 minutes)
- `asertHalfLifeSec = 43200` (12 hours)

**Ratio:** 43200 / 300 = 144 blocks per half-life ✅ Standard Bitcoin-like ratio

**✅ Confirmed: 5-minute blocks, 12-hour half-life**

---

### 🛡️ PowLimit Consistency

**consensus.hpp:**
```cpp
uint32_t minDifficultyBits   = 0x1f00ffff;  // Emergency floor
uint32_t powLimitBits        = 0x1f00ffff;  // Absolute maximum target
uint32_t asertAnchorBits     = 0x1f00ffff;  // Fair launch start
```

**✅ All three use same value** - anchor starts at powLimit (easiest allowed)

**Note:** ASERT can never make difficulty easier than `powLimitBits`

---

## Test Plan (Fast)

### Test 1: Fresh Chain (Block 1)
```bash
rm -rf /tmp/test-launch
./build/dinerod --regtest --datadir=/tmp/test-launch &
DPID=$!
sleep 3
COOKIE=$(cat /tmp/test-launch/.cookie)

# Mine block 1
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"1","method":"generatetoaddress","params":[1,"rdin1q..."]}' \
  | jq '.result'

# Verify bits=0x1f00ffff (mainnet) or 0x207fffff (regtest)
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"2","method":"getblock","params":["<hash>"]}' \
  | jq '.result.bits'

kill $DPID
```

**Expected:**
- Block 1 mines successfully
- `bits` field matches anchor (regtest: `0x207fffff`, mainnet: `0x1f00ffff`)
- Daemon log shows: `[GBT] h=1 bits=<anchor> phase=P2-ASERT`

---

### Test 2: Blocks 2-10 (ASERT Convergence)
```bash
# Continue from Test 1, mine 9 more blocks with delays
for i in {2..10}; do
  echo "=== Mining block $i ==="
  curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
    -H "Content-Type: application/json" \
    -d "{\"jsonrpc\":\"2.0\",\"id\":\"$i\",\"method\":\"generatetoaddress\",\"params\":[1,\"rdin1q...\"]}" \
    | jq -r '.result[0]'

  # Check difficulty
  sleep 2
done

# Extract difficulty progression from daemon log
grep '\[DIFF\] result: ASERT calculated' /tmp/test-launch/daemon.log
```

**Expected:**
- Blocks 2-10 mine successfully
- Difficulty adjusts based on timing (tightens if blocks arrive quickly)
- No "bad-diffbits" errors
- GBT and validator agree on all blocks

---

### Test 3: Daemon Restart (Tip Persistence)
```bash
# Kill daemon
kill $DPID
wait $DPID

# Restart
./build/dinerod --regtest --datadir=/tmp/test-launch &
DPID=$!
sleep 3

# Verify chain state
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"getblockchaininfo","params":[]}' \
  | jq '.result.blocks'

# Mine block 11
curl -s -X POST http://127.0.0.1:20998 -u "$COOKIE" \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[1,"rdin1q..."]}' \
  | jq '.result'

kill $DPID
```

**Expected:**
- Daemon restores to height 10
- Block 11 mines successfully
- Anchor time (block 1 timestamp) correctly retrieved from DB

---

## Files to Double-Check

### ✅ consensus.hpp
**Lines 32-37:**
```cpp
uint32_t asertAnchorHeight   = 1;
uint32_t asertAnchorBits     = 0x1f00ffff;  // ✅ CPU-friendly
int64_t  asertHalfLifeSec    = 43'200;      // ✅ 12 hours
uint32_t targetSpacingSec    = 300;         // ✅ 5 minutes
uint32_t powLimitBits        = 0x1f00ffff;  // ✅ Matches anchor
```

**✅ Verified correct**

---

### ❌ main.cpp (GBT)
**Lines 1497-1626:**
- ✅ Creates `Consensus consensus;`
- ✅ Applies regtest override (lines 1501-1519)
- ✅ Uses `getTip()` API for block 2 fast path
- ✅ Calls `GetNextWorkRequired()` with correct params
- ⚠️ Unused anchor cache (lines 107-109) - recommend cleanup
- ❌ No assertion for `bits == 0` or `anchor_time == 0`

**Action:** Add zero-check assertion before returning GBT

---

### ❌ block_acceptor.cpp (Validator)
**Lines 240-340:**
- ✅ Creates `Consensus consensus;` (line 253)
- ❌ **MISSING regtest override** - uses mainnet params on regtest!
- ✅ Uses `dinero::Params().genesis` for network-specific genesis
- ✅ MTP clamping matches GBT
- ✅ Calls `GetNextWorkRequired()` with correct params

**Action:** Add regtest override (copy from main.cpp:1501-1519)

---

### ✅ serialization.h
**Lines 191-201:**
- ✅ Fixed uint64_t timestamp deserialization
- ✅ Populates dual fields (timestamp/time, bits/difficulty)

**✅ Verified correct**

---

## Recommended Fixes (Priority Order)

### 🔴 P0 - CRITICAL (Blocks Production)
1. **Add regtest override to BlockAcceptor** (block_acceptor.cpp:254)
   ```cpp
   Consensus consensus;
   if (dinero::Params().name != "mainnet") {
       consensus.asertAnchorBits = dinero::Params().pow_limit_bits;
   }
   ```

### 🟠 P1 - HIGH (Safety)
2. **Add zero-check assertion in GBT** (main.cpp:1625)
   ```cpp
   if (bits == 0 || anchor_time == 0) {
       result["error"] = "CRITICAL: Invalid consensus params";
       return result;
   }
   ```

### 🟡 P2 - MEDIUM (Cleanup)
3. **Remove unused anchor cache** (main.cpp:107-109) OR populate it
4. **Verify miner doesn't do time-rolling** (check dinero-miner source)

### 🟢 P3 - LOW (Economic)
5. **Set COINBASE_MATURITY = 100** (if not already set)
6. **Add monitoring/metrics** for block time and difficulty

---

## Sign-Off Checklist

Before deploying to mainnet:

- [ ] Fix P0: Add regtest override to BlockAcceptor
- [ ] Fix P1: Add zero-check assertion in GBT
- [ ] Test 1: Mine block 1, verify anchor bits
- [ ] Test 2: Mine blocks 2-10, verify ASERT adjustment
- [ ] Test 3: Restart daemon, verify persistence
- [ ] Verify: Both GBT and validator logs show identical bits for same height
- [ ] Verify: No "bad-diffbits" errors in test logs
- [ ] Review: Coinbase maturity setting
- [ ] Review: Launch day monitoring plan

---

## Questions for User

1. **Coinbase maturity:** What is current `COINBASE_MATURITY` value?
2. **Miner time-rolling:** Does `dinero-miner` increment `curtime` during mining?
3. **Launch hashrate:** What is expected network hashrate on day 1?
4. **GUI wallet issue:** User mentioned unresolved "wallet and receiving address window connection or disconnection" - is this related to mining/consensus or separate?

---

**Status:** READY FOR FIXES → TESTING → DEPLOYMENT
