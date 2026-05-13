# Phase 3 - Commit 1: UTXO Type Fix (Critical Blocker)

**Commit Title:** `lightning: Fix UTXO type mismatch - use gRPC proto types`
**Status:** Ready to Execute (Pre-Flight Complete)
**Estimated Time:** 2-3 hours
**Risk Level:** LOW (compilation fix, no runtime changes)

---

## Problem Statement

**Current Issue:** Lightning code fails to compile with `ENABLE_LIGHTNING=ON` due to:
```
error: no type named 'UTXO' in 'dinero::WalletManager'
```

**Root Cause:** Lightning expects `WalletManager::UTXO` type, which doesn't exist.

**Available Types:**
- `dinero::SigningUTXO` (primitives/transaction.h) - Minimal cryptographic primitive
- `dinero::WalletUTXO` (wallet/utxo_index.h) - Wallet database entry
- `dinerod::UTXO` (proto/dinerod.proto) - gRPC message type ✅ CORRECT

**Solution:** Use `dinerod::UTXO` (proto type) - enforces gRPC boundary

---

## Pre-Flight Checklist

**Before Starting:**
- [x] Phase 2 testing complete
- [x] Option A (compile-time Lightning) implemented
- [x] Symbol audit complete (`docs/PHASE3_SYMBOL_AUDIT.md`)
- [ ] Create branch: `phase3/commit1-utxo-type-fix`
- [ ] Backup current state: `git stash save "pre-commit1"`
- [ ] Verify clean build with `ENABLE_LIGHTNING=OFF` succeeds

---

## Files to Modify

### 1. `include/lightning/lightning_wallet.h`

**Current Code (BROKEN):**
```cpp
std::vector<WalletManager::UTXO> selectUTXOsForAmount(  // ❌ Type doesn't exist
    uint64_t amount,
    int min_confirmations = 1
);
```

**New Code (FIXED):**
```cpp
// Forward declare proto type
namespace dinerod {
    class UTXO;
}

std::vector<dinerod::UTXO> selectUTXOsForAmount(  // ✅ Use proto type
    uint64_t amount,
    int min_confirmations = 1
);
```

**Lines to Change:** ~15, 113, 168

---

### 2. `src/lightning/lightning_wallet.cpp`

**Current Code (BROKEN):**
```cpp
std::vector<UTXO> available_utxos = m_wallet->listUnspentUTXOs(MIN_CONFIRMATIONS);  // ❌
```

**New Code (FIXED):**
```cpp
#include "proto/dinerod.pb.h"  // ✅ Include proto definitions

std::vector<dinerod::UTXO> available_utxos = m_wallet->listUnspentUTXOs(MIN_CONFIRMATIONS);
```

**Lines to Change:** ~67, 146, 151, 156, 160, 168, 459

---

### 3. `src/lightning/channel_manager.cpp`

**Current Code (uses direct wallet access - will be removed in future commits):**
```cpp
auto utxos = m_daemon_ctx.wallet->get().listUnspentUTXOs(1, 9999999);  // ❌ Type unclear
```

**New Code (explicit proto type):**
```cpp
std::vector<dinerod::UTXO> utxos = m_daemon_ctx.wallet->get().listUnspentUTXOs(1, 9999999);
```

**Lines to Change:** ~153, 999

**Note:** These call sites will be replaced with `WalletClient` in future commits (already done in Phase 2 for some locations)

---

### 4. `include/lightning/wallet_client.h`

**Current Code:**
```cpp
std::vector<UTXO> listUnspentUTXOs(  // ❌ Ambiguous type
    int min_confirmations,
    int max_confirmations = 9999999
) const;
```

**New Code:**
```cpp
#include "proto/dinerod.pb.h"  // Forward declare or include proto

std::vector<dinerod::UTXO> listUnspentUTXOs(  // ✅ Explicit proto type
    int min_confirmations,
    int max_confirmations = 9999999
) const;
```

**Lines to Change:** ~30

---

### 5. `src/lightning/wallet_client.cpp`

**Current Code:**
```cpp
std::vector<UTXO> WalletClient::listUnspentUTXOs(  // ❌
    int min_confirmations,
    int max_confirmations
) const {
```

**New Code:**
```cpp
std::vector<dinerod::UTXO> WalletClient::listUnspentUTXOs(  // ✅
    int min_confirmations,
    int max_confirmations
) const {
```

**Lines to Change:** ~41

---

## Step-by-Step Execution Plan

### Step 1: Create Branch & Backup (2 minutes)
```bash
cd /Users/haydarevich/Documents/DineroCoin
git checkout -b phase3/commit1-utxo-type-fix
git stash save "pre-commit1-backup"
```

### Step 2: Verify Baseline (5 minutes)
```bash
# Ensure current state builds with Lightning disabled
rm -rf build
cmake -B build -DENABLE_LIGHTNING=OFF
cmake --build build --target dinero_core -j8
# Should succeed ✅

# Verify Lightning fails with known error
rm -rf build
cmake -B build -DENABLE_LIGHTNING=ON
cmake --build build --target dinero_lightning 2>&1 | grep "no type named 'UTXO'"
# Should show UTXO type error ✅
```

### Step 3: Add Proto Include (10 minutes)

**File 1: `include/lightning/lightning_wallet.h`**
```bash
# Add after existing includes (line ~10)
# Add: #include "proto/dinerod.pb.h"

# Change return types (lines 15, 113, 168)
# Before: std::vector<WalletManager::UTXO>
# After:  std::vector<dinerod::UTXO>
```

**File 2: `src/lightning/lightning_wallet.cpp`**
```bash
# Add proto include at top
# Add: #include "proto/dinerod.pb.h"

# Update variable declarations (lines 67, 146, 151, 156, 160, 168, 459)
# Before: std::vector<UTXO>
# After:  std::vector<dinerod::UTXO>
```

### Step 4: Update WalletClient (10 minutes)

**File 3: `include/lightning/wallet_client.h`**
```bash
# Add proto include
# Change line ~30 return type to std::vector<dinerod::UTXO>
```

**File 4: `src/lightning/wallet_client.cpp`**
```bash
# Change line ~41 return type to std::vector<dinerod::UTXO>
```

### Step 5: Update ChannelManager (5 minutes)

**File 5: `src/lightning/channel_manager.cpp`**
```bash
# Add proto include at top
# Update lines 153, 999 to use std::vector<dinerod::UTXO>
```

### Step 6: Build & Verify (10 minutes)
```bash
# Clean rebuild with Lightning enabled
rm -rf build
cmake -B build -DENABLE_LIGHTNING=ON
cmake --build build --target dinero_lightning -j8 2>&1 | tee build.log

# Check for UTXO type errors
grep "no type named 'UTXO'" build.log
# Should be EMPTY (error fixed) ✅

# Check for new errors
grep "error:" build.log | head -20
# Review any new errors (may still have other compilation issues)
```

### Step 7: Document Results (5 minutes)
```bash
# Create commit message
git add -A
git commit -m "lightning: Fix UTXO type mismatch - use gRPC proto types

Problem:
  Lightning code failed to compile due to using WalletManager::UTXO
  which doesn't exist. Lightning was using inconsistent UTXO types
  across different files.

Solution:
  Use dinerod::UTXO (gRPC proto type) consistently across all
  Lightning wallet interface code. This enforces the gRPC boundary
  and matches Phase 2 architecture (Lightning accesses wallet via RPC).

Changed files:
  - include/lightning/lightning_wallet.h
  - src/lightning/lightning_wallet.cpp
  - include/lightning/wallet_client.h
  - src/lightning/wallet_client.cpp
  - src/lightning/channel_manager.cpp

Impact:
  - Fixes compilation blocker for ENABLE_LIGHTNING=ON
  - No runtime behavior change (code wasn't compiling before)
  - Enforces architectural boundary (Lightning uses proto types)

Tested:
  - Build with ENABLE_LIGHTNING=ON now proceeds past UTXO type errors
  - Build with ENABLE_LIGHTNING=OFF still succeeds (no regression)

Phase: 3 - Commit 1
Risk: LOW (compilation fix only)
"
```

---

## Acceptance Criteria

**Must Pass:**
- [x] Build with `ENABLE_LIGHTNING=OFF` succeeds (no regression)
- [ ] Build with `ENABLE_LIGHTNING=ON` no longer shows UTXO type errors
- [ ] All modified files use `dinerod::UTXO` consistently
- [ ] No new compilation errors introduced by this change
- [ ] Git commit message follows format above

**Nice to Have:**
- [ ] Lightning fully compiles (may have other errors - that's OK)
- [ ] Documentation updated in `docs/PHASE3_SYMBOL_AUDIT.md`

---

## Rollback Plan

**If Commit Fails:**
```bash
# Option 1: Rollback commit
git reset --hard HEAD~1

# Option 2: Restore stash
git stash pop

# Option 3: Delete branch and start over
git checkout main
git branch -D phase3/commit1-utxo-type-fix
```

**Safe Rollback Points:**
- After each file modification (commit incrementally)
- Before Step 6 (build verification)

---

## Success Criteria & Next Steps

**This Commit is Successful When:**
1. ✅ UTXO type errors eliminated from build log
2. ✅ Code uses proto types consistently
3. ✅ No regressions in default build (ENABLE_LIGHTNING=OFF)

**Next Commit (Commit 2):**
- Fix remaining compilation errors (if any)
- OR proceed to symbol migration (Transaction::Serialize)
- See `docs/PHASE3_BUILD_DECOUPLING.md` for full plan

---

## Estimated Time Breakdown

| Step | Time | Notes |
|------|------|-------|
| Pre-flight checks | 5 min | Branch creation, baseline verification |
| File modifications | 30 min | 5 files, ~10 type changes total |
| Build & test | 10 min | Verify compilation |
| Commit & document | 5 min | Git commit with message |
| **TOTAL** | **50 min** | ~1 hour for careful execution |

---

**Document Status:** Ready to Execute
**Owner:** DineroCoin Core Team
**Prerequisites:** Phase 2 complete, Option A implemented
**Risk Assessment:** LOW - compilation fix only, no runtime changes
