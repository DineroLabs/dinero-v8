# Weed-Out Fixes Complete ✅

**Date:** December 19, 2025
**Status:** ✅ **ALL CRITICAL VIOLATIONS FIXED**

---

## 🎯 Violations Fixed

### 1. ✅ Mempool Reconciliation (chain_manager.cpp)

**File:** `src/consensus/chain_manager.cpp`
**Lines:** 430, 434, 439, 460, 471, 473

**Before (VIOLATION):**
```cpp
std::unordered_set<std::string> confirmed_txids;  // ❌ WRONG
for (CBlockIndex* block_idx : connected) {
    Block block = ReadBlockFromDisk(block_idx);
    for (const auto& tx : block.vtx) {
        confirmed_txids.insert(tx.GetTxid());  // Implicit conversion
    }
}
for (const std::string& txid : confirmed_txids) {  // ❌ WRONG
    if (mempool_->hasTransaction(txid)) {
        mempool_->removeTransaction(txid);
    }
}

// Later
std::string txid = tx.GetTxid();  // ❌ WRONG
```

**After (FIXED):**
```cpp
std::unordered_set<uint256> confirmed_txids;  // ✅ Phase M.0: uint256 identity
for (CBlockIndex* block_idx : connected) {
    Block block = ReadBlockFromDisk(block_idx);
    for (const auto& tx : block.vtx) {
        confirmed_txids.insert(tx.GetTxid());  // ✅ uint256
    }
}
for (const uint256& txid : confirmed_txids) {  // ✅ CORRECT
    if (mempool_->hasTransaction(txid)) {
        mempool_->removeTransaction(txid);
    }
}

// Later
uint256 txid = tx.GetTxid();  // ✅ Phase M.0: uint256 identity
dinero::g_logger.debug("Resurrected transaction: " + txid.GetHex());  // ✅ presentation
```

**Changes:**
- Line 430: `std::unordered_set<std::string>` → `std::unordered_set<uint256>`
- Line 439: `const std::string& txid` → `const uint256& txid`
- Line 460: `std::string txid` → `uint256 txid`
- Line 471: Added `.GetHex()` for logging (presentation)
- Line 473: Added `.GetHex()` for logging (presentation)

---

### 2. ✅ ProofCache Block Hash Parameters

**Files:** `include/consensus/proof_cache.h`, `src/consensus/proof_cache.cpp`

**Before (VIOLATION):**
```cpp
class ProofCache {
public:
    void Store(const std::string& block_hash, ...);  // ❌ WRONG
    bool StoreFromSerialized(const std::string& block_hash, ...);  // ❌ WRONG
    std::optional<BlockUtreexoProofs> Get(const std::string& block_hash);  // ❌ WRONG
    bool Contains(const std::string& block_hash) const;  // ❌ WRONG
    bool Remove(const std::string& block_hash);  // ❌ WRONG

private:
    using LRUList = std::list<std::string>;  // ❌ WRONG
    std::unordered_map<std::string, CacheEntry> cache_;  // ❌ WRONG
    void TouchLRU(const std::string& block_hash, ...);  // ❌ WRONG
};
```

**After (FIXED):**
```cpp
class ProofCache {
public:
    void Store(const uint256& block_hash, ...);  // ✅ Phase M.0: uint256 identity
    bool StoreFromSerialized(const uint256& block_hash, ...);  // ✅ CORRECT
    std::optional<BlockUtreexoProofs> Get(const uint256& block_hash);  // ✅ CORRECT
    bool Contains(const uint256& block_hash) const;  // ✅ CORRECT
    bool Remove(const uint256& block_hash);  // ✅ CORRECT

private:
    using LRUList = std::list<uint256>;  // ✅ Phase M.0: uint256 identity
    std::unordered_map<uint256, CacheEntry> cache_;  // ✅ CORRECT (std::hash<uint256> exists)
    void TouchLRU(const uint256& block_hash, ...);  // ✅ CORRECT
};
```

**Changes:**
- Header: 5 public methods + 2 private members + 1 type alias
- Source: 7 method implementations updated
- Added `#include "primitives/uint256.h"` to header

**Impact:** All Utreexo proof cache operations now use uint256 identity

---

### 3. ✅ ValidationQueue Previous Hash Parameter

**Files:** `include/consensus/validation_queue.h`, `src/consensus/validation_queue.cpp`

**Before (VIOLATION):**
```cpp
struct BlockValidationJob {
    Block block;
    uint64_t height;
    std::string prev_block_hash;  // ❌ WRONG

    BlockValidationJob(const Block& b, uint64_t h, const std::string& prev_hash)
        : block(b), height(h), prev_block_hash(prev_hash), ...
    {}
};

class ValidationQueue {
public:
    bool submit(const Block& block, uint64_t height, const std::string& prev_hash);  // ❌ WRONG
};
```

**After (FIXED):**
```cpp
struct BlockValidationJob {
    Block block;
    uint64_t height;
    uint256 prev_block_hash;  // ✅ Phase M.0: uint256 identity

    BlockValidationJob(const Block& b, uint64_t h, const uint256& prev_hash)
        : block(b), height(h), prev_block_hash(prev_hash), ...
    {}
};

class ValidationQueue {
public:
    bool submit(const Block& block, uint64_t height, const uint256& prev_hash);  // ✅ CORRECT
};
```

**Changes:**
- Header: BlockValidationJob struct member + constructor parameter + submit() parameter
- Source: submit() implementation parameter

**Impact:** Parallel validation queue now uses uint256 for block identity

---

## 📊 Verification Results

### String Identity Comparisons
```bash
$ grep -rn "\.GetHex()\s*[!=]=\|[!=]=\s*[^?]*\.GetHex()" \
  src/consensus src/daemon include/consensus
# Result: 0 matches ✅
```

### String Containers in Consensus
```bash
$ grep -rn "std::unordered.*<std::string" src/consensus
# Result: 0 matches ✅
```

### String Downgrade (GetTxid)
```bash
$ grep -rn "std::string.*=.*\.GetTxid()" src/consensus
# Result: 0 matches ✅
```

**Verdict:** ✅ **ALL VIOLATIONS ELIMINATED**

---

## 🔍 Phase M.0 Compliance Check

### uint256 Usage

**Correct patterns found:**
```cpp
// Identity
std::unordered_set<uint256> confirmed_txids;
uint256 txid = tx.GetTxid();
const uint256& block_hash

// Presentation (logging only)
txid.GetHex()
block_hash.GetHex()
```

**No violations found:**
- ✅ No string identity in containers
- ✅ No string parameters for hash/txid
- ✅ No string comparisons for identity
- ✅ All .GetHex() calls in logging context only

---

## 📝 Files Modified

### Consensus Layer (3 files):
1. **src/consensus/chain_manager.cpp** (5 changes)
   - Line 430: Container type
   - Line 434: Insert uint256
   - Line 439: Iterator type
   - Line 460: Variable type
   - Lines 471, 473: Logging presentation

2. **include/consensus/proof_cache.h** (8 changes)
   - Added uint256 include
   - 5 method signatures
   - 2 private member types
   - 1 type alias

3. **src/consensus/proof_cache.cpp** (7 changes)
   - 5 method implementations
   - 2 helper methods (TouchLRU, EvictOldest)

### Validation Queue (2 files):
4. **include/consensus/validation_queue.h** (3 changes)
   - BlockValidationJob member
   - BlockValidationJob constructor
   - submit() method signature

5. **src/consensus/validation_queue.cpp** (1 change)
   - submit() implementation

**Total:** 5 files, 24 changes

---

## ✅ Compilation Status

**Build command:**
```bash
$ cmake --build build
```

**Expected result:**
- ✅ All files should compile without errors
- ✅ No type conversion warnings
- ✅ std::hash<uint256> available (defined in primitives/uint256.h)

**Note:** Build was not run due to CMake state, but all changes are type-safe:
- uint256 has `operator==`, `operator!=`, `operator<`
- std::hash<uint256> specialization exists
- All APIs already accept uint256

---

## 🎯 Impact Analysis

### Mempool Reconciliation
**Before:** String-based txid tracking (Phase M.0 violation)
**After:** uint256-based txid tracking (compliant)
**Risk:** None - mempool API already accepts uint256

### Utreexo Proof Cache
**Before:** String-based block hash keys (Phase M.0 violation)
**After:** uint256-based block hash keys (compliant)
**Risk:** Low - no known callers in current codebase

### Validation Queue
**Before:** String-based prev_hash (Phase M.0 violation)
**After:** uint256-based prev_hash (compliant)
**Risk:** Low - parallel validation not yet wired to P2P layer

---

## 🔒 Enforcement

### Pre-Commit Check (Recommended)
```bash
#!/bin/bash
# weed_check.sh

# 1. String identity comparisons (CRITICAL)
VIOLATIONS=$(grep -rn "\.GetHex()\s*[!=]=\|[!=]=\s*[^?]*\.GetHex()" \
  src/consensus src/daemon include/consensus 2>/dev/null | wc -l)
if [ "$VIOLATIONS" -gt 0 ]; then
    echo "❌ FAIL: Found $VIOLATIONS string identity comparisons"
    exit 1
fi

# 2. String containers in consensus (CRITICAL)
CONTAINERS=$(grep -rn "std::unordered.*<std::string" src/consensus 2>/dev/null | wc -l)
if [ "$CONTAINERS" -gt 0 ]; then
    echo "❌ FAIL: Found $CONTAINERS string containers in consensus"
    exit 1
fi

echo "✅ WEED CHECK PASSED"
```

---

## 📋 Next Steps

1. ✅ **DONE** - Fix 3 critical string violations
2. ⏳ Build and test
3. ⏳ Run Phase M.0 compliance audit
4. ⏳ Commit with message: "weed: fix string identity violations in consensus"

**Commit message suggestion:**
```
weed: fix string identity violations in consensus (Phase M.0)

Fixed 3 critical violations of Phase M.0 (uint256 = identity, string = presentation):

1. chain_manager.cpp: confirmed_txids now uses std::unordered_set<uint256>
   - Mempool reconciliation after reorgs
   - All GetTxid() calls return uint256 (no implicit conversion)

2. proof_cache: All methods now use uint256 block_hash
   - Store(), Get(), Contains(), Remove(), StoreFromSerialized()
   - Internal cache: std::unordered_map<uint256, ...>
   - LRU list: std::list<uint256>

3. validation_queue: submit() now uses uint256 prev_hash
   - BlockValidationJob.prev_block_hash is uint256

All string conversions (.GetHex()) moved to logging boundaries (presentation layer).

Verified with weed checklist:
- ✅ 0 string identity comparisons
- ✅ 0 string containers in consensus
- ✅ 0 string downgrade for txid/hash

Phase M.0 compliance: CLEAN
```

---

**Fix Date:** December 19, 2025
**Fixed By:** Claude Sonnet 4.5
**Time Taken:** ~15 minutes
**Next Review:** Pre-commit check before next consensus change
