# Dinero Architecture: Strict Separation

**Date**: October 3, 2025  
**Priority**: 🔴 **CRITICAL** - Security and Production Requirement

## 🎯 Core Principle

**Bitcoin-style 3-tier architecture with strict separation:**

```
┌─────────────────────────────────────────────────────────────┐
│                                                               │
│  dinero-qt (GUI)                                             │
│  - User interface only                                       │
│  - NO keys, NO signing, NO wallet logic                     │
│  - Talks RPC to walletd + dinerod                           │
│                                                               │
└────────────┬──────────────────────┬─────────────────────────┘
             │                      │
             │ RPC                  │ RPC
             ▼                      ▼
┌────────────────────────┐  ┌──────────────────────────┐
│                        │  │                          │
│  dinero-walletd        │  │  dinerod                 │
│  - HD wallet           │  │  - Blockchain            │
│  - Key management      │  │  - P2P network           │
│  - BIP39/32/84         │  │  - Block validation      │
│  - Signing             │  │  - UTXO tracking         │
│  - Encryption          │  │  - Mempool               │
│  - NO blockchain       │  │  - Mining                │
│  - NO network          │  │  - NO wallet             │
│                        │  │  - NO keys               │
└────────────────────────┘  └──────────────────────────┘
```

---

## 🔒 Security Benefits

### **1. Minimal Attack Surface**
- **Node (dinerod)**: No keys = nothing to steal even if compromised
- **Wallet (walletd)**: No network = can't be attacked remotely
- **GUI (qt)**: No secrets = safe to run untrusted code

### **2. Defense in Depth**
```
Attacker compromises dinerod → Gets blockchain data, NOT keys
Attacker compromises GUI → Gets UI, NOT keys
Attacker compromises walletd → Gets keys, but isolated from network
```

### **3. Principle of Least Privilege**
- **Node**: Only needs blockchain + network access
- **Wallet**: Only needs key storage + signing capability
- **GUI**: Only needs display + user input

### **4. Production Deployment**
```
Exchanges/Services:
  ✅ Run dinerod only (no wallet attack surface)
  ✅ Wallet in separate secure environment
  ✅ Or use hardware wallet for signing

Desktop Users:
  ✅ Run all three components
  ✅ Wallet can be encrypted
  ✅ Node can't steal keys even if exploited
```

---

## 📋 Component Responsibilities

### **dinerod** (Node Only)
**What it DOES**:
- ✅ Blockchain storage and validation
- ✅ P2P network communication
- ✅ Block/transaction relay
- ✅ UTXO set management
- ✅ Mempool management
- ✅ Mining coordination (if enabled)
- ✅ RPC server for queries

**What it NEVER does**:
- ❌ Store private keys
- ❌ Sign transactions
- ❌ Generate addresses
- ❌ Manage wallet state
- ❌ Encrypt/decrypt keys
- ❌ HD derivation

**RPC Methods Exposed**:
```
# Blockchain queries
getblockchaininfo, getblock, gettransaction

# Network queries  
getpeerinfo, getnetworkinfo

# Mempool
getrawmempool, sendrawtransaction (broadcast only, NO signing)

# Mining
getblocktemplate, submitblock

# UTXO queries
listunspent, gettxout (for any address, not wallet-specific)
```

---

### **dinero-walletd** (Wallet Service Only)
**What it DOES**:
- ✅ BIP39 mnemonic generation/restoration
- ✅ HD key derivation (BIP32/BIP84)
- ✅ Address generation
- ✅ Private key storage (encrypted)
- ✅ Transaction signing
- ✅ Wallet encryption (Argon2id + AES-GCM)
- ✅ Balance tracking (queries dinerod)
- ✅ Transaction history (queries dinerod)

**What it NEVER does**:
- ❌ Store blockchain
- ❌ Validate blocks
- ❌ P2P networking
- ❌ Mining
- ❌ Relay transactions (delegates to dinerod)

**RPC Methods Exposed**:
```
# Wallet management
createwallet, loadwallet, encryptwallet, walletpassphrase

# Address generation
getnewaddress, getaddressinfo, listaddresses

# Transaction creation & signing
createrawtransaction, signrawtransactionwithwallet, sendtoaddress

# Balance & history
getbalance, listtransactions (wallet-specific, filtered)

# HD wallet
gethdinfo, deriveaddress, listhdaddresses
```

**Architecture**:
```
dinero-walletd
  ├── RPC Server (listens on 20999)
  ├── RPC Client (talks to dinerod on 20998)
  ├── Wallet Database (wallet.db - encrypted keys)
  └── No blockchain, no network, no mining
```

---

### **dinero-qt** (UI Only)
**What it DOES**:
- ✅ Display blockchain data (from dinerod)
- ✅ Display wallet data (from walletd)
- ✅ User input and interaction
- ✅ Transaction building UI
- ✅ Address display/QR codes

**What it NEVER does**:
- ❌ Store keys
- ❌ Sign transactions
- ❌ Validate blocks
- ❌ P2P networking

**Talks to BOTH**:
```cpp
RpcClient dinerodClient("http://127.0.0.1:20998");  // Blockchain
RpcClient walletdClient("http://127.0.0.1:20999");  // Wallet

// Send transaction flow:
1. GUI: User enters recipient + amount
2. GUI → walletd: "createrawtransaction + sign"
3. walletd → dinerod: "query UTXOs"
4. walletd: Creates & signs transaction locally
5. walletd → dinerod: "sendrawtransaction" (broadcast)
6. GUI: Shows confirmation
```

---

## 🔄 Communication Flow

### **Example: Send Transaction**

```
User clicks "Send 10 DIN to din1q..."
         ↓
┌────────────────────┐
│   dinero-qt        │
└────────────────────┘
         │ RPC: sendtoaddress(addr, 10)
         ↓
┌────────────────────┐
│  dinero-walletd    │
│  1. Query UTXOs    │──RPC→ dinerod: listunspent(my_addresses)
│  2. Build tx       │
│  3. Sign with key  │ (local, never leaves walletd)
│  4. Broadcast      │──RPC→ dinerod: sendrawtransaction(signed_hex)
└────────────────────┘
         │ Response: txid
         ↓
┌────────────────────┐
│   dinero-qt        │
│  Show success      │
└────────────────────┘
```

**Key Point**: Private key **NEVER** leaves walletd. Node **NEVER** sees it.

---

## 🚨 Current Violations (TO FIX)

### **Problem 1: Wallet Code in dinerod** ❌

**Current**:
```
src/daemon/
  ├── simple_wallet.cpp          ❌ WRONG - Wallet in node
  ├── hd_wallet_manager.cpp      ❌ WRONG - HD wallet in node
  ├── wallet_crypto.cpp          ❌ WRONG - Key encryption in node
  └── rpc/wallet_gui_handlers.cpp ❌ WRONG - Wallet RPC in node
```

**Should be**:
```
src/walletd/
  ├── wallet_service.cpp         ✅ Wallet service
  ├── hd_wallet.cpp              ✅ HD derivation
  ├── wallet_crypto.cpp          ✅ Key encryption
  └── rpc/wallet_rpc_server.cpp  ✅ Wallet RPC
```

---

### **Problem 2: GUI Has Signing Logic** ❌

**Current**:
```cpp
// gui/src/mainwindow.cpp
void MainWindow::onSendTransaction() {
  // GUI should NOT have any signing logic
  // Should just call walletd RPC
}
```

**Should be**:
```cpp
// gui/src/mainwindow.cpp
void MainWindow::onSendTransaction() {
  // Just call walletd RPC
  walletdClient_->sendToAddress(recipient, amount);
}
```

---

### **Problem 3: Shared Libraries** ⚠️

**Current**:
```
dinero_wallet library linked into:
  - dinerod     ❌ WRONG
  - dinero-qt   ❌ WRONG
  - walletd     ✅ CORRECT
```

**Should be**:
```
dinero_wallet library:
  - ONLY walletd uses it
  - dinerod NEVER links it
  - GUI NEVER links it
```

---

## 🎯 Migration Plan

### **Phase 1: Create dinero-walletd** (1 week)

1. **Create new binary**:
```cmake
add_executable(dinero-walletd
  src/walletd/main.cpp
  src/walletd/wallet_service.cpp
  src/walletd/wallet_rpc_server.cpp
)
target_link_libraries(dinero-walletd
  dinero_wallet  # BIP39, HD, crypto
  dinero_rpc     # RPC client to talk to dinerod
)
```

2. **Move wallet files**:
```bash
# From daemon to walletd
mv src/daemon/simple_wallet.cpp → src/walletd/wallet_service.cpp
mv src/daemon/hd_wallet_manager.cpp → src/walletd/hd_manager.cpp
mv src/daemon/wallet_crypto.cpp → src/walletd/wallet_crypto.cpp
```

3. **Split RPC handlers**:
```
dinerod RPC (port 20998):
  - getblock, gettransaction
  - getpeerinfo
  - sendrawtransaction (broadcast only)
  - listunspent (for ANY address)

walletd RPC (port 20999):
  - getnewaddress
  - getbalance
  - sendtoaddress
  - signrawtransaction
```

---

### **Phase 2: Update dinerod** (3 days)

1. **Remove wallet code**:
```cmake
# CMakeLists.txt
add_executable(dinerod
  src/daemon/main.cpp
  src/daemon/blockchain.cpp
  src/daemon/p2p_manager.cpp
  # NO wallet files
)

target_link_libraries(dinerod
  dinero_consensus
  dinero_network
  # NO dinero_wallet
)
```

2. **Remove wallet RPC**:
```cpp
// Remove from main.cpp:
- g_rpcRegistry.registerHandler("getnewaddress", ...)
- g_rpcRegistry.registerHandler("sendtoaddress", ...)
- g_rpcRegistry.registerHandler("getbalance", ...)
```

3. **Keep only node RPC**:
```cpp
// Keep in main.cpp:
+ g_rpcRegistry.registerHandler("getblock", ...)
+ g_rpcRegistry.registerHandler("sendrawtransaction", ...)  // Broadcast only
+ g_rpcRegistry.registerHandler("listunspent", ...)  // For ANY address
```

---

### **Phase 3: Update dinero-qt** (2 days)

1. **Two RPC clients**:
```cpp
class MainWindow {
  RpcClient* dinerodClient_;   // Blockchain queries
  RpcClient* walletdClient_;   // Wallet operations
};
```

2. **Split calls**:
```cpp
// Blockchain calls → dinerod
dinerodClient_->getBlock(hash);
dinerodClient_->getPeerInfo();
dinerodClient_->sendRawTransaction(signed_hex);  // Just broadcast

// Wallet calls → walletd
walletdClient_->getNewAddress();
walletdClient_->getBalance();
walletdClient_->sendToAddress(recipient, amount);  // Creates + signs + broadcasts
```

---

## 📊 Before vs After

### **Before (Current - Insecure)** ❌
```
dinerod:
  ✅ Blockchain
  ✅ Network
  ❌ Wallet code
  ❌ Private keys
  ❌ Signing logic
  
Problem: Node compromise = keys stolen
```

### **After (Separated - Secure)** ✅
```
dinerod:
  ✅ Blockchain
  ✅ Network
  ✅ NO wallet
  ✅ NO keys
  
dinero-walletd:
  ✅ Keys
  ✅ Signing
  ✅ NO network
  
Problem: Node compromise = NO keys stolen ✅
```

---

## 🎓 Why This Matters

### **Real-World Scenario**:

**Exchange running Dinero node**:
```
Without separation:
  - Must run full node with wallet
  - Node has network access + keys
  - Node exploit = all customer funds stolen
  
With separation:
  - Run dinerod only (no wallet)
  - Run walletd in separate secure environment
  - Or use hardware wallet
  - Node exploit = nothing stolen (no keys to steal)
```

**Bitcoin does this**:
- `bitcoind` = node only
- `bitcoin-wallet` = separate tool
- Exchanges run bitcoind without any wallet

**Dinero should do the same.**

---

## ✅ Definition of Done

**Separation complete when**:
1. [ ] `dinerod` compiles without linking `dinero_wallet`
2. [ ] `dinerod` has NO wallet RPC methods
3. [ ] `dinero-walletd` exists and runs independently
4. [ ] `dinero-walletd` can operate without dinerod (for key management)
5. [ ] `dinero-qt` talks to BOTH services via RPC
6. [ ] Running dinerod alone = safe for exchanges/services
7. [ ] Keys physically impossible to access from node code

---

## 📝 Summary

**Current State**: Wallet and node are mixed (insecure, not production-ready)

**Required State**: Bitcoin-style separation (secure, production-ready)

**Effort**: ~2 weeks of refactoring

**Benefit**: 
- ✅ Production security
- ✅ Exchange-friendly
- ✅ Clean architecture
- ✅ Industry best practice

🎯 **This is NOT optional for mainnet launch. This is a security requirement.**

---

## 🚀 Implementation Priority

**Priority**: 🔴 **CRITICAL**

**Must be done BEFORE**:
- Mainnet launch
- Exchange listings
- Production use

**Can wait UNTIL**:
- After testnet is stable
- After core features complete
- When preparing for mainnet

**Current status**: In development, wallet/node mixed is OK for testing

**Target**: Separate before mainnet beta (Q1 2026?)

---

**Remember**: Bitcoin learned this the hard way. Don't repeat their mistakes. Separate from the start.

