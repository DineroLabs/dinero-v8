# Phase M.0 Implementation Guide - Mempool Type Hygiene

**Date:** December 19, 2025
**Status:** Ready for Mechanical Execution
**Session Goal:** Replace identity with truth - no architectural decisions

---

## 1. Invariants (Non-Negotiable Truth)

### What M.0 IS
✅ Replace `std::string txid` with `uint256` throughout mempool
✅ Unify to single canonical `OutPoint` definition
✅ Move RPC string conversion to boundary only
✅ Remove string concatenation for transaction identity
✅ Delete type adapters and compatibility shims

### What M.0 IS NOT
❌ Changing algorithms (fee estimation, RBF logic stays identical)
❌ Changing policies (BIP125 rules, limits unchanged)
❌ Touching network protocol (relay unchanged)
❌ Rewriting data structures (multimap indexes stay)

### Why M.0 is Mandatory (Not Optional)

**Post-H.6 Reality:**
- Consensus layer uses `uint256` everywhere (locked)
- ChainDB uses `uint256` for block/tx identity (locked)
- Documentation states "no string fallback in core" (locked)

**String-based mempool would:**
1. Create type conversion at consensus boundary (violates H.6)
2. Require string→uint256→string round trips (performance poison)
3. Allow accidental identity mixing (safety violation)
4. Force future rewrite under economic testing pressure (technical debt bomb)

**Conclusion:** M.0 is a **prerequisite**, not a choice.

---

## 2. Current State Analysis

### Three Incompatible OutPoint Definitions (Problem)

**Location 1:** `consensus/utxo_entry.h:88`
```cpp
struct OutPoint {
    std::string txid;  // ❌ Wrong type
    uint32_t vout;
};
```

**Location 2:** `mempool/coins_view_mempool.h:17`
```cpp
struct OutPoint {
    std::string txid;  // ❌ "uint256 alias" comment is a lie
    uint32_t vout;
};
```

**Location 3:** `p2p/consensus_validator.h:113` (CORRECT)
```cpp
struct OutPoint {
    Hash256 hash;      // ✅ Proper type (Hash256 = uint256)
    uint32_t index;
};
```

**Problem:** Three definitions with incompatible types, no clear authority.

### String-Based Transaction Identity (Problem)

**daemon/tx_mempool.h:25**
```cpp
struct TxMempoolEntry {
    Transaction tx;
    std::string txid;  // ❌ Should be uint256
    ...
};
```

**mempool/mempool.h:36**
```cpp
struct MempoolEntry {
    Transaction tx;
    std::string txid;  // ❌ Should be uint256
    ...
};
```

**Conflict Tracking (daemon/mempool.cpp:195)**
```cpp
std::string outpoint = input.prevout.txid + ":" + std::to_string(input.prevout.vout);
m_spent_outputs.insert(outpoint);  // ❌ String concatenation for identity
```

---

## 3. Canonical OutPoint Definition (Step 1 - Foundation)

### File: `include/consensus/outpoint.h` (NEW FILE)

**Create the One True OutPoint:**

```cpp
#pragma once

#include "primitives/uint256.h"
#include <cstdint>
#include <functional>
#include <string>

namespace dinero {

/**
 * OutPoint - Canonical transaction output identifier
 *
 * Phase M.0: Single source of truth for (txid, vout) identity
 *
 * Invariants:
 * - txid is uint256 (never string in core logic)
 * - RPC conversion happens at boundary only
 * - Equality is structural (txid == txid && vout == vout)
 * - Hashable for unordered containers
 */
struct OutPoint {
    uint256 txid;
    uint32_t vout;

    OutPoint() : vout(0) {}
    OutPoint(const uint256& hash, uint32_t n) : txid(hash), vout(n) {}

    // Equality
    bool operator==(const OutPoint& other) const {
        return txid == other.txid && vout == other.vout;
    }

    bool operator!=(const OutPoint& other) const {
        return !(*this == other);
    }

    // Ordering (for maps)
    bool operator<(const OutPoint& other) const {
        if (txid != other.txid) {
            return txid < other.txid;
        }
        return vout < other.vout;
    }

    // NOTE: RPC/logging boundary only. Core logic must not depend on these methods.
    // Use for: RPC input/output, logging, debugging only
    // Never use for: identity comparison, storage keys, algorithm logic
    std::string ToString() const {
        return txid.GetHex() + ":" + std::to_string(vout);
    }

    static OutPoint FromString(const std::string& str) {
        size_t colon = str.find(':');
        if (colon == std::string::npos) {
            return OutPoint{};
        }
        uint256 hash = uint256::FromHex(str.substr(0, colon));
        uint32_t n = std::stoul(str.substr(colon + 1));
        return OutPoint{hash, n};
    }
};

} // namespace dinero

// std::hash specialization for unordered containers
namespace std {
template<>
struct hash<dinero::OutPoint> {
    size_t operator()(const dinero::OutPoint& outpoint) const {
        // Combine txid hash with vout
        size_t h1 = std::hash<dinero::uint256>{}(outpoint.txid);
        size_t h2 = std::hash<uint32_t>{}(outpoint.vout);
        return h1 ^ (h2 << 1);
    }
};
} // namespace std
```

**Critical:** This is the **only** OutPoint definition going forward.

---

## 4. Migration Plan (Step-by-Step)

### Step 1: Create Canonical OutPoint
- [ ] Create `include/consensus/outpoint.h` with definition above
- [ ] Ensure `primitives/uint256.h` has `std::hash` specialization
- [ ] Add to `CMakeLists.txt` if needed
- [ ] Verify compiles standalone

### Step 2: Replace TxMempoolEntry (daemon/tx_mempool.h)

**Before:**
```cpp
struct TxMempoolEntry {
    Transaction tx;
    std::string txid;  // Line 25
    ...
};
```

**After:**
```cpp
#include "consensus/outpoint.h"

struct TxMempoolEntry {
    Transaction tx;
    uint256 txid;  // Changed from string
    ...

    // Helper: Get txid as hex (RPC only)
    std::string GetTxIdHex() const { return txid.GetHex(); }
};
```

**Update Constructor (if exists):**
```cpp
TxMempoolEntry(const Transaction& tx, uint64_t fee, int64_t time)
    : tx(tx)
    , txid(tx.GetHash())  // Use uint256, not string
    , fee(fee)
    , time(time)
{ }
```

### Step 3: Replace MempoolEntry (mempool/mempool.h)

**Before:**
```cpp
struct MempoolEntry {
    Transaction tx;
    std::string txid;  // Line 36
    ...
};
```

**After:**
```cpp
#include "consensus/outpoint.h"

struct MempoolEntry {
    Transaction tx;
    uint256 txid;  // Changed from string
    ...

    // RPC boundary helper
    std::string GetTxIdHex() const { return txid.GetHex(); }
};
```

### Step 4: Fix Conflict Tracking (daemon/mempool.cpp)

**Before (Lines 193-196):**
```cpp
for (const auto& input : tx.vin) {
    std::string outpoint = input.prevout.txid + ":" + std::to_string(input.prevout.vout);
    if (m_spent_outputs.find(outpoint) != m_spent_outputs.end()) {
        // Conflict detected
    }
}
```

**After:**
```cpp
for (const auto& input : tx.vin) {
    OutPoint outpoint{input.prevout.hash, input.prevout.n};
    if (m_spent_outpoints.find(outpoint) != m_spent_outpoints.end()) {
        // Conflict detected
    }
}
```

**Update Member Variable:**
```cpp
// Before
std::unordered_set<std::string> m_spent_outputs;

// After
std::unordered_set<OutPoint> m_spent_outpoints;
```

### Step 5: Fix Orphan Pool (daemon/tx_mempool.h)

**Before:**
```cpp
std::unordered_map<std::string, OrphanMeta> orphans_;
std::unordered_map<std::string, std::vector<std::string>> orphan_children_;
```

**After:**
```cpp
std::unordered_map<uint256, OrphanMeta> orphans_;
std::unordered_map<uint256, std::vector<uint256>> orphan_children_;
```

### Step 6: Fix Ancestry Tracking (mempool/mempool.h)

**Before:**
```cpp
std::unordered_set<std::string> parents;
std::unordered_set<std::string> children;
std::vector<std::string> ancestors;
```

**After:**
```cpp
std::unordered_set<uint256> parents;
std::unordered_set<uint256> children;
std::vector<uint256> ancestors;
```

### Step 7: RPC Boundary (Single Conversion Point)

**Pattern for ALL RPC methods:**

**Input (RPC → Core):**
```cpp
din::Json rpc_gettransaction(const ExecutionContext& ctx, const din::Json& params) {
    // RPC receives string
    std::string txid_hex = params["txid"].get<std::string>();

    // Convert ONCE at boundary
    uint256 txid = uint256::FromHex(txid_hex);

    // Pass uint256 to core
    auto tx = mempool.getTransaction(txid);
    ...
}
```

**Output (Core → RPC):**
```cpp
din::Json response;
response["txid"] = tx.txid.GetHex();  // Convert at boundary
return response;
```

**Never:**
```cpp
❌ auto tx = mempool.getTransaction(txid_hex);  // String leaking into core
❌ std::string txid = tx.GetTxid();  // Core returning string
```

### Step 8: Delete Obsolete OutPoint Definitions

**Files to modify:**

1. **consensus/utxo_entry.h**
   - Delete `struct OutPoint` (lines 88-110)
   - Replace with `#include "consensus/outpoint.h"`

2. **mempool/coins_view_mempool.h**
   - Delete `struct OutPoint` (lines 17-30)
   - Replace with `#include "consensus/outpoint.h"`

3. **p2p/consensus_validator.h**
   - Delete `struct OutPoint` (lines 113-118)
   - Replace with `#include "consensus/outpoint.h"`

**Verify no other definitions exist:**
```bash
grep -r "struct OutPoint" include/ src/ --exclude-dir=.git
```

Should return **ONLY** `include/consensus/outpoint.h`.

---

## 5. Transaction Class Integration

### Verify Transaction::GetHash() Returns uint256

**Check wallet/transaction.h:**

```cpp
class Transaction {
public:
    uint256 GetHash() const;  // ✅ Must return uint256
    std::string GetTxid() const { return GetHash().GetHex(); }  // RPC helper only
};
```

**If Transaction uses string internally, fix it first:**

```cpp
// Before
std::string txid;

// After
mutable uint256 cached_hash;  // Lazy-computed
mutable bool hash_valid = false;

uint256 GetHash() const {
    if (!hash_valid) {
        cached_hash = ComputeHash();
        hash_valid = true;
    }
    return cached_hash;
}
```

---

## 6. Compilation Fix Checklist

### Common Errors and Fixes

**Error 1: No matching function for `getTransaction(const std::string&)`**

**Fix:** Update method signature:
```cpp
// Before
std::shared_ptr<Transaction> getTransaction(const std::string& txid) const;

// After
std::shared_ptr<Transaction> getTransaction(const uint256& txid) const;
```

**Error 2: Cannot convert `uint256` to `std::string`**

**Fix:** Add `.GetHex()` at RPC boundary only:
```cpp
// Before
result["txid"] = tx.txid;

// After
result["txid"] = tx.txid.GetHex();
```

**Error 3: `std::unordered_map<uint256, T>` fails to compile**

**Fix:** Ensure `std::hash<uint256>` is defined in `primitives/uint256.h`:
```cpp
namespace std {
template<>
struct hash<dinero::uint256> {
    size_t operator()(const dinero::uint256& h) const {
        return h.GetHash();  // Or appropriate hash method
    }
};
}
```

**Error 4: `OutPoint` ambiguous**

**Fix:** Delete all duplicate definitions (Step 8 above).

---

## 7. Test Verification Points

### After Each Step, Verify:

**Step 2-3 (TxMempoolEntry/MempoolEntry):**
```bash
# Compile check
make -j$(nproc) 2>&1 | tee /tmp/m0_compile.log

# Search for remaining string txid
grep -r "std::string txid" include/daemon/tx_mempool.h include/mempool/mempool.h
# Should return ZERO matches
```

**Step 4 (Conflict Tracking):**
```bash
# Verify no string concatenation for outpoints
grep -r "txid.*:.*vout" src/daemon/mempool.cpp
# Should return ZERO matches (only in ToString() for logging)
```

**Step 7 (RPC Boundary):**
```bash
# Check RPC methods use GetHex() for output
grep -r "txid.GetHex()" src/rpc/methods_mempool*.cpp
# Should find conversion points

# Verify no string txid passed to core
grep -r "getTransaction.*txid_hex" src/daemon/mempool.cpp
# Should return ZERO matches
```

**Step 8 (OutPoint Unification):**
```bash
# Only one OutPoint definition
grep -r "struct OutPoint" include/ src/
# Should return ONLY include/consensus/outpoint.h
```

### Integration Test

**Create test_m0_type_hygiene.cpp:**

```cpp
#include "consensus/outpoint.h"
#include "daemon/tx_mempool.h"
#include "mempool/mempool.h"
#include <cassert>
#include <unordered_set>

void test_outpoint_canonical() {
    uint256 hash1 = uint256::FromHex("abc123...");
    OutPoint op1{hash1, 0};
    OutPoint op2{hash1, 0};

    // Equality
    assert(op1 == op2);

    // Hashable
    std::unordered_set<OutPoint> set;
    set.insert(op1);
    assert(set.count(op2) == 1);

    // ToString/FromString round trip
    std::string str = op1.ToString();
    OutPoint op3 = OutPoint::FromString(str);
    assert(op1 == op3);
}

void test_mempool_entry_types() {
    Transaction tx;
    TxMempoolEntry entry1(tx, 1000, 12345);

    // txid is uint256
    static_assert(std::is_same<decltype(entry1.txid), uint256>::value,
                  "TxMempoolEntry::txid must be uint256");

    dinero::mempool::MempoolEntry entry2;
    static_assert(std::is_same<decltype(entry2.txid), uint256>::value,
                  "MempoolEntry::txid must be uint256");
}

int main() {
    test_outpoint_canonical();
    test_mempool_entry_types();
    return 0;
}
```

**Run:**
```bash
g++ -std=c++17 -I include/ test_m0_type_hygiene.cpp -o test_m0
./test_m0
echo "M.0 Type Hygiene: PASS"
```

---

## 8. Commit Strategy

### M0.1 - Canonical OutPoint
```bash
git add include/consensus/outpoint.h
git commit -m "feat(mempool): add canonical OutPoint with uint256 identity

Phase M.0 Step 1: Single source of truth for (txid, vout)

- txid is uint256 (not string)
- Hashable for unordered containers
- RPC conversion at boundary only (ToString/FromString)
- Foundation for mempool type hygiene

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

### M0.2 - TxMempoolEntry Migration
```bash
git add include/daemon/tx_mempool.h src/daemon/tx_mempool.cpp
git commit -m "refactor(mempool): migrate TxMempoolEntry to uint256 txid

Phase M.0 Step 2: Replace string txid with uint256

- TxMempoolEntry::txid is now uint256
- GetTxIdHex() helper for RPC boundary
- Orphan pool uses uint256 keys

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

### M0.3 - MempoolEntry Migration
```bash
git add include/mempool/mempool.h src/mempool/mempool.cpp
git commit -m "refactor(mempool): migrate MempoolEntry to uint256 txid

Phase M.0 Step 3: Policy layer uses uint256

- MempoolEntry::txid is now uint256
- Ancestry tracking uses uint256
- Parent/child sets use uint256

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

### M0.4 - Conflict Tracking
```bash
git add src/daemon/mempool.cpp
git commit -m "refactor(mempool): replace string-based conflict tracking with OutPoint

Phase M.0 Step 4: Structural identity for spent outputs

Before: string concat (txid + ':' + vout)
After: OutPoint (uint256, uint32_t)

Benefits:
- Type safety (no accidental string mixing)
- Performance (integer equality vs string comparison)
- Correctness (no formatting bugs)

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

### M0.5 - RPC Boundary
```bash
git add src/rpc/methods_mempool*.cpp
git commit -m "refactor(rpc): enforce uint256 conversion at RPC boundary

Phase M.0 Step 7: String conversion ONLY at RPC layer

RPC input:  hex string -> uint256::FromHex() -> core
RPC output: uint256 -> GetHex() -> JSON string

Core logic never sees string txids.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

### M0.6 - Delete Duplicates
```bash
git add include/consensus/utxo_entry.h include/mempool/coins_view_mempool.h include/p2p/consensus_validator.h
git commit -m "refactor(mempool): unify to canonical OutPoint definition

Phase M.0 Step 8: Single source of truth

Deleted:
- consensus/utxo_entry.h OutPoint (string-based)
- mempool/coins_view_mempool.h OutPoint (string-based)
- p2p/consensus_validator.h OutPoint (Hash256-based)

Replaced with:
- consensus/outpoint.h OutPoint (uint256-based, canonical)

All includes updated.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

### M0.LOCK - Documentation
```bash
git add PHASE_M0_TYPE_HYGIENE_LOCK.md
git commit -m "docs(mempool): lock Phase M.0 type hygiene invariants

Phase M.0 Complete: Mempool uses uint256 identity

Invariants Locked:
- Single OutPoint definition (consensus/outpoint.h)
- All mempool txid storage is uint256
- RPC conversion at boundary only
- No string concatenation for identity
- No type adapters or compatibility shims

Prerequisites for Phase M.1 satisfied.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>"
```

---

## 9. Success Criteria (Phase M.0 Complete)

✅ Single `OutPoint` definition in `consensus/outpoint.h`
✅ All `TxMempoolEntry` use `uint256 txid`
✅ All `MempoolEntry` use `uint256 txid`
✅ Conflict tracking uses `std::unordered_set<OutPoint>`
✅ Orphan pool uses `std::unordered_map<uint256, ...>`
✅ Ancestry tracking uses `std::unordered_set<uint256>`
✅ RPC methods convert at boundary (FromHex/GetHex)
✅ Zero string concatenation for transaction identity
✅ All tests pass
✅ Lock document created

**Grep Verification (All Must Return Zero):**
```bash
grep -r "std::string txid" include/daemon/tx_mempool.h include/mempool/mempool.h
grep -r "std::unordered_set<std::string> m_spent" src/daemon/mempool.cpp
grep -r "txid.*:.*vout" src/daemon/mempool.cpp | grep -v ToString | grep -v FromString
grep -r "struct OutPoint" include/ src/ | grep -v "consensus/outpoint.h"
```

---

## 10. Estimated Time (Realistic)

- **Step 1** (Canonical OutPoint): 30 min
- **Step 2** (TxMempoolEntry): 60 min
- **Step 3** (MempoolEntry): 60 min
- **Step 4** (Conflict tracking): 90 min
- **Step 5** (Orphan pool): 30 min
- **Step 6** (Ancestry): 30 min
- **Step 7** (RPC boundary): 120 min (most files)
- **Step 8** (Delete duplicates): 30 min
- **Testing**: 60 min
- **Documentation**: 30 min

**Total: ~8-10 hours of focused execution**

---

## 11. Blockers and Risks

### Potential Blocker: Transaction Class Uses String Internally

**Check:**
```bash
grep -r "std::string.*txid\|std::string.*hash" include/wallet/transaction.h
```

**If Transaction::GetHash() returns string:**
- Must fix Transaction class first (out of scope for M.0)
- OR add wrapper method that computes uint256 on demand

**Resolution:** Verify Transaction class before starting M.0.

### Potential Blocker: uint256 Missing std::hash

**Check:**
```bash
grep -r "struct hash<.*uint256" include/primitives/uint256.h
```

**If missing:**
- Add to `primitives/uint256.h` first (Step 0)

**Resolution:** Add hash specialization as prerequisite.

---

## 12. Post-M.0 State

### What Changes
- Mempool uses `uint256` everywhere internally
- Single `OutPoint` definition across codebase
- RPC is the only string boundary
- No hidden type conversions

### What Stays The Same
- All algorithms (RBF, CPFP, eviction) unchanged
- All policies (limits, fees) unchanged
- Network protocol unchanged
- RPC interface unchanged (still accepts/returns hex strings)

### What M.1 Can Now Do
- Build relay logic on clean types
- Optimize lookups (uint256 comparison is faster)
- Add fee estimation without type confusion
- Integrate with consensus layer cleanly (same types)

---

## 13. Final Reminder

**This is mechanical work. No design decisions remain.**

If you encounter uncertainty:
1. Check this guide first
2. Check H.6 lock document
3. Ask, don't assume

The hard part (deciding to do M.0) is done. This is just connecting the right types correctly.

**Next session:**
1. Open this guide
2. Start at Step 1
3. Execute linearly
4. Commit at each step boundary
5. Stop when success criteria met

Phase M.1 cannot succeed without M.0. Do M.0 first.
