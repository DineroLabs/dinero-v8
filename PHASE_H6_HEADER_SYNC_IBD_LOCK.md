# 🔒 Phase H.6 — Header Sync + IBD LOCKED

**Date:** December 18, 2025
**Status:** COMPLETE AND FROZEN
**Version:** v0.15.0.4

---

## 📜 PHASE DECLARATION

**Phase H.6 (Header-First Sync + Initial Block Download) is COMPLETE.**

The following systems are production-ready and **architecturally frozen**:
- Header-first synchronization with orphan queue
- Block index management with uint256 type hygiene
- Chainwork-based fork choice (ByWorkThenHash)
- Candidate tip tracking with deterministic tie-breaking
- Restart-safe chain state persistence

**This document locks the architectural contracts and invariants.**

---

## ✅ COMPLETED SYSTEMS

### 1. Header-First Synchronization
**Files:**
- `include/consensus/block_index.h` (lines 168-183)
- `src/consensus/block_index.cpp` (lines 95-187)

**Canonical Functions:**
```cpp
bool MaybeQueueOrphan(CBlockIndex* block_index);
void ProcessOrphanQueue(const uint256& parent_hash);
void OnParentValidated(CBlockIndex* parent_index);
bool CanConnect(CBlockIndex* block_index);
```

**Invariants:**
1. Headers can arrive out of order
2. Orphaned headers queued by parent hash (g_orphan_pool)
3. When parent validated → orphans processed recursively
4. No orphan leaks (all eventually connected or discarded)

**Test:** `tests/p2p/test_header_sync_restart.cpp`

---

### 2. Block Index Type Hygiene (uint256)
**Date Completed:** December 18, 2025

**Architectural Change:**
```cpp
// BEFORE (inconsistent):
using uint256 = std::string;  // Bug waiting to happen

// AFTER (canonical):
class uint256 {
    uint8_t data[32];
    // ... proper 256-bit hash type
};
```

**Critical Files:**
- `include/primitives/uint256.h` — Canonical uint256 definition
- `include/primitives/uint256.h` (lines 152-165) — `std::hash<uint256>` specialization
- `include/consensus/block_index.h` (lines 57-59) — CBlockIndex hash fields

**Unified Fields:**
```cpp
class CBlockIndex {
    uint256 hash;           // This block's hash (was: std::string)
    uint256 prev_hash;      // Previous block hash (was: std::string)
    uint256 merkle_root;    // Merkle root (was: std::string)
};
```

**Global Maps:**
```cpp
// All use uint256 keys (was: std::string):
std::unordered_map<uint256, std::unique_ptr<CBlockIndex>> g_block_index;
std::unordered_map<uint256, std::vector<CBlockIndex*>> g_orphan_pool;
```

**Boundary Pattern (RPC/API):**
```cpp
// At entry: string → uint256
uint256 hash = uint256::FromHexUnsafe(block_hash_hex);

// Internal: pure uint256
CBlockIndex* pindex = FindBlockIndex(hash);

// At exit: uint256 → string
return pindex->hash.GetHex();
```

**Invariants:**
1. All internal block identifiers are uint256 (never std::string)
2. String conversions ONLY at RPC/API boundaries
3. Logging uses `.GetHex()` for display
4. No type aliases that weaken uint256 back to string

---

### 3. Fork Choice (Chainwork + Deterministic Tie-Breaking)
**File:** `include/consensus/block_index.h` (lines 131-154)

**Consensus Rule (v0.15.0.4):**
```cpp
struct ByWorkThenHash {
    bool operator()(const CBlockIndex* a, const CBlockIndex* b) const {
        // Primary: Highest chainwork wins
        int work_cmp = chainwork::CompareWork(a->chainwork, b->chainwork);
        if (work_cmp != 0) return work_cmp > 0;

        // CONSENSUS TIE-BREAKING: Lowest block hash wins
        return a->GetBlockHash() < b->GetBlockHash();
    }
};
```

**Why This Matters:**
- Prevents network splits under equal-work scenarios
- Guarantees deterministic fork resolution across all nodes
- No timing dependencies (hash comparison is pure data)

**Invariants:**
1. Chainwork is primary ordering (most work wins)
2. Hash tie-breaking is secondary (lexicographically smallest)
3. Candidate tips stored in: `std::set<CBlockIndex*, ByWorkThenHash> g_candidates`
4. GetBestCandidate() always deterministic

---

### 4. Chainwork Calculation (Bitcoin-Compatible)
**Files:**
- `include/consensus/block_index.h` (lines 98-128)
- `src/consensus/chainwork.cpp`

**Functions:**
```cpp
std::string WorkForBits(uint32_t bits);           // Work = 2^256 / (target + 1)
std::string AddWork(const std::string& a, const std::string& b);
int CompareWork(const std::string& a, const std::string& b);
std::string BitsToTarget(uint32_t bits);
std::string TargetToWork(const std::string& target_hex);
```

**Invariants:**
1. Work stored as hex string (256-bit arbitrary precision)
2. Chainwork is cumulative (genesis to current block)
3. Higher chainwork = more valid chain
4. Work calculation must match Bitcoin semantics

---

### 5. Restart Safety (Chain State Persistence)
**Files:**
- `src/storage/chain_db.cpp` — TipInfo serialization
- `include/storage/tip_info.h` — Chain tip metadata

**Restart Invariant:**
```cpp
struct TipInfo {
    uint256 hash;           // Tip block hash (32 raw bytes)
    int height;             // Block height
    arith_uint256 work;     // Cumulative chainwork
    uint32_t timestamp;     // Block timestamp
};
```

**Serialization (Binary, 32-byte hashes):**
```cpp
// Write:
out.append(reinterpret_cast<const char*>(tip.hash.data), 32);

// Read:
std::memcpy(tip.hash.data, data.data() + offset, 32);
```

**Invariants:**
1. TipInfo persisted to ChainDB on every tip change
2. On restart: chain reconstructed from TipInfo + block index
3. No string conversion in serialization (raw 32 bytes)
4. active_tip_ restored correctly after crash

**Test:** `tests/p2p/test_header_sync_restart.cpp`

---

## 🚫 FORBIDDEN MODIFICATIONS

### DO NOT:
1. **Change uint256 back to std::string**
   - This creates type inconsistency bugs
   - All hash types must remain uint256

2. **Modify ByWorkThenHash comparator**
   - Consensus-critical tie-breaking rule
   - Any change = potential network split

3. **Change orphan queue semantics**
   - Headers must be queueable out-of-order
   - Recursive processing must complete before returning

4. **Weaken type boundaries**
   - No adapters that convert uint256 → string internally
   - String conversions ONLY at RPC/API layer

5. **Skip restart safety**
   - TipInfo must always be persisted
   - active_tip_ must always be restorable

---

## ✅ SAFE TO MODIFY

These areas are **non-consensus** and can evolve:

- **Performance optimizations** (as long as results identical)
  - Orphan queue data structure (current: `unordered_map<uint256, vector<CBlockIndex*>>`)
  - Chainwork comparison algorithm (current: string-based, could be uint256-based)

- **Logging and metrics**
  - Header sync progress reporting
  - Orphan queue size monitoring

- **RPC interface** (as long as internal types unchanged)
  - Additional getblockheader fields
  - New header sync status endpoints

- **Testing** (add more, never remove)
  - Additional restart scenarios
  - Stress tests for orphan queue
  - Reorg depth testing

---

## 🔐 VERIFICATION INVARIANTS

**These MUST always be true:**

### 1. Type Hygiene
```bash
# All CBlockIndex hash fields are uint256 (never std::string)
grep "std::string hash" include/consensus/block_index.h
# Should return: NO MATCHES

# Global maps use uint256 keys
grep "std::unordered_map<uint256" include/consensus/block_index.h
# Should match: g_block_index, g_orphan_pool declarations
```

### 2. Orphan Queue Correctness
```cpp
// After header validation:
assert(g_orphan_pool[missing_parent_hash].contains(orphan_index));

// After parent arrives:
ProcessOrphanQueue(parent_hash);
assert(g_orphan_pool[parent_hash].empty());  // All processed
```

### 3. Deterministic Fork Choice
```cpp
CBlockIndex* tip1 = GetBestCandidate();
CBlockIndex* tip2 = GetBestCandidate();
assert(tip1 == tip2);  // Same call = same result (no randomness)
```

### 4. Restart Safety
```bash
# Before restart:
./dinero-cli getblockcount
# Returns: N

# Kill daemon, restart:
./dinero-cli getblockcount
# Must return: N (same height, tip restored)
```

---

## 📊 PHASE METRICS

**Code Changed:**
- 12+ files modified
- ~70 mechanical conversions (string → uint256)
- 0 compilation errors after unification

**Tests Added/Modified:**
- `tests/consensus/test_prune_eligibility.cpp` — Refactored to unit tests (no ChainManager)
- `tests/p2p/test_header_sync_restart.cpp` — Restart safety validation

**Type Safety:**
- `std::hash<uint256>` specialization added
- All `FindBlockIndex()` calls use uint256
- No temporary type conversions or adapters

---

## 🔄 INTEGRATION WITH OTHER PHASES

### Dependencies (Complete):
- **Consensus Locked** — Genesis hash, PoW, block validation
- **ChainDB** — Persistent storage for TipInfo and block metadata
- **Primitives** — uint256 canonical type, BlockHeader structure

### Dependent Phases (In Progress):
- **Phase P.1 (Prune Eligibility)** — Uses CBlockIndex flags, defers ChainManager integration
- **Phase M.1 (Mempool)** — Will use FindBlockIndex(uint256) for tx validation
- **Phase B.1 (UTXO Set)** — Already implemented, will integrate with block application

### Deferred (Future):
- **ChainManager Integration Tests** — Require Phase M.1 (Mempool) completion
- **Full IBD Performance Optimization** — Current focus: correctness over speed

---

## 📝 ARCHITECTURAL DECISIONS LOCKED

### 1. Header-First is Canonical
- Block headers downloaded first (fast, low bandwidth)
- Block bodies downloaded second (slow, high bandwidth)
- This matches Bitcoin Core's design for scalability

### 2. uint256 is Single Source of Truth
- No string aliases for hash types
- All conversions at RPC boundary only
- Prevents type confusion bugs at compile time

### 3. Deterministic Fork Choice is Required
- Equal chainwork must resolve deterministically
- Hash comparison prevents network splits
- No node-specific preferences or timing

### 4. Restart Must Be Transparent
- User should never notice crashes (within reason)
- Chain tip always recoverable from ChainDB
- No manual reindex required for clean shutdown

---

## 🎯 SUCCESS CRITERIA (MET)

- ✅ Headers can sync independently of blocks
- ✅ Orphan headers queued and processed correctly
- ✅ Fork choice is deterministic across all nodes
- ✅ Restart recovers chain state without user intervention
- ✅ Type system prevents hash confusion bugs
- ✅ Tests validate restart safety and orphan handling
- ✅ Zero compilation errors after uint256 unification

---

## 🚀 NEXT PHASES

**Not Part of H.6 (Do Not Add to This Phase):**

1. **Phase M.1 — Mempool (Full Implementation)**
   - Consensus-correct transaction validation
   - UTXO spend checks, double-spend prevention
   - Reorg reconciliation (remove conflicted, reinsert orphaned)
   - **Estimated effort:** Weeks, not hours
   - **Blocker for:** ChainManager integration tests

2. **Phase P.1 — Prune Eligibility (Lock Semantics)**
   - Choose: depth-based (288 blocks) vs UTXO-driven
   - Implement UpdatePruneEligibility in ChainManager
   - **Requires:** Phase M.1 for integration tests

3. **Phase P.2 — Block Pruning (Execution)**
   - Delete eligible block/undo data from disk
   - Update BlockStorage references
   - **Requires:** Phase P.1 locked

---

## 🔒 FREEZE NOTICE

**Phase H.6 is LOCKED as of December 18, 2025.**

Any modifications to the systems described in this document require:
1. Explicit architectural review
2. Verification that invariants remain satisfied
3. Update to this lock document with rationale

**This is not open for negotiation. The architecture is frozen.**

---

## 📚 REFERENCES

- Bitcoin Core header-first sync: [bitcoin/bitcoin#4468](https://github.com/bitcoin/bitcoin/pull/4468)
- BIP113 (Median Time Past): For future locktime validation
- uint256 type design: Canonical 32-byte binary representation

---

**Document Owner:** DineroCoin Core Development
**Last Updated:** December 18, 2025
**Next Review:** Only if consensus rules change (hard fork)
