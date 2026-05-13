# Phase M.0 — uint256 Integrity Lock

**Status:** ⚠️ NEAR-COMPLETE (December 19, 2025)
**Compliance:** 99% (166/168 uses correct, 2 violations remaining)

**Rule:** "uint256 is identity, .GetHex() is presentation"

---

## 1. Invariant

**Core Principle:**
- `uint256` is the canonical representation of hashes in core logic
- `.GetHex()` is ONLY for presentation (logging, RPC output, serialization)
- No early downgrades: never store hex strings in variables unless at RPC/storage boundary

**Forbidden Patterns:**
```cpp
// ❌ WRONG: Early downgrade
std::string hash_hex = block_hash.GetHex();
if (hash_hex == something) { ... }  // Logic depends on string

// ❌ WRONG: String-based identity
std::string tx_id = txid.GetHex();
outpoint_map[tx_id] = data;  // Using hex for lookups

// ❌ WRONG: Round-trip conversion
std::string hex = hash.GetHex();
uint256 hash2 = uint256::FromHex(hex);  // Wasteful
```

**Correct Patterns:**
```cpp
// ✅ CORRECT: Keep as uint256, convert inline for presentation
const uint256& hash = block_hash;
logger->info("Hash: " + hash.GetHex());  // Inline conversion

// ✅ CORRECT: uint256-based identity
std::unordered_map<uint256, Data> outpoint_map;
outpoint_map[txid] = data;  // Direct uint256 key

// ✅ CORRECT: Presentation helpers for RPC
struct ReorgStats {
    uint256 old_tip;  // Identity
    uint256 new_tip;

    std::string GetOldTipHex() const { return old_tip.GetHex(); }  // RPC helper
};
```

---

## 2. Audit Results

### 2.1 Consensus Layer (src/consensus/, include/consensus/)

**Total `.GetHex()` uses:** 36
**Violations found:** 0
**Status:** ✅ CLEAN

**Allowed uses (10 non-logging):**
1. `chainwork.cpp:304` - ChainworkToHex() helper (storage/presentation function)
2. `header_sync_manager.cpp:383, 391` - P2P block download requests (network protocol boundary)
3. `chain_manager.cpp:671` - GetBlockHashByHeight() return (RPC boundary)
4. `chain_manager.h:77` - GetBestBlockHash() return (RPC boundary)
5. `chain_manager.h:132-134` - ReorgStats::GetOldTipHex() helpers (RPC presentation)
6. `header_sync_manager.h:250` - GetBestHeaderHash() return (RPC boundary)
7. `outpoint.h:49` - OutPoint::ToString() method (debugging/RPC only)

**Fixes applied:**
- ✅ `header_sync_manager.cpp:649` - Eliminated early downgrade in logging
- ✅ `chain_manager.h:127-129` - Changed ReorgStats fields from `std::string` to `uint256`
- ✅ `chain_manager.cpp:157-159` - Updated ReorgStats assignments to use uint256
- ✅ `chain_manager.cpp:486` - Fixed OutPoint construction (removed `.GetHex()`)

### 2.2 Daemon Layer (src/daemon/, include/daemon/)

**Last Audit:** December 19, 2025
**Total `.GetHex()` uses:** 168 (across all layers)
**Violations found:** 2
**Status:** ⚠️ NEAR-CLEAN (99% compliant)

**Remaining Violations:**
1. ⚠️ `chainstate_service.cpp:151` - Genesis hash string comparison
2. ⚠️ `chainstate_service.cpp:857` - Block locator using hex strings for identity

**Acceptable uses (presentation boundaries):**
1. `mempool.cpp:1738` - InventoryVector construction (P2P protocol message)
2. `mining_safety_gates.cpp:103` - CompareChainwork() call (inline conversion)
3. `chainstate_service.cpp:293, 350` - Inline logging/metrics (presentation)
4. `block_acceptor.cpp:644` - OUT parameter for FindParentBlock() (function boundary)
5. All RPC methods in `src/rpc/` - RPC boundary conversions (15 uses)
6. All logging statements - Inline .GetHex() for display (74 uses)

**Fixes applied:**
- ✅ `chainstate_service.cpp:289` - Inlined GetHex() into logging (removed early downgrade)
- ✅ `chainstate_service.cpp:344` - Inlined GetHex() into JSON metrics (removed early downgrade)
- ✅ `chainstate_service.cpp:856` - Eliminated temporary variable (call GetHex() twice)
- ✅ `mining_safety_gates.cpp:103` - Inlined GetHex() into CompareChainwork() call

### 2.3 Storage Layer (src/storage/, include/storage/)

**Total `.GetHex()` uses:** 25
**Violations found:** 0
**Status:** ✅ CLEAN

**Allowed uses (4 early downgrades):**
1. `chain_db.cpp:155` - RocksDB key construction: `"U:" + hash.GetHex()`
2. `chain_db.cpp:181` - RocksDB key construction for undo records
3. `chain_db.cpp:582` - RocksDB value serialization (block hash storage)
4. `chain_db.cpp:840` - UTXO key encoding (hex-to-bytes conversion)

**Rationale:** Storage layer is the boundary between in-memory uint256 and on-disk persistence. Hex serialization for RocksDB keys/values is explicitly allowed.

### 2.4 Legacy/Unused Code

**Block validation.h structs** (DEFERRED):
- Files: `include/consensus/block_validation.h` (legacy structs)
- Contains: UTXO, TxInput, UndoEntry with string-based txid fields
- Status: Appears unused by main codebase (consensus_validator.h is clean)
- Decision: Defer migration until confirmed these structs are active

---

## 3. Architectural Impact

### 3.1 ReorgStats Migration

**Before (Phase M.0 violation):**
```cpp
struct ReorgStats {
    std::string old_tip;
    std::string new_tip;
    std::string fork_point;
};

// Usage:
last_reorg_stats_.old_tip = active_tip_->hash.GetHex();  // Early downgrade
```

**After (M.0 compliant):**
```cpp
struct ReorgStats {
    uint256 old_tip;         // Phase M.0: uint256 identity, not string
    uint256 new_tip;
    uint256 fork_point;

    // Phase M.0: RPC presentation helpers
    std::string GetOldTipHex() const { return old_tip.GetHex(); }
    std::string GetNewTipHex() const { return new_tip.GetHex(); }
    std::string GetForkPointHex() const { return fork_point.GetHex(); }
};

// Usage:
last_reorg_stats_.old_tip = active_tip_->hash;  // Direct uint256 assignment

// RPC:
json["old_tip"] = stats.GetOldTipHex();  // Convert at RPC boundary
```

**Benefits:**
- Reorg stats can now be compared directly (`old_tip == new_tip`)
- No round-trip conversions (FromHex...GetHex)
- Consistent with outpoint.h pattern (uint256 identity + ToString() helper)

### 3.2 OutPoint Canonicalization

**Verified:** Single OutPoint definition in `include/consensus/outpoint.h`
```cpp
struct OutPoint {
    uint256 txid;    // NOT std::string
    uint32_t vout;

    OutPoint(const uint256& hash, uint32_t n);  // Constructor takes uint256
    std::string ToString() const;  // RPC/logging boundary ONLY
};
```

**Phase M.0 Type Hygiene Lock** (separate document) confirms:
- No duplicate OutPoint definitions
- All OutPoint instances use uint256 for txid
- ToString() method marked as RPC boundary only

---

## 4. Presentation Boundaries (Allowed Zones)

The following uses of `.GetHex()` are explicitly **ALLOWED**:

### 4.1 RPC Output
- Methods returning `std::string` for RPC (GetBestBlockHash, GetBlockHashByHeight)
- JSON serialization for RPC responses
- Helper methods ending in `...Hex()` (GetOldTipHex, GetNewTipHex)

### 4.2 Logging/Debugging
- Direct inline calls: `logger->info("Hash: " + hash.GetHex())`
- Substring for abbreviated display: `hash.GetHex().substr(0, 16)`
- ToString() methods for debugging output

### 4.3 P2P Protocol
- Block locator generation (GenerateBlockLocator returns `vector<string>`)
- Inventory messages (InventoryVector takes string for network serialization)
- GETHEADERS/GETDATA message construction

### 4.4 Storage Layer Internals
- RocksDB key construction: `"U:" + hash.GetHex()`
- Database value serialization (persisting hashes to disk)
- UTXO key encoding (hex-to-bytes for compact keys)

### 4.5 Config/Genesis Comparisons
- Comparing against genesis hash from config (string constant)
- Chainwork comparison with minimum chainwork param (string from config)

---

## 5. Code Review Guidelines

### 5.1 PR Review Checklist

When reviewing code that uses `.GetHex()`, verify:

**✅ ACCEPT if:**
- [ ] Inline in logging: `logger->info("Hash: " + hash.GetHex())`
- [ ] RPC method return: `std::string GetFoo() { return foo.GetHex(); }`
- [ ] P2P serialization: `vector.push_back(hash.GetHex())`
- [ ] Storage layer key: `db_key = "prefix:" + hash.GetHex()`
- [ ] Helper method named `...Hex()`: `std::string GetOldTipHex() const`

**❌ REJECT if:**
- [ ] Early downgrade: `std::string x = hash.GetHex(); use(x);`
- [ ] Logic depends on hex: `if (hash.GetHex() == other.GetHex())`
- [ ] Hex used for identity: `map[hash.GetHex()] = value`
- [ ] Round-trip: `auto hex = x.GetHex(); auto y = FromHex(hex);`

### 5.2 Common Violations

**Pattern 1: Early Downgrade**
```cpp
// ❌ WRONG
std::string best_hash = tip.hash.GetHex();
logger->info("Tip: " + best_hash);

// ✅ FIX
logger->info("Tip: " + tip.hash.GetHex());
```

**Pattern 2: String Comparison**
```cpp
// ❌ WRONG
if (db_hash.GetHex() != config_hash.GetHex()) { ... }

// ✅ FIX (if both are uint256)
if (db_hash != config_hash) { ... }

// ✅ FIX (if config_hash is string constant from config)
if (db_hash.GetHex() != config_hash) { ... }  // Inline, acceptable
```

**Pattern 3: Reused Hex**
```cpp
// ❌ WRONG
std::string genesis_hex = genesis.GetHex();
if (std::find(locator.begin(), locator.end(), genesis_hex) == locator.end()) {
    locator.push_back(genesis_hex);
}

// ✅ FIX (M.0 prefers calling GetHex() twice over caching)
if (std::find(locator.begin(), locator.end(), genesis.GetHex()) == locator.end()) {
    locator.push_back(genesis.GetHex());
}
```

---

## 6. Testing Strategy

### 6.1 Compile-Time Enforcement

**Guardrails:**
- OutPoint constructor only accepts `const uint256&` (no string overload)
- ReorgStats fields are uint256 (no implicit string conversion)
- unordered_map<uint256, T> requires std::hash<uint256> specialization

**What this catches:**
```cpp
OutPoint op(txid.GetHex(), 0);  // ❌ Compile error (no string constructor)
OutPoint op(txid, 0);            // ✅ Compiles
```

### 6.2 Runtime Testing

**Key test cases:**
1. ReorgStats serialization (RPC should call GetOldTipHex())
2. Block locator generation (returns vector<string> for P2P)
3. UTXO persistence (ChainDB uses hex keys internally)

**Regression tests:**
- Verify no round-trip conversions (FromHex...GetHex)
- Confirm RPC responses use helper methods (...Hex())
- Check outpoint.h usage (no ToString() in core logic)

---

## 7. Future Work (Out of Scope for M.0)

### 7.1 Deferred Cleanups

**block_validation.h legacy structs:**
- Contains UTXO, TxInput, UndoEntry with string txid fields
- Appears unused by main consensus code
- **Action:** Confirm usage, then migrate or delete

**BlockAcceptor::FindParentBlock signature:**
- Currently returns chainwork as `std::string& parentChainwork` OUT param
- Could be refactored to return `arith_uint256`
- **Action:** Consider in future BlockAcceptor refactor

### 7.2 Potential Optimizations

**CompareChainwork function:**
- Currently takes `(string, string)` and converts to arith_uint256 internally
- Could be overloaded to accept `(arith_uint256, arith_uint256)` directly
- **Action:** Low priority (string version needed for config params)

---

## 8. Audit Command Reference

### 8.1 Find Early Downgrades
```bash
grep -rn "std::string.*=.*\.GetHex()" \
  --include="*.cpp" --include="*.h" \
  src/consensus/ include/consensus/ \
  src/daemon/ include/daemon/ \
  | grep -v "logger" | grep -v "Phase M.0"
```

**Expected:** Only storage layer (chain_db.cpp) hits

### 8.2 Find GetHex in Core Logic
```bash
grep -rn "\.GetHex()" \
  --include="*.cpp" --include="*.h" \
  src/consensus/ include/consensus/ \
  | grep -v "logger" | grep -v "LOG_" \
  | grep -v "// Phase M.0" | grep -v "\.GetHex().substr"
```

**Expected:** Only RPC boundaries, ToString() methods, P2P serialization

### 8.3 Verify ReorgStats
```bash
grep -A5 "struct ReorgStats" include/consensus/chain_manager.h
```

**Expected:**
```cpp
struct ReorgStats {
    uint256 old_tip;
    uint256 new_tip;
    uint256 fork_point;
    // ... helpers
};
```

### 8.4 Verify OutPoint Constructor
```bash
grep -A2 "OutPoint(" include/consensus/outpoint.h
```

**Expected:**
```cpp
OutPoint(const uint256& hash, uint32_t n) : txid(hash), vout(n) {}
```

---

## 9. Lock Commitment

**This phase is LOCKED as of December 19, 2025.**

**Commits:**
- header_sync_manager.cpp:649 early downgrade fix
- ReorgStats migration (chain_manager.h/cpp)
- chainstate_service.cpp cleanups (lines 289, 344, 856)
- mining_safety_gates.cpp:103 inline conversion
- chain_manager.cpp:486 OutPoint construction fix

**Verification:**
```bash
# Zero early downgrades in consensus/daemon layers:
grep -rn "std::string.*=.*\.GetHex()" src/consensus/ src/daemon/ \
  | grep -v logger | grep -v "Phase M.0"
# (Expected: no output)

# ReorgStats uses uint256:
grep "uint256.*tip\|uint256.*fork" include/consensus/chain_manager.h
# (Expected: 3 matches - old_tip, new_tip, fork_point)

# OutPoint takes uint256:
grep "OutPoint(const uint256" include/consensus/outpoint.h
# (Expected: 1 match - constructor signature)
```

**Enforcement:**
- Code reviews MUST reject new early downgrades
- ReorgStats fields MUST remain uint256 (no string fields)
- OutPoint constructor MUST NOT accept string overload

---

## 10. Related Documentation

- **PHASE_M0_TYPE_HYGIENE_LOCK.md** - OutPoint canonicalization, type unification
- **PHASE_H6_UINT256_REFACTOR_LOCK.md** - uint256 implementation (FromHex, GetHex, operators)
- **include/consensus/outpoint.h** - Canonical OutPoint definition

---

## 11. Comprehensive Audit Results (December 19, 2025)

### 11.1 Summary Statistics

| Metric | Count | Status |
|--------|-------|--------|
| **Total .GetHex() calls** | 168 | - |
| **🔴 Critical violations (consensus)** | 0 | ✅ CLEAN |
| **🟡 Medium violations (daemon)** | 2 | ⚠️ Needs fix |
| **✅ Acceptable uses** | 166 | ✅ Compliant |

### 11.2 Breakdown by Category

**Acceptable Uses (166):**
- Logging statements: 74 uses
- RPC boundaries: 15 uses
- Storage layer keys: 22 uses
- Helper methods (GetTxIdHex, etc.): 5 uses
- Documentation/examples/tests: 50 uses

**Violations (2):**
1. `src/daemon/services/chainstate_service.cpp:151` - Genesis hash string comparison
2. `src/daemon/services/chainstate_service.cpp:857` - Block locator std::find with hex strings

### 11.3 Violation Details

#### Violation #1: Genesis Hash String Comparison
**Location:** `src/daemon/services/chainstate_service.cpp:151`

**Current code:**
```cpp
const uint256& db_genesis_hash = db_genesis_result.value();
std::string expected_genesis_hash = params.genesis_hash;
if (db_genesis_hash.GetHex() != expected_genesis_hash) {
    logger_->error("FATAL: GENESIS MISMATCH");
    // ...
}
```

**Issue:** Comparing uint256 (via .GetHex()) against string parameter

**Fix:**
```cpp
const uint256& db_genesis_hash = db_genesis_result.value();
uint256 expected_genesis_hash = uint256::FromHex(params.genesis_hash);
if (db_genesis_hash != expected_genesis_hash) {  // Phase M.0: Direct uint256 comparison
    logger_->error("FATAL: GENESIS MISMATCH");
    // ...
}
```

#### Violation #2: Block Locator Using Hex Strings
**Location:** `src/daemon/services/chainstate_service.cpp:857-858`

**Current code:**
```cpp
std::vector<std::string> locator;  // Stores hex strings
// ...
if (std::find(locator.begin(), locator.end(), genesis_hash.GetHex()) == locator.end()) {
    locator.push_back(genesis_hash.GetHex());
}
```

**Issue:** Using hex strings for identity in container

**Fix:**
```cpp
std::vector<uint256> locator;  // Phase M.0: Store uint256, not hex strings
// ...
if (std::find(locator.begin(), locator.end(), genesis_hash) == locator.end()) {
    locator.push_back(genesis_hash);
}
// Convert to hex only at P2P/RPC boundary when serializing
```

### 11.4 Audit Commands

**Scan for violations:**
```bash
# String comparisons (should return 0 for consensus, 2 for daemon)
grep -rn "\.GetHex()" src/consensus/ src/daemon/ | grep -E "(==|!=)" | grep -v "Phase M.0"

# Early downgrades (should return 0)
grep -rn "std::string.*=.*\.GetHex()" src/consensus/ src/daemon/ | \
  grep -v "logger\|MPLOG\|g_logger\|substr\|Phase M.0"

# Count acceptable logging uses
grep -rn "\.GetHex()" src/ include/ | grep -E "logger|MPLOG|g_logger|std::cout" | wc -l
```

**Verify compliance:**
```bash
# Run comprehensive audit script
cd ~/Documents/DineroCoin
./audit_gethex_violations.sh
```

### 11.5 Next Steps

**Priority 1: Fix remaining violations**
- [ ] Fix chainstate_service.cpp:151 (genesis comparison)
- [ ] Fix chainstate_service.cpp:857 (locator vector type)
- [ ] Test changes with existing test suite
- [ ] Re-run audit to verify 0 violations

**Priority 2: Documentation cleanup**
- [ ] Add "Phase M.0: RPC boundary only" comments to unmarked uses
- [ ] Update this document with final clean state
- [ ] Add pre-commit hook to prevent new violations

**Priority 3: Enforcement**
- [ ] Add static analysis check for .GetHex() patterns
- [ ] Enforce in code review checklist
- [ ] Document in contributor guidelines

### 11.6 Compliance Score

**Current state: 99% compliant (166/168 uses are correct)**

**Target: M.0-Clean (100% compliant)**
- Estimated effort: 2 small fixes
- Risk: Very low (localized changes)
- Impact: Full Phase M.0 compliance

---

**Phase M.0 status: ⚠️ NEAR-COMPLETE (99% done, 2 violations remaining)**
