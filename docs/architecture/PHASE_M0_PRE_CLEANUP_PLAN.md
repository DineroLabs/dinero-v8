# Phase M.0-pre: txid Type Cleanup Plan

**Date**: 2025-12-26
**Goal**: Eliminate all `std::string txid` from core code (consensus/mempool)
**Scope**: 34 violations detected by mechanical gate
**Estimated effort**: 3-4 hours

---

## Violations by Category

### Consensus Layer (9 violations)

**Files**:
- `include/consensus/undo.h` (2) - SpentCoin/CreatedOut constructors
- `include/consensus/validation_worker_pool.h` (1) - worker task parameters
- `include/consensus/coins_db.h` (2) - legacy string methods
- `include/consensus/utreexo_accumulator.h` (1) - HashUTXO function
- `src/consensus/block_validation.cpp` (1) - CalculateTxId usage
- `src/consensus/coins_view_cache.cpp` (2) - legacy method implementations
- `src/consensus/validation_worker_pool.cpp` (1) - worker task implementation
- `src/consensus/utreexo_accumulator.cpp` (1) - HashUTXO implementation

**Impact**: Low (these are mostly legacy/unused paths)

### Mempool Layer (25 violations)

**Files**:
- `include/mempool/mempool.h` (comments only - already fixed)
- `include/mempool/invalid_tx_cache.h` (1) - cache lookup return type
- `include/mempool/covenant_policy.h` (3) - covenant tracking methods
- `src/mempool/mempool.cpp` (16) - extensive string-based tracking
- `src/mempool/invalid_tx_cache.cpp` (1) - cache implementation
- `src/mempool/covenant_policy.cpp` (4) - covenant tracker implementation

**Impact**: High (core mempool functionality)

---

## Migration Strategy

### Phase 1: Consensus Layer (Low Risk)
**Order**: Headers first, then implementations

1. `include/consensus/undo.h`
   - Change `SpentCoin(const std::string& txid_,...)` → `SpentCoin(const uint256& txid_,...)`
   - Change `CreatedOut(const std::string& txid_,...)` → `CreatedOut(const uint256& txid_,...)`

2. `include/consensus/utreexo_accumulator.h`
   - Change `HashUTXO(const std::string& txid,...)` → `HashUTXO(const uint256& txid,...)`

3. `include/consensus/validation_worker_pool.h`
   - Change task struct: `std::string txid` → `uint256 txid`

4. `include/consensus/coins_db.h`
   - Remove or migrate legacy string-based methods

5. Implementation files:
   - `src/consensus/utreexo_accumulator.cpp` - Update HashUTXO
   - `src/consensus/coins_view_cache.cpp` - Remove legacy methods
   - `src/consensus/validation_worker_pool.cpp` - Update task handling
   - `src/consensus/block_validation.cpp` - Fix CalculateTxId call site

### Phase 2: Mempool Layer (High Impact)
**Order**: Core data structures → indexes → helpers

1. **Core tracking** (`src/mempool/mempool.cpp`):
   - Internal storage: `std::unordered_map<std::string, ...>` → `std::unordered_map<uint256, ...>`
   - Method signatures: `contains(std::string)` → `contains(uint256)`
   - Local variables: `std::string txid = tx.GetTxid()` → `uint256 txid = tx.GetTxid()`

2. **Covenant policy** (`include/mempool/covenant_policy.h`):
   - `removeTransaction(const std::string&)` → `removeTransaction(const uint256&)`
   - `getConflictingTx(const std::string&,...)` → Returns uint256 not string
   - Internal: Outpoint tracking with uint256

3. **Invalid tx cache** (`include/mempool/invalid_tx_cache.h`):
   - `lookup(const uint256& txid,...)` already takes uint256 ✅
   - Return type: `std::optional<std::string>` → `std::optional<uint256>` (or keep string for reason?)
   - Actually, this returns the rejection REASON (string), not txid - FALSE POSITIVE

---

## Implementation Order

### Step 1: Consensus Headers (30 min)
- undo.h
- utreexo_accumulator.h
- validation_worker_pool.h
- coins_db.h (remove legacy methods if unused)

### Step 2: Consensus Implementations (30 min)
- utreexo_accumulator.cpp
- validation_worker_pool.cpp
- coins_view_cache.cpp (remove legacy)
- block_validation.cpp (fix call sites)

### Step 3: Mempool Headers (30 min)
- covenant_policy.h (method signatures)
- invalid_tx_cache.h (review - may be false positive)

### Step 4: Mempool Implementations (90 min)
- mempool.cpp - systematic migration:
  1. Change member variables (maps, sets)
  2. Update method signatures
  3. Fix local variables
  4. Update call sites
- covenant_policy.cpp
- invalid_tx_cache.cpp (if needed)

### Step 5: Build & Test (30 min)
- Build consensus library
- Build mempool components
- Run mechanical gate (should PASS)
- Update CI enforcement

---

## False Positives to Filter

Some detections are NOT actual violations:

1. **Comments** - "Phase M.0: Changed std::string txid to uint256" ✅ Already filtered
2. **GetTxIdHex() method** - Converts uint256 → string for presentation ✅ Already filtered
3. **InvalidTxCache::lookup return** - Returns rejection REASON (string), not txid
   - Need to verify this is actually a string reason, not a string txid

---

## Key Patterns

### Pattern 1: Storage Migration
```cpp
// Before
std::unordered_map<std::string, std::shared_ptr<MempoolEntry>> entries_;

// After
std::unordered_map<uint256, std::shared_ptr<MempoolEntry>> entries_;
```

### Pattern 2: Method Signatures
```cpp
// Before
bool contains(const std::string& txid) const;

// After
bool contains(const uint256& txid) const;
```

### Pattern 3: Local Variables
```cpp
// Before
std::string txid = tx.GetTxid();

// After
uint256 txid = tx.GetTxid();
```

### Pattern 4: Outpoint Construction (WRONG)
```cpp
// WRONG - input.prevout.txid is already uint256
std::string parent_txid = input.prevout.txid;

// RIGHT - use directly
const uint256& parent_txid = input.prevout.txid;
// or just
input.prevout.txid  // use inline
```

---

## Risk Mitigation

1. **Build after each phase**: Catch errors early
2. **Consensus first**: Smaller scope, lower risk
3. **Mechanical gate**: Automated verification
4. **Incremental commits**: Easy rollback if needed

---

## Success Criteria

- ✅ All 34 violations fixed
- ✅ `bash scripts/check_txid_types.sh` returns PASS
- ✅ Consensus library builds
- ✅ Mempool components build
- ✅ Gate 2 flipped to ENFORCED in CI
- ✅ No std::string txid in consensus/mempool

---

**Ready to execute**: Start with Step 1 (Consensus Headers)
