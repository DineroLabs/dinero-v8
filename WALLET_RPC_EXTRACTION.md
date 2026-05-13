# Wallet RPC Extraction Plan

**Date**: October 4, 2025  
**Goal**: Extract wallet RPC handlers from main.cpp into reusable module

---

## 🎯 Problem

**Current**: main.cpp (307KB) has complete wallet RPC implementations  
**Issue**: main_clean.cpp (61KB) is missing them  
**Solution**: Extract to shared module both can use

---

## 📊 Wallet RPC Code Location in main.cpp

### **Methods to Extract** (Lines 2939-5500+)

```cpp
// main.cpp line numbers:

getwalletinfo             // 2941-2976  (36 lines)
getnewaddress             // 2978-3025  (48 lines)
getbalance                // 3027-3090  (64 lines)
listaddresses             // ~3100-3200
listtransactions          // ~3250-3400
listunspent               // ~3450-3600
createrawtransaction      // ~3650-3800
signrawtransactionwithwallet  // 3807-3950  (144 lines)
sendrawtransaction        // ~4000-4150
sendtoaddress             // 5385-5500  (116 lines)
```

**Total**: ~800-1000 lines of wallet RPC code

---

## 🏗️ New File Structure

```
src/daemon/rpc/
├── wallet_complete_handlers.h    # NEW
├── wallet_complete_handlers.cpp  # NEW
├── wallet_basic_handlers.cpp     # EXISTS (partial)
├── wallet_gui_handlers.cpp       # EXISTS (partial)
└── ...
```

---

## 📝 Implementation

### **Step 1: Create Header**

```cpp
// src/daemon/rpc/wallet_complete_handlers.h
#pragma once

#include <json/json.h>
#include <string>

// Forward declarations
namespace dinero {
    class WalletManager;
    class Blockchain;
}

class RpcRegistry;

namespace dinero::rpc {

/**
 * Register ALL wallet RPC methods (complete implementation)
 * Extracted from main.cpp lines 2939-5500
 * 
 * Methods registered:
 * - getwalletinfo
 * - getnewaddress
 * - getbalance
 * - listaddresses
 * - listtransactions
 * - listunspent
 * - createrawtransaction
 * - signrawtransactionwithwallet
 * - sendrawtransaction
 * - sendtoaddress
 */
void RegisterCompleteWalletRPC(
    RpcRegistry& registry,
    WalletManager* wallet_manager,
    Blockchain* blockchain
);

} // namespace dinero::rpc
```

### **Step 2: Extract Implementations**

```cpp
// src/daemon/rpc/wallet_complete_handlers.cpp
#include "wallet_complete_handlers.h"
#include "wallet/wallet_manager.h"
#include "daemon/blockchain.h"
#include "rpc_registry.h"
#include <iostream>

namespace dinero::rpc {

void RegisterCompleteWalletRPC(
    RpcRegistry& registry,
    WalletManager* wallet_manager,
    Blockchain* blockchain
) {
    
    // getwalletinfo (from main.cpp lines 2941-2976)
    registry.registerHandler("getwalletinfo", [wallet_manager, blockchain](const ExecutionContext& ctx, const Json::Value& params) -> Json::Value {
        Json::Value result;
        
        if (!wallet_manager) {
            result["error"] = "Wallet manager not initialized";
            return result;
        }
        
        try {
            // Update wallet manager with current blockchain height for maturity calculations
            if (blockchain) {
                uint32_t current_height = blockchain->getBlockHeight();
                wallet_manager->setBlockchainHeight(current_height);
            }
            
            auto balance = wallet_manager->getBalance();
            result["walletname"] = wallet_manager->current();
            result["walletversion"] = 1;
            result["balance"] = balance.total;
            result["spendable_balance"] = balance.spendable;
            result["confirmed_balance"] = balance.confirmed;
            result["unconfirmed_balance"] = balance.unconfirmed;
            result["immature_balance"] = balance.immature;
            result["utxo_count"] = balance.utxo_count;
            result["immature_utxo_count"] = balance.immature_utxo_count;
            result["encrypted"] = wallet_manager->isWalletEncrypted();
            result["locked"] = wallet_manager->isWalletLocked();
            result["unlocked_until"] = wallet_manager->isWalletLocked() ? 0 : (int64_t)time(nullptr) + 600;
            result["rpc_schema"] = "din.rpc.v1";
            result["schema_rev"] = 1;
        } catch (const std::exception& e) {
            result["error"] = std::string("Wallet error: ") + e.what();
        }
        
        return result;
    });
    
    // getnewaddress (from main.cpp lines 2978-3025)
    registry.registerHandler("getnewaddress", [wallet_manager](const ExecutionContext& ctx, const Json::Value& params) -> Json::Value {
        Json::Value result;
        
        if (!wallet_manager) {
            result["error"] = "Wallet manager not initialized";
            return result;
        }
        
        try {
            // Extract label parameter if provided
            std::string label = "";
            if (params.isObject() && params.isMember("label")) {
                label = params["label"].asString();
            } else if (params.isArray() && params.size() > 0) {
                label = params[0].asString();
            }
            
            // Use the wallet manager to generate a REAL spendable address
            std::string address = wallet_manager->getNewAddress(label);
                
            if (address.empty()) {
                result["error"] = "Failed to generate HD wallet address";
                return result;
            }
            
            // Return just the address string as expected by Bitcoin RPC
            return Json::Value(address);
            
        } catch (const std::exception& e) {
            result["error"] = std::string("Address generation error: ") + e.what();
        }
        
        return result;
    });
    
    // getbalance (from main.cpp lines 3027-3090)
    registry.registerHandler("getbalance", [wallet_manager](const ExecutionContext& ctx, const Json::Value& params) -> Json::Value {
        Json::Value result;
        
        if (!wallet_manager) {
            result["error"] = "Wallet manager not initialized";
            return result;
        }
        
        try {
            auto balance = wallet_manager->getBalance();
            
            const double UNA_PER_DIN = 1000000.0;
            
            result["balance"] = balance.total;
            result["confirmed"] = balance.confirmed;
            result["unconfirmed"] = balance.unconfirmed;
            result["utxo_count"] = balance.utxo_count;
            
            // Legacy format for compatibility
            result["balance_din"] = std::to_string(balance.total);
            result["balance_una"] = static_cast<int64_t>(balance.total * UNA_PER_DIN);
            result["confirmed_din"] = std::to_string(balance.confirmed);
            
        } catch (const std::exception& e) {
            result["error"] = std::string("Balance error: ") + e.what();
        }
        
        return result;
    });
    
    // TODO: Extract remaining methods:
    // - listaddresses
    // - listtransactions
    // - listunspent
    // - createrawtransaction
    // - signrawtransactionwithwallet (lines 3807-3950)
    // - sendrawtransaction
    // - sendtoaddress (lines 5385-5500)
}

} // namespace dinero::rpc
```

### **Step 3: Update main.cpp to Use Extracted Code**

```cpp
// src/daemon/main.cpp

// Add include:
#include "rpc/wallet_complete_handlers.h"

// Replace lines 2939-5500 with:
int main(int argc, char** argv) {
    // ... existing setup code ...
    
    // Register wallet RPC methods (extracted)
    dinero::rpc::RegisterCompleteWalletRPC(
        g_rpcRegistry,
        g_wallet_manager.get(),
        g_blockchain.get()
    );
    
    // ... rest of main ...
}
```

### **Step 4: Update main_clean.cpp to Use Same Code**

```cpp
// src/daemon/main_clean.cpp

// Add include:
#include "rpc/wallet_complete_handlers.h"

int main(int argc, char** argv) {
    // ... existing setup code ...
    
    // Register wallet RPC methods (same as main.cpp now!)
    dinero::rpc::RegisterCompleteWalletRPC(
        g_rpcRegistry,
        g_wallet_manager.get(),
        g_blockchain.get()
    );
    
    // ... rest of main ...
}
```

---

## 🎯 Benefits

### **Before**
```
main.cpp:          307 KB (includes ~1000 lines wallet RPC)
main_clean.cpp:    61 KB  (missing wallet RPC)

Problem: Code duplication, maintenance nightmare
```

### **After**
```
main.cpp:                    ~280 KB (calls RegisterCompleteWalletRPC)
main_clean.cpp:              ~65 KB  (calls RegisterCompleteWalletRPC)
wallet_complete_handlers.cpp: ~30 KB  (shared wallet RPC code)

Benefits:
✅ DRY (Don't Repeat Yourself)
✅ Single source of truth for wallet RPC
✅ Easier to maintain (change once, works everywhere)
✅ Can reuse in future walletd binary
```

---

## 📋 Implementation Checklist

- [ ] Create `src/daemon/rpc/wallet_complete_handlers.h`
- [ ] Create `src/daemon/rpc/wallet_complete_handlers.cpp`
- [ ] Extract `getwalletinfo` from main.cpp
- [ ] Extract `getnewaddress` from main.cpp
- [ ] Extract `getbalance` from main.cpp
- [ ] Extract `listaddresses` from main.cpp
- [ ] Extract `listtransactions` from main.cpp
- [ ] Extract `listunspent` from main.cpp
- [ ] Extract `createrawtransaction` from main.cpp
- [ ] Extract `signrawtransactionwithwallet` from main.cpp
- [ ] Extract `sendrawtransaction` from main.cpp
- [ ] Extract `sendtoaddress` from main.cpp
- [ ] Update main.cpp to use extracted code
- [ ] Update main_clean.cpp to use extracted code
- [ ] Update CMakeLists.txt to compile new file
- [ ] Test: verify all wallet RPCs still work
- [ ] Commit changes

---

## 🧪 Testing

```bash
# Build with new structure
cmake --build build --target dinerod -j8

# Test wallet RPCs
curl --user $(cat data/mainnet/.cookie) \
  --data-binary '{"method":"getnewaddress","params":[],"id":1}' \
  http://127.0.0.1:20998

curl --user $(cat data/mainnet/.cookie) \
  --data-binary '{"method":"getbalance","params":[],"id":1}' \
  http://127.0.0.1:20998

# Should work identically to before
```

---

## ⏱️ Timeline

**Estimated Time**: 3-4 hours

1. Create files (30 min)
2. Extract methods (2 hours)
3. Update main.cpp & main_clean.cpp (30 min)
4. Update CMake (15 min)
5. Test (30 min)
6. Documentation (15 min)

---

## 🎯 Success Criteria

- [ ] wallet_complete_handlers.cpp compiles
- [ ] main.cpp uses shared wallet RPC
- [ ] main_clean.cpp uses shared wallet RPC
- [ ] All wallet RPC tests pass
- [ ] Code reduced by ~1000 lines (no duplication)
- [ ] Ready for dinero-walletd extraction (Phase 2)

---

**Next**: Run `move_duplicates.sh` first, then extract wallet RPC code.

