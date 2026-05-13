# TODO Audit - Week 6 Day 1

**Date**: 2025-11-06
**Status**: 114 TODOs identified across 49 files
**Priority**: Cleanup recommended for Week 7

---

## 📊 Summary

```
Total TODOs:           114
Files with TODOs:      49
Critical (blocking):   10
Stale includes:        13
Outdated (Week N):     2
Future features:       3
Other:                 86
```

---

## 🔴 Critical TODOs (Priority: P0 - Must Fix)

These block production functionality:

### 1. Mining Transaction Selection
**File**: `src/daemon/mining.cpp:688`
```cpp
// TODO: Implement mempool transaction selection and fee calculation
```
**Impact**: Mining only includes coinbase, no user transactions
**Priority**: 🔴 P0 (production blocker)

### 2. SetMiningAddress Implementation
**Files**:
- `src/daemon/mining.cpp:1253`
- `src/daemon/mining.cpp:1266`

```cpp
// TODO: Implement SetMiningAddress
```
**Impact**: Cannot dynamically change mining address
**Priority**: 🟡 P1 (nice to have)

### 3. Mempool Dependency Tracking
**File**: `src/daemon/mempool.cpp:334,601`
```cpp
// TODO: Check dependencies are satisfied
// TODO: Update dependency tracking when transactions are added/removed
```
**Impact**: Could accept invalid transaction chains
**Priority**: 🔴 P0 (consensus risk)

### 4. Blockchain Transaction Lookup
**File**: `src/daemon/mempool.cpp:518`
```cpp
// TODO: Add blockchain transaction lookup when hasTransaction method is implemented
```
**Impact**: Cannot properly validate mempool entries
**Priority**: 🟡 P1 (functionality gap)

### 5. Header Sync Heights
**Files**: `src/daemon/header_sync_manager.cpp:132,178`
```cpp
m_best_header_height = 0; // TODO: Get actual height from blockchain database
m_best_block_height = 0; // TODO: Get actual height from blockchain
```
**Impact**: Header sync always starts from 0
**Priority**: 🟡 P1 (efficiency issue)

### 6. UTXO Index Integration
**File**: `src/daemon/genesis_init.cpp:305`
```cpp
// TODO: Add to UTXOIndex when API is available
```
**Impact**: Genesis outputs not in UTXO set
**Priority**: 🟡 P1 (may already be handled elsewhere)

### 7. ARP Bridge Rate Averaging
**File**: `src/daemon/arp_manager.cpp:164`
```cpp
// TODO: Implement actual bridge rate averaging
```
**Impact**: ARP feature incomplete
**Priority**: 🟢 P2 (future feature)

---

## 🟡 Stale Includes (Priority: P1 - Should Clean)

These are commented-out includes with TODOs (13 occurrences):

### Examples:
```cpp
// #include "consensus/genesis_block.h" // TODO: Add genesis block support
// #include "consensus/premine_block.hpp"    // TODO: Add premine functionality
// #include "util/hex.h"               // TODO: Add hex conversion utilities
```

**Files**:
- `src/daemon/blockchain.cpp` (7 includes)
- `src/daemon/mining.cpp` (1 include)
- `src/daemon/gbt_work_manager.cpp` (2 includes)
- `src/daemon/founder_control.cpp` (1 include)
- Others

**Action**: Remove or uncomment these includes

---

## 🟢 Outdated Week-Specific TODOs (Priority: P1 - Should Update)

These reference old "Week N" goals:

### 1. RPC Context Wiring (DONE!)
**File**: `src/daemon/daemon_app.cpp:138`
```cpp
// TODO Week 2: Wire RPC context for context-aware handlers
```
**Status**: ✅ Completed in Week 6
**Action**: Remove or update to "DONE"

### 2. Re-enable Optional Services
**File**: `src/daemon/daemon_app.cpp:78`
```cpp
// TODO: Re-enable optional services once they're needed
```
**Status**: Unclear if needed
**Action**: Evaluate and remove/implement

---

## 🔵 Future Features (Priority: P2 - Non-Blocking)

These are enhancements for future versions:

### 1. WebSocket Server
**File**: `src/daemon/ws_stubs.cpp:3`
```cpp
// TODO: Replace with full WebSocket server implementation
```
**Status**: Stubs sufficient for now
**Priority**: 🔵 P2 (v1.1 feature)

### 2. Thread-Safe Database
**File**: `src/daemon/db_meta_utils.cpp:62`
```cpp
// Global database handle (TODO: Make this thread-safe)
```
**Status**: Works but could be improved
**Priority**: 🔵 P2 (optimization)

### 3. Enhanced Security
**File**: `src/daemon/founder_control.cpp:186`
```cpp
// TODO: Store and validate public key for enhanced security
```
**Status**: Current security sufficient
**Priority**: 🔵 P2 (hardening)

---

## 📋 TODO Cleanup Plan

### Phase 1: Critical Fixes (Week 7 Day 1-2)

**Priority**: Fix production blockers

1. **Mempool Transaction Selection** (mining.cpp:688)
   - Implement `SelectTransactions()` properly
   - Add fee calculation
   - Test with multiple transactions

2. **Mempool Dependency Tracking** (mempool.cpp:334,601)
   - Implement dependency graph
   - Validate transaction chains
   - Add tests for edge cases

3. **Blockchain Transaction Lookup** (mempool.cpp:518)
   - Implement `hasTransaction()` method
   - Wire into mempool validation
   - Add integration test

**ETA**: 4-6 hours

---

### Phase 2: Code Cleanup (Week 7 Day 3)

**Priority**: Remove stale code

1. **Remove Stale Includes** (13 files)
   - Delete commented-out includes
   - Or uncomment and use them
   - Update TODOs to be specific

2. **Update Week-Specific TODOs** (2 files)
   - Mark completed ones as DONE
   - Remove obsolete ones
   - Update references to current week

3. **Consolidate Similar TODOs**
   - Many TODOs are duplicates
   - Group related ones
   - Create single tracking issues

**ETA**: 2-3 hours

---

### Phase 3: Feature Completion (Week 7-8)

**Priority**: Fill functionality gaps

1. **SetMiningAddress Dynamic Updates**
   - Implement RPC method
   - Wire into mining service
   - Add test coverage

2. **Header Sync Height Tracking**
   - Get actual heights from blockchain
   - Remove hardcoded 0 values
   - Test sync from various heights

3. **UTXO Index Integration**
   - Verify genesis outputs in UTXO set
   - Add integration test
   - Document expected behavior

**ETA**: 6-8 hours

---

### Phase 4: Future Enhancements (Week 8+)

**Priority**: Nice to have

1. **WebSocket Server** (replace stubs)
2. **Thread-Safe Database** (remove global)
3. **ARP Bridge Rate Averaging** (complete feature)
4. **Enhanced Security** (founder control hardening)

**ETA**: 1-2 weeks

---

## 🎯 Recommended Actions

### Immediate (Next Session)

1. **Create GitHub Issues** for each critical TODO
2. **Label Issues**: `P0-critical`, `P1-should-fix`, `P2-future`
3. **Assign to Milestones**: Week 7, Week 8, v1.1, etc.

### Week 7 Focus

- Fix 3 critical TODOs (mempool + mining)
- Clean 15 stale includes/comments
- Update 2 outdated Week references

### Week 8 Focus

- Complete remaining P1 TODOs
- Add comprehensive tests
- Document all deferred features

---

## 📊 By the Numbers

### Before Cleanup (Current)
```
Total TODOs:     114
Critical:        10
Stale/Outdated:  15
Clean Files:     ~50% (51/100 files have TODOs)
```

### After Week 7 Cleanup (Target)
```
Total TODOs:     <50
Critical:        0
Stale/Outdated:  0
Clean Files:     >80% (80/100 files clean)
```

---

## 🚨 Production Readiness Impact

**Current Status**: 🟡 **Mostly Production-Ready**

**Blockers**:
1. Mempool transaction selection (mining only includes coinbase)
2. Mempool dependency validation (consensus risk)
3. Blockchain transaction lookup (validation gap)

**Non-Blockers**:
- Stale includes (cosmetic)
- Outdated comments (documentation)
- Future features (deferred to v1.1)

**Recommendation**: Fix 3 critical TODOs before mainnet launch, rest can be cleaned up in Week 7.

---

## 🔍 Detailed TODO List by File

### src/daemon/mining.cpp (4 TODOs)
```
Line 15:   // #include TODO
Line 688:  // TODO: Implement mempool transaction selection (CRITICAL)
Line 1253: // TODO: Implement SetMiningAddress
Line 1266: // TODO: Implement SetMiningAddress
```

### src/daemon/mempool.cpp (3 TODOs)
```
Line 334: // TODO: Check dependencies are satisfied (CRITICAL)
Line 518: // TODO: Add blockchain transaction lookup (CRITICAL)
Line 601: // TODO: Update dependency tracking
```

### src/daemon/blockchain.cpp (14 TODOs)
```
Lines 3-24:   // Multiple commented includes
Lines 147-162: // Premine implementation TODOs
Line 288:     // Premine values TODO
Line 319:     // chainfacts constant TODO
```

### src/daemon/header_sync_manager.cpp (4 TODOs)
```
Line 132: // TODO: Get actual height from blockchain
Line 178: // TODO: Get actual height from blockchain
Line 274: // TODO: Get real block hash
Line 363: // TODO: Implement proper height validation
```

### Others (89 TODOs)
- Various minor TODOs across 45+ files
- Most are non-critical or cosmetic
- Many can be resolved with simple code cleanup

---

## ✅ Success Criteria

Week 7 TODO cleanup is complete when:

1. ✅ All 10 critical TODOs are fixed or documented as non-blocking
2. ✅ All 13 stale includes are removed
3. ✅ All 2 outdated Week references are updated
4. ✅ Total TODO count < 50
5. ✅ No TODO appears in production-critical code paths

---

## 📚 References

- **TODO Tracking Issue**: TBD (create in GitHub)
- **Cleanup Branch**: `feature/todo-cleanup-week7`
- **Test Coverage**: Ensure all TODO fixes have tests

---

*"TODOs are technical debt. Pay it down regularly or interest compounds."*
— Week 7 Cleanup Principle
