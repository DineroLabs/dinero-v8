# Duplicate Files Analysis - Dinero Project

**Date**: October 4, 2025  
**Total Duplicates Found**: 61 backup files + multiple main.cpp variants

---

## 📊 Summary

### **Backup Files by Type**
- `.bak` files: 30+
- `.backup` files: 8+
- `_backup` suffix: 5+
- `_old` / `.old`: 3+
- Multiple `main.cpp` variants: 3 files

### **Critical Findings**

#### **1. Multiple Main Implementations** ⚠️

```
src/daemon/
├── main.cpp          (307 KB) ✅ COMPLETE - Full wallet RPC
├── main_clean.cpp    (61 KB)  ⚠️ MINIMAL - Missing wallet code
└── main_simple.cpp   (8 KB)   ⚠️ VERY BASIC
```

**Recommendation**: 
- **Keep**: `main.cpp` (most complete)
- **Move to duplicates**: `main_clean.cpp`, `main_simple.cpp`
- **Action**: Extract wallet RPC from main.cpp to reuse in main_clean.cpp if needed

#### **2. Duplicate File Basenames** (Same filename in different dirs)

```
Found 20+ files with same basename in multiple directories:
- address.cpp (4 locations)
- RpcClient.cpp (3 locations)  
- MainWindowWallet.cpp (2 locations)
- MainWindowMiner.cpp (2 locations)
- wallet_*.cpp (11+ files in daemon/rpc/)
```

---

## 📋 Complete Duplicate File List

### **Consensus Module**
```bash
src/consensus/
├── genesis_premine_test.cpp.bak
├── chain_manager.cpp.bak
├── genesis_premine.cpp.bak
└── commitment.cpp.bak
```

### **Privacy/Coinjoin**
```bash
src/core/privacy/
├── coinjoin_adapter_jm.cpp.bak
└── coinjoin_adapter_generic.cpp.bak

src/privacy/
└── coinjoin_factory.cpp.bak
```

### **Wallet Duplicates**
```bash
src/core/wallet/
└── descriptor_wallet.cpp.backup

src/daemon/
├── wallet.cpp.bak
├── test_vesting_system.cpp.bak
└── test_wallet_integration.cpp
```

### **CLI Duplicates**
```bash
src/cli/
├── main_backup.cpp
├── main_new.cpp.bak
├── ws_client.cpp.bak
├── NodeinfoValidator.cpp.bak
└── retry.cpp.bak
```

### **Mining Duplicates**
```bash
src/mining/
├── block_assembler.cpp.bak
└── miner.cpp.bak

src/daemon/
├── mining_engine.cpp.backup
├── mining.cpp.bak
├── gbt_work_manager.cpp.backup
└── gbt_work_manager.cpp.bak
```

### **Storage/Database**
```bash
src/storage/
├── atomic_block_writer.cpp.bak
├── schema_manager.cpp.bak
└── backup_manager.cpp.bak

src/common/
└── blockchain_db_backup.cpp (not backup, but name suggests it)
```

### **Daemon Core**
```bash
src/daemon/
├── main.cpp.backup
├── block_acceptor.cpp.backup
├── rpc_server_clean.cpp.bak
├── gbt_work_manager.cpp.backup
└── gbt_work_manager.cpp.bak
```

### **Auth**
```bash
src/auth/
└── auth_store.cpp.bak
```

### **RPC**
```bash
src/daemon/rpc/
└── encrypted_key_validator.cpp.bak
```

---

## 🔍 Wallet Code Comparison: main.cpp vs main_clean.cpp

### **main.cpp (COMPLETE)** ✅

**Wallet RPC Methods Present**:
- `getwalletinfo` (lines 2941-2976) - Full balance details
- `getnewaddress` (lines 2978-3025) - HD wallet integration
- `getbalance` (lines 3027-3050+) - Complete balance tracking
- `signrawtransactionwithwallet` (line 3807+)
- `sendtoaddress` (lines 5385+)
- `listtransactions` (implemented)
- `listunspent` (implemented)

**Key Features**:
```cpp
// Lines 2978-3025: getnewaddress with HD wallet
g_rpcRegistry.registerHandler("getnewaddress", [](const ::ExecutionContext& ctx, const din::Json& params) -> din::Json {
    if (!g_wallet_manager) {
        result["error"] = "Wallet manager not initialized";
        return result;
    }
    
    // Extract label parameter
    std::string label = "";
    if (params.isObject() && params.isMember("label")) {
        label = params["label"].asString();
    }
    
    // Use wallet manager to generate REAL spendable address
    address = g_wallet_manager->getNewAddress(label);
    
    if (!address.empty()) {
        dinero::g_logger.info("✅ Generated real wallet address: " + address);
    }
    
    return din::Json(address);
});
```

### **main_clean.cpp (INCOMPLETE)** ⚠️

**Status**: 
- Has basic structure
- Missing wallet RPC handlers
- Only ~61KB vs 307KB in main.cpp
- Includes wallet headers but no implementations

**Missing**:
- No `getnewaddress` implementation
- No `getbalance` implementation  
- No `sendtoaddress` implementation
- No wallet RPC registry

---

## 🎯 Consolidation Strategy

### **Phase 1: Move Obvious Duplicates** (Now)

**Action**: Move all `.bak`, `.backup`, `_backup`, `_old` files to `duplicates/` folder

```bash
# Organized structure:
duplicates/
├── consensus/
│   ├── genesis_premine_test.cpp.bak
│   ├── chain_manager.cpp.bak
│   └── ...
├── daemon/
│   ├── main.cpp.backup
│   ├── main_clean.cpp (candidate)
│   ├── main_simple.cpp (candidate)
│   └── ...
├── cli/
│   └── ...
└── README.md (explanation of why files are here)
```

### **Phase 2: Extract Wallet RPC to Shared Module** (Later)

**Goal**: Make wallet RPC reusable between main.cpp and main_clean.cpp

**Create**:
```cpp
// src/daemon/rpc/wallet_complete_handlers.cpp
// Extract lines 2978-5500 from main.cpp into reusable functions

namespace dinero::rpc {
    void RegisterWalletRPC(RpcRegistry& registry, WalletManager* wallet_mgr);
}
```

**Then both main.cpp and main_clean.cpp can do**:
```cpp
#include "rpc/wallet_complete_handlers.h"
dinero::rpc::RegisterWalletRPC(g_rpcRegistry, g_wallet_manager.get());
```

### **Phase 3: Consolidate Duplicate Implementations** (After separation)

**Files to Consolidate**:
1. `address.cpp` (4 versions) - Keep one canonical version
2. `RpcClient.cpp` (3 versions) - Keep GUI version
3. `wallet_*.cpp` in `daemon/rpc/` (11 files) - Merge into wallet service

---

## 📝 Immediate Actions

### **1. Move Backup Files** ✅ (Safe - Can be undone)

```bash
# Create organized duplicates directory
mkdir -p duplicates/{consensus,privacy,wallet,cli,mining,storage,daemon,auth,rpc}

# Move all .bak files
find src -name "*.bak" -exec sh -c 'mkdir -p "duplicates/$(dirname $1 | sed s/src\\///)"; mv "$1" "duplicates/$(dirname $1 | sed s/src\\///)/"' _ {} \;

# Move all .backup files
find src -name "*.backup" -exec sh -c 'mkdir -p "duplicates/$(dirname $1 | sed s/src\\///)"; mv "$1" "duplicates/$(dirname $1 | sed s/src\\///)/"' _ {} \;

# Move alternative main files
mv src/daemon/main_clean.cpp duplicates/daemon/
mv src/daemon/main_simple.cpp duplicates/daemon/
mv src/cli/main_backup.cpp duplicates/cli/
```

### **2. Extract Wallet RPC from main.cpp** 🔄 (Reuse code)

**Create**: `src/daemon/rpc/wallet_complete_handlers.cpp`

**Extract from main.cpp**:
- Lines 2941-2976: `getwalletinfo`
- Lines 2978-3025: `getnewaddress`
- Lines 3027-3090: `getbalance`
- Lines 3807+: `signrawtransactionwithwallet`
- Lines 5385+: `sendtoaddress`
- All other wallet RPCs

**Benefits**:
- ✅ Reusable in multiple binaries (dinerod, future walletd)
- ✅ Easier to maintain (one implementation)
- ✅ Can copy to main_clean.cpp if needed

### **3. Document What We Keep** 📚

**Active Files** (DO NOT MOVE):
- `src/daemon/main.cpp` - Primary daemon (most complete)
- `src/wallet/hd_wallet.cpp` - Real HD wallet implementation
- `src/wallet/bip39.cpp` - BIP39 implementation
- `gui/src/mainwindow.cpp` - Qt GUI (complete)

**Duplicates Moved** (Can restore if needed):
- All `.bak` / `.backup` files → `duplicates/`
- Alternative main files → `duplicates/daemon/`
- Test files → `duplicates/` (keep if used)

---

## ⚠️ Important Notes

### **Before Moving Files**:
1. ✅ Check if file is referenced in CMakeLists.txt
2. ✅ Check if file is imported anywhere
3. ✅ Verify we have working replacement
4. ✅ Keep duplicates/ folder in git (don't delete)

### **After Moving**:
1. ✅ Verify build still works: `cmake --build build`
2. ✅ Verify tests pass
3. ✅ Document in duplicates/README.md why each file was moved

---

## 🎯 Success Criteria

**Duplicate Cleanup Complete When**:
- [ ] All `.bak` / `.backup` files in `duplicates/`
- [ ] Only ONE canonical version of each active file
- [ ] Build system clean (no duplicate symbol errors)
- [ ] Wallet RPC code extracted and reusable
- [ ] Documentation explains all moves

**Benefits**:
- ✅ Cleaner project structure
- ✅ Faster builds (fewer files)
- ✅ No confusion about which file is "correct"
- ✅ Easy to restore if needed (just copy from duplicates/)

---

## 📚 Next Steps

1. **Run duplicate mover script** (see next file)
2. **Extract wallet RPC** to shared module
3. **Update CMakeLists.txt** to remove moved files
4. **Test build** to ensure nothing breaks
5. **Commit** with message: "chore: organize duplicate files"

**Timeline**: 2-3 hours
**Risk**: LOW (files moved, not deleted)
**Reversible**: YES (just copy from duplicates/ back)

