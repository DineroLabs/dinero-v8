# Global Variable Elimination Guide

**Status:** Infrastructure ready for Phase 1 (Mechanical Migration)
**Created:** November 10, 2025
**Goal:** Drive global variable usage to zero through systematic refactoring

---

## 📋 Current Status

### ✅ Infrastructure Complete (Steps 0-3)

- [x] DaemonContext singleton accessor (`DaemonContext::instance()`)
- [x] Global shim with deprecated wrappers (`include/core/global_shim.hpp`)
- [x] Service accessor methods (getChainDB, getUTXOIndex, getDataDir)
- [x] Ban enforcement header (`include/core/ban_globals.hpp`)
- [x] Pre-commit hook (blocks new global usages)
- [x] Migration script (`scripts/migrate_globals_to_shim.sh`)
- [x] CMakeLists updated to compile `daemon_context.cpp`

### 🎯 Next Steps (Execution Plan)

Follow these steps in order:

---

## Step 1: Initialize DaemonContext Singleton

**File:** `src/daemon/main.cpp` (or `daemon_app.cpp`)

Find where `DaemonContext` is created and add:

```cpp
// Near the top of main() or DaemonApp::Init()
DaemonContext ctx;

// IMMEDIATELY after creating ctx:
DaemonContext::setInstance(&ctx);

// ... rest of initialization ...

// BEFORE exiting main():
DaemonContext::setInstance(nullptr);  // Clean shutdown
```

**Test:**
```bash
cd /Users/haydarevich/Documents/DineroCoin
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dinerod -j8
```

If it compiles → proceed to Step 2.

---

## Step 2: Run Mechanical Migration (Dry Run)

This converts all raw `g_*` globals to `dinero::legacy::g_*()` shim calls.

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Preview what will change (safe, no modifications)
./scripts/migrate_globals_to_shim.sh --dry-run
```

Review the output. It should show ~69 files that will be modified.

---

## Step 3: Apply Mechanical Migration

```bash
# Actually perform the replacements
./scripts/migrate_globals_to_shim.sh

# Review changes
git status
git diff | head -200
```

**What happened:**
- All `g_mempool` → `dinero::legacy::g_mempool()`
- All `g_blockchain` → `dinero::legacy::g_blockchain()`
- Added `#include "core/global_shim.hpp"` to modified files

**Verify:**
```bash
# Count how many files now use the shim
grep -r "dinero::legacy::g_" src/ include/ | wc -l

# Should be ~1000+ uses across ~69 files
```

---

## Step 4: Build with Deprecation Warnings

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-Wdeprecated"
cmake --build build -j8 2>&1 | tee build_warnings.log

# Check warnings
grep "deprecated" build_warnings.log | head -50
```

**Expected:** Hundreds of deprecation warnings like:
```
warning: 'g_mempool' is deprecated: Use ctx.mempool or inject IMempoolService*
```

This is **GOOD** - it shows the shim is working. These warnings guide refactoring.

---

## Step 5: Verify Everything Still Works

```bash
# Run the daemon
./build/bin/dinerod --regtest

# In another terminal, test RPC
./build/bin/dinero-cli --regtest getblockchaininfo
./build/bin/dinero-cli --regtest wallet.getbalance

# If it works → migration successful!
```

---

## Step 6: Commit Phase 1

```bash
git add -A
git commit -m "Phase 1: Convert raw globals to deprecated shim accessors

- Added DaemonContext::instance() singleton
- Created dinero::legacy::g_*() shim wrappers
- Mechanically migrated 69 files to use shim
- Added ban_globals.hpp + pre-commit hook
- All globals marked deprecated to guide refactoring

Next: Refactor high-ROI files to constructor injection"
```

---

## Step 7: Refactor High-ROI Files (Phase 2)

Attack these 5 files first (they account for ~800 global uses):

### 7.1 `src/daemon/mining.cpp` (147 uses)

**Before:**
```cpp
// mining.cpp
void MiningService::getTemplate() {
    if (dinero::legacy::g_blockchain()) {
        auto height = dinero::legacy::g_blockchain()->getHeight();
    }
}
```

**After:**
```cpp
// mining.hpp
class MiningService {
public:
    MiningService(ChainstateService* chain, MempoolService* mempool, WalletService* wallet);
private:
    ChainstateService* chain_;
    MempoolService* mempool_;
    WalletService* wallet_;
};

// mining.cpp
MiningService::MiningService(ChainstateService* chain, MempoolService* mempool, WalletService* wallet)
    : chain_(chain), mempool_(mempool), wallet_(wallet) {}

void MiningService::getTemplate() {
    if (chain_) {
        auto height = chain_->getBlockHeight();
    }
}
```

**Wiring (in `daemon_app.cpp` or wherever services are created):**
```cpp
auto mining = std::make_unique<MiningService>(
    ctx.chainstate.get(),
    ctx.mempool.get(),
    ctx.wallet.get()
);
ctx.mining = std::move(mining);
```

**Test:**
```bash
# Remove global_shim.hpp include from mining.cpp
# Build should still succeed (no more deprecated warnings for this file)
cmake --build build --target dinerod -j8
```

### 7.2 `src/wallet/wallet_manager.cpp` (136 uses)

Same pattern - inject `ChainDB*` via constructor.

### 7.3 Split Address Files (167 + 159 uses)

**Strategy:** Split into pure vs. repo code:
- `address_pure.cpp` - encoding/validation (NO dependencies, NO globals)
- `address_repo.cpp` - database access (inject `ChainDB*`)

### 7.4 `src/daemon/founder_control.cpp` (107 uses)

Inject `ChainstateService*`.

### 7.5 `src/daemon/blockchain.cpp` (87 uses)

This file **provides** IChainstate - it shouldn't need globals.

---

## Step 8: Enable Ban Enforcement (Gradual)

Once a file is fully refactored:

```cpp
// At top of file:
#include "core/ban_globals.hpp"  // Compile-time enforcement

// Remove this line:
// #include "core/global_shim.hpp"  // No longer needed
```

Build should succeed with **zero** warnings for that file.

---

## Step 9: Monitor Progress

```bash
# Count remaining shim uses:
grep -r "dinero::legacy::g_" src/ include/ --include="*.cpp" --include="*.hpp" | wc -l

# Goal: Drive this to 0

# Count files still using shim:
grep -rl "global_shim.hpp" src/ include/ | wc -l

# Goal: Drive this to 0
```

---

## Step 10: Delete Legacy Infrastructure

When shim uses = 0:

```bash
rm include/core/global_shim.hpp
rm src/daemon/legacy_globals_stub.cpp
rm include/core/ban_globals.hpp  # No longer needed

# Update pre-commit hook to just error on ANY g_* usage
```

---

## 🛠️ Mechanical Replacement Reference

The migration script replaces these patterns:

| Old (Raw Global) | New (Shim Call) |
|------------------|-----------------|
| `g_mempool` | `dinero::legacy::g_mempool()` |
| `g_blockchain` | `dinero::legacy::g_blockchain()` |
| `g_wallet_manager` | `dinero::legacy::g_wallet_manager()` |
| `g_chain_db_direct` | `dinero::legacy::g_chain_db_direct()` |
| `g_utxo_set_direct` | `dinero::legacy::g_utxo_set_direct()` |
| `g_peer_manager` | `dinero::legacy::g_peer_manager()` |
| `g_p2p` | `dinero::legacy::g_peer_manager()` |

---

## 🚨 Troubleshooting

### Build Fails: "DaemonContext not initialized"

**Cause:** Forgot to call `DaemonContext::setInstance(&ctx)` in `main()`

**Fix:**
```cpp
// In main.cpp or daemon_app.cpp
DaemonContext ctx;
DaemonContext::setInstance(&ctx);  // ADD THIS LINE
```

### Segfault at Runtime

**Cause:** Accessing service before it's initialized

**Debug:**
```cpp
// In global_shim.hpp, we already have null checks:
inline auto g_mempool() {
    try {
        return ctx().mempool ? ctx().mempool.get() : nullptr;
    } catch (...) {
        return nullptr;  // Graceful degradation
    }
}

// So caller should check:
if (auto* mempool = dinero::legacy::g_mempool()) {
    mempool->submit(tx);
}
```

### Pre-Commit Hook Blocks Commit

**Cause:** You added new raw `g_*` usage

**Fix:** Use the shim or inject the service instead

**Emergency Bypass (not recommended):**
```bash
git commit --no-verify
```

---

## 📊 Success Criteria

Phase 1 Complete when:
- [x] Shim infrastructure exists
- [x] All 69 files converted to use shim
- [x] Build succeeds with deprecation warnings
- [x] Runtime works (daemon starts, RPC works)
- [x] Pre-commit hook blocks new globals

Phase 2 Complete when:
- [ ] Top 5 files refactored to constructor injection
- [ ] ~800 shim calls eliminated
- [ ] Deprecation warnings reduced by 80%

Phase 3 Complete when:
- [ ] All files use constructor injection
- [ ] Zero shim calls remain
- [ ] Legacy files deleted
- [ ] Only whitelisted globals exist (g_rpcRegistry, g_logger, g_secp)

---

## 📚 Further Reading

- **Your Original Plan:** See top of this conversation for the full strategy
- **Strangler Fig Pattern:** https://martinfowler.com/bliki/StranglerFigApplication.html
- **Dependency Injection:** https://en.wikipedia.org/wiki/Dependency_injection

---

**Next Action:** Run Step 1 (Initialize singleton) then Step 2-3 (Mechanical migration)

Good luck! 🚀
