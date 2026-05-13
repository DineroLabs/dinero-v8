# Phase 3: Template Builder Layer - CRITICAL AUDIT

**Date:** 2026-01-13
**Status:** 🚨 **MULTIPLE CRITICAL BUGS FOUND**
**Priority:** URGENT - Must fix before genesis

---

## Executive Summary

Found **MULTIPLE** components with hand-rolled header serialization using **WRONG** sizes:
- ❌ 80 bytes (Bitcoin legacy)
- ❌ 112 bytes (old DineroCoin transitional)

**BlockHeader v1 MUST be 128 bytes everywhere.**

---

## Critical Bugs Found

### 1. ❌ BlockTemplateManager::buildBlockHeader()

**File:** `src/mining/mining_coordinator.cpp` (line 80)

**Bug:**
```cpp
// Serialize DineroCoin block header (112 bytes):  // ❌ WRONG - should be 128
// ...
// Timestamp (4 bytes, little-endian)  // ❌ WRONG - should be 8 bytes
uint32_t time32 = static_cast<uint32_t>(timestamp);
// ...
// Missing: reserved[12] field
```

**Impact:** Any miner using this will create INVALID blocks

**Status:** ✅ **FIXED** - Now uses `BlockHeader::SerializeForHash()`

**Used by:**
- `src/stratum_bridge/stratum_server_integrated.cpp`
- `src/rpc/methods_miner_control.cpp`
- `src/mining/worker_interface.cpp`

---

### 2. ❌ MiningEngine::BuildBlockHeader()

**File:** `src/daemon/mining_engine.cpp` (line 521)

**Bug:**
```cpp
// Version (4 bytes) + PrevHash (32 bytes) + MerkleRoot (32 bytes) +
// Timestamp (4 bytes) + Bits (4 bytes) + Nonce (4 bytes) = 80 bytes  // ❌ WRONG

header.reserve(80);  // Block header is always 80 bytes  // ❌ WRONG

// Timestamp (4 bytes, little-endian)  // ❌ WRONG - should be 8 bytes
WriteLE32(header, work.timestamp);

// Missing: utreexo_root (32 bytes)
// Missing: reserved[12] (12 bytes)
// Missing: 64-bit timestamp
```

**Impact:** Builds 80-byte Bitcoin-style headers (WRONG for DineroCoin)

**Status:** ⚠️ **NEEDS FIX**

**Used by:**
- `src/daemon/gbt_work_manager.cpp`
- `src/daemon/mining_engine_fixed.cpp` (might be fixed version?)

---

### 3. ⚠️ mining/miner_engine.cpp

**File:** `src/mining/miner_engine.cpp` (line 189)

**Status:** ✅ **ALREADY FIXED** (commit f2502f68)

Now uses `BlockHeader::SerializeForHash()` correctly.

---

## Template Builder Components (All Must Use 128 Bytes)

### RPC Layer ✅
- `src/rpc/methods_mining_template.cpp` - getblocktemplate
  - **Status:** ✅ FIXED - Returns correct fields

### Mining Template ⚠️
- `src/mining/block_template.cpp` - BlockTemplateBuilder
  - **Status:** ❓ NEEDS CHECK - Does NOT directly serialize headers
- `src/mining/mining_coordinator.cpp` - BlockTemplateManager
  - **Status:** ✅ FIXED

### Block Assembler ❓
- `src/mining/block_assembler.cpp` - BlockAssembler
  - **Status:** ❓ NEEDS CHECK - May not directly serialize headers

### Mining Engines ⚠️
- `src/mining/miner_engine.cpp` - MiningEngine (internal)
  - **Status:** ✅ FIXED
- `src/daemon/mining_engine.cpp` - MiningEngine (GBT)
  - **Status:** ❌ NEEDS FIX (80-byte headers)

### Stratum Server ❓
- `src/stratum_bridge/stratum_server.cpp` - Basic server
- `src/stratum_bridge/stratum_server_integrated.cpp` - Uses BlockTemplateManager
- `src/stratum_bridge/stratum_server_unified.cpp`
- `src/stratum_bridge/stratum_server_complete.cpp`
  - **Status:** ❓ NEEDS CHECK - May use BlockTemplateManager (fixed)

---

## The Correct Pattern

**ALL template builders MUST use:**

```cpp
// ✅ CORRECT: Use canonical serialization
auto header_bytes = header.SerializeForHash();  // Returns std::array<uint8_t, 128>

// If you need std::vector:
return std::vector<uint8_t>(header_bytes.begin(), header_bytes.end());

// If you need std::string:
return std::string(reinterpret_cast<const char*>(header_bytes.data()), header_bytes.size());
```

**❌ NEVER hand-roll serialization:**

```cpp
// ❌ WRONG - hand-rolled serialization is ALWAYS wrong
std::vector<uint8_t> header;
header.reserve(80);  // or 112, or any size
header.push_back(...);  // manual byte-by-byte
```

---

## Verification Commands

### Find all hand-rolled header serialization:

```bash
# Search for reserve(80) or reserve(112)
grep -rn "reserve(80)\|reserve(112)" src/mining/ src/daemon/ src/stratum_bridge/

# Search for "80 bytes" or "112 bytes" comments
grep -rn "80 bytes\|112 bytes" src/mining/ src/daemon/ src/stratum_bridge/

# Search for manual timestamp serialization (32-bit)
grep -rn "uint32_t time\|time32\|timestamp.*uint32" src/mining/ src/daemon/ src/stratum_bridge/

# Search for functions that build headers
grep -rn "buildBlockHeader\|BuildBlockHeader\|serializeHeader" src/mining/ src/daemon/ src/stratum_bridge/
```

### Results:
```
src/mining/mining_coordinator.cpp:80: buildBlockHeader  ✅ FIXED
src/daemon/mining_engine.cpp:521: BuildBlockHeader  ❌ NEEDS FIX
```

---

## Required Fixes

### Fix #1: src/daemon/mining_engine.cpp (line 521)

**Replace entire function:**

```cpp
std::string MiningEngine::BuildBlockHeader(const WorkTemplate& work, uint32_t nonce, uint32_t extraNonce) {
    // ✅ CRITICAL FIX (Phase 3): Use BlockHeader::SerializeForHash()
    //
    // Previous code was WRONG:
    // - Built 80-byte Bitcoin-style headers
    // - Truncated timestamp to 32-bit
    // - Missing utreexo_root
    // - Missing reserved[12]
    //
    // BlockHeader v1 MUST be exactly 128 bytes

    // Build BlockHeader from WorkTemplate
    BlockHeader header;
    header.version = work.version;
    header.prev_block_hash = uint256::FromHexUnsafe(work.blockHash);
    header.merkle_root = uint256::FromHexUnsafe(work.merkle_root);
    header.utreexo_root = uint256();  // Zero for now (will be set by template builder)
    header.timestamp = work.timestamp;
    header.difficulty = work.bits;
    header.nonce = nonce;
    header.ZeroReserved();

    // Use canonical serialization (returns std::array<uint8_t, 128>)
    auto header_bytes = header.SerializeForHash();

    // Convert to string for return
    return std::string(reinterpret_cast<const char*>(header_bytes.data()), header_bytes.size());
}
```

**Also update BuildCompleteBlock comment:**
```cpp
// OLD:
block.reserve(200);  // Approximate: 80 (header) + ~120 (coinbase tx)

// NEW:
block.reserve(250);  // Approximate: 128 (header) + ~120 (coinbase tx)
```

---

## Action Items (Priority Order)

1. **URGENT:** Fix `src/daemon/mining_engine.cpp` BuildBlockHeader()
   - Replace hand-rolled serialization with BlockHeader::SerializeForHash()
   - Update all comments from 80/112 → 128 bytes

2. **VERIFY:** Check Stratum server implementations
   - Confirm they use BlockTemplateManager (already fixed)
   - Add assertions if they serialize headers directly

3. **AUDIT:** BlockAssembler
   - Verify it doesn't serialize headers directly
   - If it does, fix it

4. **TEST:** After all fixes
   - Compile and test getblocktemplate
   - Test with Stratum server (if used)
   - Verify header size in all paths

---

## Testing Checklist

After fixes, verify:

- [ ] getblocktemplate returns 128-byte header fields
- [ ] BlockTemplateManager::buildBlockHeader returns 128 bytes
- [ ] MiningEngine::BuildBlockHeader returns 128 bytes
- [ ] Stratum server (if used) sends 128-byte headers
- [ ] All miners (internal/external) receive 128-byte templates
- [ ] No 80/112-byte paths remain active

---

## Summary of Findings

**Components with WRONG serialization:**
- ❌ `src/daemon/mining_engine.cpp` - 80 bytes (CRITICAL)
- ✅ `src/mining/mining_coordinator.cpp` - 112 bytes (FIXED)
- ✅ `src/mining/miner_engine.cpp` - 112 bytes (FIXED)

**Impact if not fixed:**
- External miners using MiningEngine will mine INVALID blocks
- Stratum miners using affected code will mine INVALID blocks
- Chain will break on first mined block

**Status:** 2 of 3 fixed, 1 remaining (daemon/mining_engine.cpp)

---

🚨 **CRITICAL: Fix remaining component before genesis**
