# Weed-Out Report - Post-Foundation Invariant Violations

**Date:** December 19, 2025
**Scope:** Consensus paths + daemon integration
**Status:** 🔴 **VIOLATIONS FOUND** - Requires fixes

---

## Executive Summary

**Invariants Checked:**
1. ✅ No string identity comparisons (`.GetHex() == ` patterns)
2. ⚠️ **1 VIOLATION:** String containers in consensus
3. ⚠️ **25 TODOs** in consensus paths
4. ✅ No string concatenation for identity
5. ⚠️ **2 VIOLATIONS:** String parameters for block_hash

**Critical Fixes Required:** 3
**Medium Priority:** 25 TODOs to review
**Low Priority:** Code consolidation

---

## 🔴 CRITICAL VIOLATIONS (Must Fix Immediately)

### 1. String Container for Transaction IDs

**File:** `src/consensus/chain_manager.cpp:430-444`

```cpp
// VIOLATION: Using std::string for txids
std::unordered_set<std::string> confirmed_txids;  // ❌ WRONG
for (CBlockIndex* block_idx : connected) {
    Block block = ReadBlockFromDisk(block_idx);
    for (const auto& tx : block.vtx) {
        confirmed_txids.insert(tx.GetTxid());  // ❌ GetTxid() returns uint256, not string
    }
}

// Later usage
for (const std::string& txid : confirmed_txids) {  // ❌ WRONG type
    if (mempool_->hasTransaction(txid)) {
        mempool_->removeTransaction(txid);
    }
}
```

**Problem:**
- `Transaction::GetTxid()` returns `uint256` (correct)
- But storing in `std::unordered_set<std::string>` (wrong)
- This violates Phase M.0: uint256 is identity, string is presentation

**Fix:**
```cpp
// CORRECT:
std::unordered_set<uint256> confirmed_txids;  // ✅ Identity type
for (CBlockIndex* block_idx : connected) {
    Block block = ReadBlockFromDisk(block_idx);
    for (const auto& tx : block.vtx) {
        confirmed_txids.insert(tx.GetTxid());  // ✅ uint256
    }
}

for (const uint256& txid : confirmed_txids) {  // ✅ CORRECT
    // Need to check if mempool API accepts uint256 or hex string
    // If mempool uses string keys (boundary), convert here:
    if (mempool_->hasTransaction(txid.GetHex())) {
        mempool_->removeTransaction(txid.GetHex());
    }
}
```

**Impact:** Mempool reconciliation after reorgs
**Priority:** HIGH
**Effort:** 10 minutes

---

### 2. String Parameter for Block Hash (ProofCache)

**File:** `src/consensus/proof_cache.cpp:54`

```cpp
bool ProofCache::StoreFromSerialized(const std::string& block_hash,  // ❌ WRONG
                                    const std::vector<uint8_t>& serialized_proofs) {
    // ...
    Store(block_hash, proofs);
    return true;
}

std::optional<BlockUtreexoProofs> ProofCache::Get(const std::string& block_hash) {  // ❌ WRONG
    // ...
}
```

**Problem:**
- Block identity should be `uint256`, not `std::string`
- Violates Phase M.0

**Fix:**
```cpp
bool ProofCache::StoreFromSerialized(const uint256& block_hash,  // ✅ CORRECT
                                    const std::vector<uint8_t>& serialized_proofs) {
    // ...
}

std::optional<BlockUtreexoProofs> ProofCache::Get(const uint256& block_hash) {  // ✅ CORRECT
    // ...
}
```

**Impact:** Utreexo proof caching
**Priority:** HIGH
**Effort:** 15 minutes (need to update all callers)

---

### 3. String Parameter for Previous Hash (ValidationQueue)

**File:** `src/consensus/validation_queue.cpp:161`

```cpp
bool ValidationQueue::submit(const Block& block,
                            uint64_t height,
                            const std::string& prev_hash) {  // ❌ WRONG
    // ...
}
```

**Problem:**
- Previous block hash should be `uint256`, not `std::string`

**Fix:**
```cpp
bool ValidationQueue::submit(const Block& block,
                            uint64_t height,
                            const uint256& prev_hash) {  // ✅ CORRECT
    // ...
}
```

**Impact:** Parallel validation queue
**Priority:** MEDIUM
**Effort:** 10 minutes

---

## ⚠️ SERIALIZATION BOUNDARY (Needs Review)

### String Data in Deserialization

**File:** `src/consensus/block_index_persistence.cpp:77`

```cpp
BlockIndexEntry BlockIndexEntry::Deserialize(const std::string& data) {  // ⚠️ CHECK
    // Uses data.data() and data.size() for memcpy
    std::memcpy(dest, data.data() + offset, size);
}
```

**Analysis:**
- This is using `std::string` as a byte buffer (common pattern in C++)
- NOT a hash/identity - just binary data container
- **VERDICT:** ✅ **ACCEPTABLE** (binary serialization, not identity)

**Note:** C++17 `std::byte` or `std::vector<uint8_t>` would be clearer, but this is not a violation.

---

## 📋 TODO INVENTORY (25 found in consensus paths)

### Critical TODOs (Affect Correctness):

1. **`src/consensus/validation_queue.cpp:447`**
   ```cpp
   // TODO: Store undo data for reorg support
   ```
   **Status:** Missing feature for reorg safety
   **Priority:** HIGH (blocks production reorgs)

2. **`src/consensus/chain_manager.cpp:842`**
   ```cpp
   // TODO: Need to track UTXO-to-block mapping to implement this check
   ```
   **Status:** Missing validation check
   **Priority:** MEDIUM

3. **`src/consensus/block_index_persistence.cpp:359-371`**
   ```cpp
   // TODO: This requires access to ChainDB instance
   // TODO: Call BlockIndexDB::SaveBlockIndices(indices)
   ```
   **Status:** Incomplete persistence
   **Priority:** HIGH (restart safety)

### Future Work TODOs (Marked as such):

4. **`src/consensus/header_sync_manager.cpp:107`**
   ```cpp
   // TODO: Implement orphan header handling (future work)
   ```
   **Status:** Explicitly deferred
   **Priority:** LOW

5. **`src/consensus/header_sync_manager.cpp:232`**
   ```cpp
   // TODO: Implement actual PoW validation using ASERT difficulty
   ```
   **Status:** Using simplified PoW currently
   **Priority:** MEDIUM (correctness)

6. **`src/consensus/header_sync_manager.cpp:247`**
   ```cpp
   // TODO: Implement BIP113 median-time-past validation
   ```
   **Status:** Timestamp validation incomplete
   **Priority:** MEDIUM (consensus rule)

### Validation TODOs:

7. **`src/consensus/validation_worker_pool.cpp:231`**
   ```cpp
   // TODO: Store prev_spk and prev_value in task struct (extend if needed)
   ```
   **Priority:** MEDIUM

8. **`src/consensus/validation_worker_pool.cpp:414`**
   ```cpp
   // TODO: Integrate full signature verification
   ```
   **Priority:** HIGH (correctness)

9. **`src/consensus/parallel_block_validator.cpp:470`**
   ```cpp
   // TODO: Script verification would go here
   ```
   **Priority:** HIGH (correctness)

### Monitoring/Metrics TODOs:

10-15. **`src/consensus/chain_manager.cpp`** (7 TODOs)
    - Lines 156, 198, 710, 711, 722, 723, 842, 921
    - Mostly interface adaptation and metrics
    - **Priority:** LOW-MEDIUM

### Serialization TODOs:

16. **`src/consensus/tx_parser.cpp:290`**
    ```cpp
    // TODO: Add SegWit serialization when witness data is stored
    ```
    **Priority:** MEDIUM

17. **`src/consensus/script_verify.cpp:707`**
    ```cpp
    // TODO: Handle larger scripts with proper compact size encoding
    ```
    **Priority:** LOW

### Acceptable TODOs:

18. **`src/consensus/utxo_set.cpp:124`**
    ```cpp
    // TODO: If we want accurate count, need to scan ChainDB
    ```
    **Status:** Optimization, not correctness
    **Priority:** LOW

19. **`include/consensus/coin_type.h:26`**
    ```cpp
    // TODO: Replace with official coin type after SLIP-44 registration
    ```
    **Status:** External dependency
    **Priority:** LOW

20. **`include/consensus/pow_anchor_util.hpp:28`**
    ```cpp
    // TODO: Implement actual chain walking via ChainDB
    ```
    **Priority:** MEDIUM

### Found in Backup Files (Ignore):

21-25. **`src/consensus/header_sync_manager.cpp.backup`** and other `.backup` files
    - Not in active code
    - **Action:** Can be deleted

---

## ✅ CLEAN CHECKS (No Violations)

### 1. String Identity Comparisons
```bash
$ grep -rn "\.GetHex()\s*[!=]=\|[!=]=\s*[^?]*\.GetHex()" src/consensus src/daemon include/consensus
# Result: 0 matches ✅ CLEAN
```

**Verdict:** ✅ **NO VIOLATIONS** - Phase M.0 compliant

### 2. Early String Downgrade (Non-Logging)
```bash
$ grep -rn "std::string.*=.*\.GetHex()" src/consensus | grep -v "logger\|LOG\|RPC\|json\|substr"
# Result: 0 matches ✅ CLEAN
```

**Verdict:** ✅ **NO VIOLATIONS** - String conversions only in logging

### 3. String Concatenation for Identity
```bash
$ grep -rn "GetHex().*\\+" src/consensus
$ grep -rn "\".*:\".*GetHex" src/consensus
# Result: 0 matches ✅ CLEAN
```

**Verdict:** ✅ **NO VIOLATIONS** - No manual key construction

### 4. Duplicate Implementations
```bash
$ grep -rn "PerformReorg\|ActivateBestChain" src | grep "bool.*("
# Result: Only ActivateBestChain found (PerformReorg removed)
```

**Verdict:** ✅ **CLEAN** - Old PerformReorg removed

---

## 🔍 CODE CONSOLIDATION OPPORTUNITIES

### Legacy Headers-First Sync Implementations

**Found:**
- `src/daemon/p2p/headers_sync.cpp` - Unused Qt implementation
- Phase H (HeaderSyncManager) is the canonical implementation

**Recommendation:**
- Delete legacy implementation
- Reduces confusion, maintenance burden
- **Priority:** LOW (cleanup, not correctness)

---

## 📊 Violation Summary by Severity

| Severity | Count | Category | Action Required |
|----------|-------|----------|-----------------|
| 🔴 CRITICAL | 3 | String identity in consensus | Fix immediately |
| 🟡 HIGH | 6 | Missing features (TODOs) | Plan implementation |
| 🟢 MEDIUM | 10 | Incomplete validation | Review and prioritize |
| ⚪ LOW | 9 | Optimizations, cleanup | Defer |

---

## 🛠️ Recommended Fix Order

### Phase 1: Critical String Violations (Today)

1. **Fix confirmed_txids in chain_manager.cpp**
   - Change to `std::unordered_set<uint256>`
   - Update mempool API calls
   - **Estimated time:** 15 minutes

2. **Fix ProofCache block_hash parameter**
   - Change to `const uint256&`
   - Update all callers
   - **Estimated time:** 20 minutes

3. **Fix ValidationQueue prev_hash parameter**
   - Change to `const uint256&`
   - Update all callers
   - **Estimated time:** 10 minutes

**Total Phase 1 time:** ~45 minutes

### Phase 2: High-Priority TODOs (This Week)

4. **Undo data storage (validation_queue.cpp:447)**
5. **Full signature verification (validation_worker_pool.cpp:414)**
6. **Script verification (parallel_block_validator.cpp:470)**
7. **BlockIndex persistence (block_index_persistence.cpp:359-371)**

### Phase 3: Medium-Priority TODOs (Next Week)

8. PoW validation (ASERT difficulty)
9. BIP113 median-time-past
10. SegWit serialization

### Phase 4: Cleanup (When Convenient)

11. Remove backup files
12. Delete legacy headers_sync.cpp
13. Remove low-priority TODOs with implementations

---

## 🔒 Enforcement Going Forward

### Pre-Commit Checks (Add to CI)

```bash
#!/bin/bash
# weed_check.sh - Run before commits

echo "=== WEED CHECK ==="

# 1. String identity violations
VIOLATIONS=$(grep -rn "\.GetHex()\s*[!=]=\|[!=]=\s*[^?]*\.GetHex()" \
  src/consensus src/daemon include/consensus 2>/dev/null | wc -l)
if [ "$VIOLATIONS" -gt 0 ]; then
    echo "❌ FAIL: Found $VIOLATIONS string identity comparisons in consensus"
    exit 1
fi

# 2. String containers in consensus
CONTAINERS=$(grep -rn "std::unordered.*<std::string" src/consensus 2>/dev/null | wc -l)
if [ "$CONTAINERS" -gt 0 ]; then
    echo "⚠️  WARNING: Found $CONTAINERS string containers in consensus (review manually)"
fi

# 3. TODOs in consensus
TODOS=$(grep -rn "TODO\|FIXME\|XXX" src/consensus include/consensus 2>/dev/null | \
  grep -v "test\|adapter\|backup" | wc -l)
echo "📋 Found $TODOS TODOs in consensus (review periodically)"

echo "✅ WEED CHECK COMPLETE"
```

### Quarterly Review

Run full weed checklist every 3 months:
1. String identity violations
2. Container type violations
3. TODO inventory
4. Duplicate implementations
5. Serialization boundaries

---

## 📝 Next Steps

**Immediate (Today):**
1. ✅ Document violations (this report)
2. ⏳ Fix 3 critical string violations
3. ⏳ Run full build + tests
4. ⏳ Commit with message: "weed: fix string identity violations in consensus"

**This Week:**
5. Review high-priority TODOs
6. Plan implementation for missing features
7. Add weed check to pre-commit hooks

**This Month:**
8. Implement high-priority TODOs
9. Remove legacy code (headers_sync.cpp)
10. Clean up backup files

---

**Audit Date:** December 19, 2025
**Auditor:** Claude Sonnet 4.5
**Next Review:** Weekly until all critical violations fixed, then quarterly
