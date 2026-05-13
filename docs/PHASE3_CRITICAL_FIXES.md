# Phase 3: Critical Legacy Code Fixes

**Date:** 2026-01-13
**Priority:** URGENT - Must be fixed before genesis
**Status:** IN PROGRESS

---

## Issues Found

### 1. ❌ Legacy Header Size Constants

**File:** `include/consensus/header_consensus.h`

**Issue:**
```cpp
constexpr size_t CURRENT_BLOCK_HEADER_SIZE = 112;  // ❌ WRONG
static_assert(CURRENT_BLOCK_HEADER_SIZE == 112, ...);  // ❌ WRONG
```

**Fix:**
```cpp
constexpr size_t CURRENT_BLOCK_HEADER_SIZE = 128;  // ✅ CORRECT
static_assert(CURRENT_BLOCK_HEADER_SIZE == 128, ...);  // ✅ CORRECT
```

**Status:** ✅ FIXED

---

### 2. ✅ Legacy Genesis Handling - MARKED AS LEGACY

**File:** `src/consensus/genesis_canonical.cpp` (line 49)

**Issue:**
```cpp
if (header.size() == 80) {
    std::copy(header.begin(), header.end(), out.headerLE.begin());
}
```

**Risk:**
- This code uses OLD 80-byte header format (pre-BlockHeader v1)
- Should NOT be used for Phase 3 genesis regeneration

**Fix Applied:**
- ✅ Added warning comments marking this as LEGACY code
- ✅ Documented that `tools/genesis_miner_v3_correct.cpp` should be used instead
- ✅ Added TODO to deprecate this file after Phase 3

**Status:** ✅ FIXED (marked as legacy, not to be used for Phase 3)

---

### 3. ✅ getblocktemplate Field Names - FIXED

**File:** `src/rpc/methods_mining_template.cpp`

**Issue:**
- Was returning `bits` instead of `difficulty`
- Already had correct `curtime` (64-bit)
- Was missing `reserved` field

**Fix Applied:**
```cpp
// ✅ FIXED (lines 196-220):
result["difficulty"] = difficulty_hex.str();  // Changed from "bits"
result["curtime"] = static_cast<int64_t>(template_block->timestamp);  // Already correct
result["reserved"] = "000000000000000000000000";  // Added (12 zero bytes)
```

**Changes:**
- ✅ Changed field name from "bits" to "difficulty"
- ✅ Updated debug logging to use "difficulty"
- ✅ Added "reserved" field (24 hex chars = 12 bytes)
- ✅ Already had correct "curtime" (64-bit timestamp)

**Status:** ✅ FIXED

---

### 4. ⚠️ Test Files Using Legacy Field Names

**Issue:**
Multiple test files reference `.bits` and `.time` fields that don't exist in BlockHeader v1.

**BlockHeader v1 Actual Fields:**
- ✅ `difficulty` (NOT `bits`)
- ✅ `timestamp` (64-bit, NOT 32-bit `time`)

**Test files with `.bits` references:**
```
tests/consensus/test_consensus_ring2_validity.cpp (lines 249, 290, 327, 337)
tests/consensus/test_fork_choice.cpp (line 62)
tests/consensus/test_header_sync_restart.cpp (line 191)
tests/consensus/test_header_sync_stall_behavior.cpp (line 88)
tests/consensus/test_ibd_smoke.cpp (lines 47, 120, 251)
tests/consensus/test_ibd_connect.cpp (line 53)
tests/integration/test_utreexo_mining_e2e.cpp (lines 233, 305, 463, 579)
tests/reorg/test_deep_reorg.cpp (lines 64, 182, 304)
tests/network/test_bridge_node_cache.cpp (line 54)
tests/network/test_phase_7_4_2_message_dispatch.cpp (lines 427, 440)
... and many more
```

**Test files with `.time` references:**
```
tests/reorg/test_deep_reorg.cpp (line 63)
tests/reorg/test_crash_safe_reorg_rev_dat.cpp (line 68)
tests/reorg/test_tx_edge_case_reorg.cpp (line 118)
tests/network/test_phase_7_4_2_message_dispatch.cpp (lines 425, 438)
tests/integration/test_utreexo_mining_e2e.cpp (lines 232, 304, 462, 578)
... and many more
```

**Impact:**
- These tests likely don't compile or use wrong fields
- May be setting legacy fields that don't affect BlockHeader v1
- Need systematic review and update

**Action Required:**
- [ ] Audit all test files for `.bits` usage → change to `.difficulty`
- [ ] Audit all test files for `.time` usage → change to `.timestamp`
- [ ] Verify tests compile and pass after updates
- [ ] Consider: Some tests may need dual-field compatibility for migration

**Status:** ⚠️ IDENTIFIED (needs systematic fix across test suite)

---

### 5. ❌ CRITICAL BUG: Mining Engine Header Serialization - FIXED

**File:** `src/mining/miner_engine.cpp` (line 189)

**Issue:**
The `hashBlockHeader()` function was using HAND-ROLLED serialization that was COMPLETELY WRONG for BlockHeader v1:
```cpp
// ❌ WRONG (old code):
std::vector<uint8_t> header_data;
header_data.reserve(112);  // WRONG: BlockHeader v1 is 128 bytes!

// Timestamp (4 bytes, little-endian) - WRONG: Should be 8 bytes!
uint32_t time = static_cast<uint32_t>(header.timestamp);
// ... only serializes 32-bit timestamp ...

// Missing: reserved[12] field NOT serialized at all!
```

**Consequences:**
- PoW hash would be computed from WRONG data (112 bytes instead of 128)
- Timestamp truncated from 64-bit to 32-bit (data loss)
- Reserved field not included in hash (consensus violation)
- **All mined blocks would be INVALID**

**Fix Applied:**
```cpp
// ✅ FIXED: Use BlockHeader::SerializeForHash() (canonical method)
auto header_bytes = header.SerializeForHash();  // Correct 128-byte serialization
static_assert(std::tuple_size<decltype(header_bytes)>::value == 128,
              "BlockHeader::SerializeForHash() must return exactly 128 bytes");

std::vector<uint8_t> hash_raw = Dinero::Common::double_sha256_raw(
    header_bytes.data(), header_bytes.size());
```

**Why This Was Critical:**
- Mining engine is used for ALL block mining (internal and via RPC)
- Wrong serialization → wrong PoW → invalid blocks → chain breaks
- Would have been discovered on first block after genesis

**Status:** ✅ FIXED (now uses canonical BlockHeader::SerializeForHash())

---

## Pre-Genesis Verification Checklist

### Code Verification:

- [x] Header size constant updated (112 → 128) ✅
- [x] Static assertions updated ✅
- [x] Genesis canonical code reviewed (marked as legacy) ✅
- [x] getblocktemplate field names verified (difficulty, curtime, reserved) ✅
- [x] Production code `.bits` references checked ✅
- [x] Production code `.time` references checked ✅
- [x] Mining engine header serialization FIXED (critical bug) ✅
- [ ] Test files `.bits` references updated (systematic fix needed)
- [ ] Test files `.time` references updated (systematic fix needed)
- [ ] Block validation header size enforcement (needs verification)
- [ ] Runtime assertions added (temporary Phase 3 guards)

### Test Verification:

- [ ] Compile test: `sizeof(BlockHeader) == 128`
- [ ] Runtime test: Header serialization produces 128 bytes
- [ ] getblocktemplate test: Returns correct field names
- [ ] External miner test: Can mine Block 1
- [ ] Validation test: Rejects non-128-byte headers

---

## Command to Find Remaining Issues

```bash
# Find remaining .bits references (excluding ASERT)
grep -rn "\.bits\b" src/ include/ \
  | grep -v "ASERT.*BITS" \
  | grep -v "nBits" \
  | grep -v "// " \
  | grep -v "/\*"

# Find remaining .time references (excluding .timestamp)
grep -rn "\.time\b" src/ include/ \
  | grep -v "timestamp" \
  | grep -v "uptime" \
  | grep -v "// " \
  | grep -v "/\*"

# Find 80/112 byte header references
grep -rn "\(80\|112\)" src/ include/ \
  | grep -i header \
  | grep -i size
```

---

## Critical Path for Phase 3

```
1. Fix header size constants                     ✅ DONE
2. Review genesis_canonical.cpp                  ✅ DONE (marked as legacy)
3. Verify getblocktemplate field names           ✅ DONE (difficulty, curtime, reserved)
4. Search for .bits/.time references             ✅ DONE (production code clean, tests need fix)
5. Fix test suite legacy references              ⏳ TODO (systematic update needed)
6. Add runtime assertions (temporary)            ⏳ TODO
7. Test with external miner                      ⏳ TODO
8. Mine genesis                                  ⏳ WAITING (after all fixes)
9. Verify Block 1                                ⏳ WAITING (after genesis)
```

---

## DO NOT PROCEED Until:

- [ ] All legacy code reviewed/fixed
- [ ] All field names updated
- [ ] All size checks correct
- [ ] External miner tested
- [ ] getblocktemplate verified

**Mining genesis with legacy code active = CONSENSUS FORK RISK**
