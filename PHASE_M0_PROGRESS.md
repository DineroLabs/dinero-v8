# Phase M.0 - Type Hygiene Progress Report

**Date:** December 19, 2025
**Status:** Header files complete - Implementation in progress

---

## ✅ Completed (Steps 1-3 + partial Step 4)

### Step 1: Canonical OutPoint ✅
- **Commit:** 38424da1
- **File:** `include/consensus/outpoint.h`
- **Changes:**
  - Created single source of truth for OutPoint
  - `uint256 txid` + `uint32_t vout`
  - Hashable for unordered containers
  - ToString()/FromString() for RPC boundary only

### Step 2: TxMempoolEntry Migration ✅
- **Commit:** 11cd2010
- **File:** `include/daemon/tx_mempool.h`
- **Changes:**
  - `TxMempoolEntry::txid` now uint256
  - `GetTxIdHex()` helper for RPC
  - Orphan pool uses uint256 keys
  - Dependency tracking uses uint256 sets
  - All TxMempool methods use uint256 parameters
  - UTXOView interface updated to uint256

### Step 3: MempoolEntry Migration ✅
- **Commit:** 6f174fd5
- **File:** `include/mempool/mempool.h`
- **Changes:**
  - `MempoolEntry::txid` now uint256
  - `GetTxIdHex()` helper for RPC
  - Ancestry tracking (parents/children/ancestors) uses uint256
  - All Mempool class methods use uint256 parameters
  - Storage maps and indexes updated to uint256
  - Conflict tracking maps to uint256

### Step 4a: daemon::Mempool Header ✅
- **Commit:** 296b828a
- **File:** `include/daemon/mempool.h`
- **Changes:**
  - Mempool class methods use uint256 parameters
  - `isOutputSpentInMempool` now uses OutPoint directly
  - `MempoolEntry.depends` → `std::vector<uint256>`
  - `MempoolEntry.spends` → `std::vector<OutPoint>`
  - `m_spent_outputs` → `std::unordered_set<OutPoint>`
  - All indexes updated to uint256

### Step 4b-4e: daemon/mempool.cpp Implementation ✅
- **Commits:** 94a5f941, d13d1907, 27f47c29, 50da212d
- **File:** `src/daemon/mempool.cpp` (1923 lines)
- **Completed (4-pass approach):**
  - PASS 1: Conflict tracking - string outpoints → OutPoint structs
  - PASS 2: Method signatures - all APIs migrated to uint256
  - PASS 3: Internal containers - maps/sets/vectors to uint256
  - PASS 4: Logging - all logging uses GetHex() at boundary
- **Verification:** ✅ All grep checks pass

---

## 🚧 Remaining Work

### Step 4c: Complete daemon/mempool.cpp Implementation
**File:** `src/daemon/mempool.cpp` (estimated 3-4 hours)

**Key areas needing fixes:**

#### 1. Conflict Tracking Logic (Lines 194-210)
```cpp
// OLD (string concatenation):
std::string outpoint = input.prevout.txid + ":" + std::to_string(input.prevout.vout);
if (m_spent_outputs.find(outpoint) != m_spent_outputs.end()) { ... }

// NEW (OutPoint struct):
OutPoint outpoint{input.prevout.hash, input.prevout.n};
if (m_spent_outputs.find(outpoint) != m_spent_outputs.end()) { ... }
```

#### 2. Entry.spends Population (Lines 445-448)
```cpp
// OLD:
for (const auto& input : tx.vin) {
    std::string outpoint = input.prevout.txid + ":" + std::to_string(input.prevout.vout);
    m_spent_outputs.insert(outpoint);
    entry.spends.push_back(outpoint);
}

// NEW:
for (const auto& input : tx.vin) {
    OutPoint outpoint{input.prevout.hash, input.prevout.n};
    m_spent_outputs.insert(outpoint);
    entry.spends.push_back(outpoint);
}
```

#### 3. Methods Needing Parameter Updates
- `removeTransaction(const uint256& txid)` - Line ~500
- `hasTransaction(const uint256& txid)` - Line ~530
- `getTransaction(const uint256& txid)` - Line ~540
- `getMempoolEntry(const uint256& txid)` - Line ~550
- `getTransactionFee(const uint256& txid)` - Line ~560
- `getTransactionFeeRate(const uint256& txid)` - Line ~570
- `broadcastTransaction(const uint256& txid)` - Line ~600
- `removeConfirmedTransactions(const std::vector<uint256>&)` - Line ~620
- `updateDependencies(const uint256& txid)` - Line ~650
- `isOutputSpentInMempool(const OutPoint& outpoint)` - Line 1162

#### 4. RBF Conflict Resolution (Lines 250-268)
- Conflicting txids vector → uint256
- Iteration over conflicts → uint256
- Entry removal → uint256

#### 5. TEST_ONLY Mode (Lines 680-850)
- Similar conflict tracking fixes
- Outpoint construction using OutPoint
- Entry iteration using uint256

#### 6. Utility Methods
- `getTransactionIds()` → return `std::vector<uint256>`
- `selectTransactionsForBlock()` → iterate with uint256
- `clear()` → already correct (clears containers)

#### 7. Logging Statements
All logging that prints txid needs `.GetHex()`:
```cpp
// OLD:
MPLOG_DEBUG("Transaction " + txid + " rejected");

// NEW:
MPLOG_DEBUG("Transaction " + txid.GetHex() + " rejected");
```

---

### Step 7: RPC Boundary Conversion
**Estimated:** 2 hours

**Files to update:**
- `src/rpc/methods_mempool*.cpp`
- `src/rpc/methods_blockchain*.cpp` (any mempool calls)
- `src/daemon/rpc/rpc_mempool.cpp`

**Pattern:**
```cpp
// RPC input (hex string → uint256):
std::string txid_hex = params["txid"].get<std::string>();
uint256 txid = uint256::FromHexUnsafe(txid_hex);

// RPC output (uint256 → hex string):
result["txid"] = txid.GetHex();
```

---

### Step 8: Delete Obsolete OutPoint Definitions
**Estimated:** 30 minutes

**Files:**
1. `include/consensus/utxo_entry.h` - Delete OutPoint (lines 88-110)
2. `include/mempool/coins_view_mempool.h` - Delete OutPoint (lines 17-30)
3. `include/p2p/consensus_validator.h` - Delete OutPoint (lines 113-118)

**Verification:**
```bash
grep -r "struct OutPoint" include/ src/ | grep -v "consensus/outpoint.h"
# Should return ZERO matches
```

---

### Step 9: Verification
**Estimated:** 1 hour

**Grep Checks:**
```bash
# No string txid in mempool headers
grep -r "std::string txid" include/daemon/tx_mempool.h include/mempool/mempool.h include/daemon/mempool.h
# Should return ZERO matches (except comments)

# No string concatenation for outpoints
grep -r "txid.*:.*vout" src/daemon/mempool.cpp | grep -v ToString | grep -v FromString
# Should return ZERO matches

# Only one OutPoint definition
grep -r "struct OutPoint" include/ src/ | grep -v "consensus/outpoint.h"
# Should return ZERO matches
```

**Compilation Test:**
```bash
make -j$(nproc) 2>&1 | tee /tmp/m0_final_compile.log
# Should complete successfully
```

---

### Step 10: Lock Document
**Estimated:** 30 minutes

Create `PHASE_M0_TYPE_HYGIENE_LOCK.md` documenting:
- Invariants enforced
- Single OutPoint definition location
- RPC boundary rules
- Prohibited patterns
- Commit history

---

## Time Estimate Summary

| Task | Status | Time Remaining |
|------|--------|----------------|
| Step 4c: daemon/mempool.cpp | In Progress | 3-4 hours |
| Step 7: RPC boundary | Pending | 2 hours |
| Step 8: Delete duplicates | Pending | 30 min |
| Step 9: Verification | Pending | 1 hour |
| Step 10: Lock document | Pending | 30 min |
| **TOTAL** | - | **~7-8 hours** |

---

## Current Blockers

1. **daemon/mempool.cpp implementation** must be completed before compilation succeeds
2. Headers expect uint256, implementation still uses std::string in many places
3. Transaction::GetHash() vs GetTxid() - need to verify which returns uint256

---

## Next Session Commands

```bash
# Resume at daemon/mempool.cpp line 194
# Fix conflict tracking first (highest priority)

# Pattern to search for remaining work:
grep -n "std::string.*txid\|txid.*:.*vout" src/daemon/mempool.cpp

# After fixes, verify:
make -j$(nproc)
```

---

## Commit Strategy (Remaining)

```
M0.4 - daemon/mempool.cpp conflict tracking (Step 4c)
M0.5 - RPC boundary conversion (Step 7)
M0.6 - Delete duplicate OutPoints (Step 8)
M0.LOCK - Documentation lock (Step 10)
```

---

## Success Criteria

✅ All headers use uint256 (DONE)
⏳ All implementations use uint256 (IN PROGRESS)
⏳ Single OutPoint definition (PENDING deletion of duplicates)
⏳ RPC conversion at boundary only (PENDING)
⏳ Zero string concatenation for identity (IN PROGRESS)
⏳ All tests pass (PENDING)
⏳ Lock document created (PENDING)

---

**Last Updated:** Step 4a complete (commit 296b828a)
**Next Action:** Complete daemon/mempool.cpp implementation
