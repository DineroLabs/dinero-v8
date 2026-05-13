# Dinero Architecture Separation - Detailed Implementation Plan

**Date**: October 4, 2025  
**Status**: 🔴 **PLANNING PHASE**  
**Priority**: Critical for production readiness

---

## 🎯 Executive Summary

**Current State**: Wallet and node code are dangerously mixed  
**Target State**: Bitcoin-style 3-tier separation (node | wallet | GUI)  
**Timeline**: 2 weeks  
**Blockers**: None - all dependencies available

---

## 📊 Current Architecture Analysis

### **What We Found** (October 4, 2025)

#### **1. Wallet Code Living in `dinerod`** ❌

```bash
src/daemon/
├── simple_wallet.cpp          # ❌ Wallet logic in node
├── hd_wallet_manager.cpp      # ❌ HD derivation in node  
├── hd_wallet_manager.h
├── wallet_crypto.cpp          # ❌ Key encryption in node
├── wallet_crypto.h
└── rpc/
    ├── WalletHandlers.cpp     # ❌ Wallet RPC in node
    ├── WalletHandlers.h
    ├── wallet_basic_handlers.cpp
    ├── wallet_gui_handlers.cpp
    ├── wallet_rpc_handlers.cpp
    └── wallet_stage3_handlers.cpp
```

**Risk**: Node compromise = ALL wallet keys stolen

---

#### **2. CMake Linking Violations** ❌

```cmake
# CMakeLists.txt line 137-150 (macOS)
add_executable(dinerod
  src/daemon/main.cpp
  # ... node files ...
  src/daemon/simple_wallet.cpp      # ❌ WALLET
  src/daemon/hd_wallet_manager.cpp  # ❌ WALLET
  src/daemon/wallet_crypto.cpp      # ❌ WALLET
)

# Line 175-178
target_link_libraries(dinerod PRIVATE 
  dinero_consensus
  dinero_wallet              # ❌ NODE LINKS WALLET LIB
  dinero_rpc_handlers        # ❌ CONTAINS WALLET RPCS
  /opt/homebrew/lib/libjsoncpp.dylib
  /opt/homebrew/lib/libsecp256k1.dylib
)
```

**Problem**: Node binary contains ALL wallet code

---

#### **3. Wallet RPC Methods in Node** ❌

```cpp
// src/daemon/main.cpp - lines 2978-5499
g_rpcRegistry.registerHandler("getnewaddress", ...);           // ❌ WALLET
g_rpcRegistry.registerHandler("getbalance", ...);              // ❌ WALLET
g_rpcRegistry.registerHandler("signrawtransactionwithwallet", ...); // ❌ WALLET
g_rpcRegistry.registerHandler("sendtoaddress", ...);           // ❌ WALLET
```

**Problem**: Node exposes wallet operations = attack surface

---

#### **4. GUI Connects to Node for Wallet Ops** ⚠️

```cpp
// gui/src/mainwindow.cpp
rpc_->createWallet(...);      // ⚠️ Should call walletd
rpc_->getNewAddress(...);     // ⚠️ Should call walletd
rpc_->sendToAddress(...);     // ⚠️ Should call walletd
```

**Problem**: GUI thinks node has wallet = no separation possible

---

## 🏗️ Target Architecture (Bitcoin-Style)

```
┌────────────────────────────────────────────────────────┐
│                    dinero-qt (GUI)                      │
│  - Display only, NO secrets                            │
│  - Talks to BOTH services via RPC                      │
└──────────┬─────────────────────────┬───────────────────┘
           │                         │
           │ RPC :20999             │ RPC :20998
           │ (wallet ops)            │ (blockchain ops)
           ▼                         ▼
┌──────────────────────┐    ┌──────────────────────┐
│   dinero-walletd     │    │     dinerod          │
│   (Keys & Signing)   │    │   (Node & Chain)     │
├──────────────────────┤    ├──────────────────────┤
│ ✅ BIP39/32/84       │    │ ✅ Blockchain        │
│ ✅ Address gen       │    │ ✅ P2P network       │
│ ✅ Key storage       │    │ ✅ UTXO index        │
│ ✅ Tx signing        │    │ ✅ Mempool           │
│ ✅ Argon2id + AES    │    │ ✅ Mining            │
│ ❌ NO blockchain     │    │ ❌ NO wallet         │
│ ❌ NO networking     │    │ ❌ NO keys           │
│ ❌ NO mining         │    │ ❌ NO signing        │
└──────────────────────┘    └──────────────────────┘
         │                           ▲
         │ RPC :20998 (query only)   │
         └───────────────────────────┘
         (walletd queries blockchain from dinerod)
```

---

## 📋 Implementation Plan (2 Weeks)

### **Week 1: Create `dinero-walletd`**

---

#### **Day 1-2: Project Structure**

**1. Create walletd directory structure**:

```bash
mkdir -p src/walletd/{rpc,storage,crypto}

# New files to create:
src/walletd/
├── main.cpp                    # Wallet service entry point
├── wallet_service.cpp          # Core wallet service
├── wallet_service.h
├── rpc/
│   ├── wallet_rpc_server.cpp   # RPC server (port 20999)
│   ├── wallet_rpc_server.h
│   ├── wallet_rpc_handlers.cpp # RPC method implementations
│   └── wallet_rpc_handlers.h
└── storage/
    ├── wallet_db.cpp           # Encrypted wallet storage
    └── wallet_db.h
```

**2. Move existing wallet files**:

```bash
# Move from daemon to walletd
mv src/daemon/simple_wallet.cpp src/walletd/simple_wallet.cpp
mv src/daemon/simple_wallet.h src/walletd/simple_wallet.h
mv src/daemon/hd_wallet_manager.cpp src/walletd/hd_wallet_manager.cpp
mv src/daemon/hd_wallet_manager.h src/walletd/hd_wallet_manager.h
mv src/daemon/wallet_crypto.cpp src/walletd/crypto/wallet_crypto.cpp
mv src/daemon/wallet_crypto.h src/walletd/crypto/wallet_crypto.h

# Move wallet RPC handlers
mv src/daemon/rpc/WalletHandlers.cpp src/walletd/rpc/wallet_rpc_handlers.cpp
mv src/daemon/rpc/WalletHandlers.h src/walletd/rpc/wallet_rpc_handlers.h
mv src/daemon/rpc/wallet_basic_handlers.cpp src/walletd/rpc/
mv src/daemon/rpc/wallet_gui_handlers.cpp src/walletd/rpc/
mv src/daemon/rpc/wallet_rpc_handlers.cpp src/walletd/rpc/
mv src/daemon/rpc/wallet_stage3_handlers.cpp src/walletd/rpc/
```

**3. Update includes in moved files**:

```cpp
// Old:
#include "daemon/simple_wallet.h"
#include "daemon/rpc/WalletHandlers.h"

// New:
#include "walletd/simple_wallet.h"
#include "walletd/rpc/wallet_rpc_handlers.h"
```

---

#### **Day 3-4: Implement `dinero-walletd` Core**

**1. Create `src/walletd/main.cpp`**:

```cpp
// src/walletd/main.cpp
#include "walletd/wallet_service.h"
#include "walletd/rpc/wallet_rpc_server.h"
#include <cstdlib>
#include <iostream>
#include <signal.h>

namespace {
std::unique_ptr<dinero::WalletService> g_wallet_service;
std::unique_ptr<dinero::WalletRpcServer> g_rpc_server;
volatile sig_atomic_t shutdown_flag = 0;

void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nShutdown signal received...\n";
        shutdown_flag = 1;
    }
}
} // namespace

int main(int argc, char** argv) {
    // Parse args
    std::string datadir = std::getenv("HOME") + std::string("/Documents/DineroCoin/data/mainnet/wallet");
    std::string dinerod_url = "http://127.0.0.1:20998"; // Default node RPC
    uint16_t rpc_port = 20999; // Wallet RPC port
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-datadir" && i + 1 < argc) {
            datadir = argv[++i];
        } else if (arg == "-rpcport" && i + 1 < argc) {
            rpc_port = std::stoi(argv[++i]);
        } else if (arg == "-dinerod" && i + 1 < argc) {
            dinerod_url = argv[++i];
        } else if (arg == "-help" || arg == "--help") {
            std::cout << "Usage: dinero-walletd [options]\n"
                      << "  -datadir=<path>   Wallet data directory\n"
                      << "  -rpcport=<port>   RPC server port (default: 20999)\n"
                      << "  -dinerod=<url>    Node RPC URL (default: http://127.0.0.1:20998)\n"
                      << "  -help             Show this help\n";
            return 0;
        }
    }
    
    std::cout << "🔑 Dinero Wallet Service v0.1.0\n";
    std::cout << "📂 Data directory: " << datadir << "\n";
    std::cout << "🌐 RPC server: http://127.0.0.1:" << rpc_port << "\n";
    std::cout << "🔗 Node URL: " << dinerod_url << "\n\n";
    
    // Signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        // Initialize wallet service
        g_wallet_service = std::make_unique<dinero::WalletService>(datadir, dinerod_url);
        if (!g_wallet_service->initialize()) {
            std::cerr << "❌ Failed to initialize wallet service\n";
            return 1;
        }
        
        // Start RPC server
        g_rpc_server = std::make_unique<dinero::WalletRpcServer>(rpc_port, g_wallet_service.get());
        if (!g_rpc_server->start()) {
            std::cerr << "❌ Failed to start RPC server\n";
            return 1;
        }
        
        std::cout << "✅ Wallet service started. Press Ctrl+C to stop.\n";
        
        // Main loop
        while (!shutdown_flag) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        // Shutdown
        std::cout << "\n🛑 Shutting down wallet service...\n";
        g_rpc_server->stop();
        g_wallet_service->shutdown();
        std::cout << "✅ Wallet service stopped.\n";
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Fatal error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
```

**2. Create `src/walletd/wallet_service.h`**:

```cpp
// src/walletd/wallet_service.h
#pragma once

#include "walletd/hd_wallet_manager.h"
#include "walletd/simple_wallet.h"
#include <memory>
#include <string>

namespace dinero {

class WalletService {
public:
    WalletService(const std::string& datadir, const std::string& node_rpc_url);
    ~WalletService();
    
    bool initialize();
    void shutdown();
    
    // Wallet management
    bool hasWallet() const;
    bool createWallet(const std::string& name, int word_count = 12);
    bool loadWallet(const std::string& name);
    bool unlockWallet(const std::string& password, int timeout_seconds = 60);
    bool lockWallet();
    
    // Address generation
    std::string generateAddress(const std::string& label = "");
    std::vector<std::string> listAddresses() const;
    
    // Transaction operations
    std::string createRawTransaction(const Json::Value& inputs, const Json::Value& outputs);
    std::string signRawTransaction(const std::string& hex_tx);
    std::string sendToAddress(const std::string& address, uint64_t amount_una);
    
    // Balance & history
    uint64_t getBalance() const;
    std::vector<Json::Value> listTransactions(int count = 10) const;
    
    // Node RPC client (for querying blockchain)
    Json::Value callNode(const std::string& method, const Json::Value& params);
    
private:
    std::string datadir_;
    std::string node_rpc_url_;
    std::unique_ptr<HDWalletManager> hd_wallet_;
    std::unique_ptr<SimpleWallet> simple_wallet_;
    bool initialized_;
};

} // namespace dinero
```

**3. Create `src/walletd/rpc/wallet_rpc_server.h`**:

```cpp
// src/walletd/rpc/wallet_rpc_server.h
#pragma once

#include "walletd/wallet_service.h"
#include <cstdint>
#include <memory>

namespace dinero {

class WalletRpcServer {
public:
    WalletRpcServer(uint16_t port, WalletService* wallet_service);
    ~WalletRpcServer();
    
    bool start();
    void stop();
    
private:
    uint16_t port_;
    WalletService* wallet_service_;
    // (same HTTP server as dinerod - reuse http_rpc_server.cpp)
};

} // namespace dinero
```

---

#### **Day 5: Update CMakeLists.txt for walletd**

```cmake
# Add to root CMakeLists.txt

# ========================================
# dinero-walletd (Wallet Service)
# ========================================

add_executable(dinero-walletd
  src/walletd/main.cpp
  src/walletd/wallet_service.cpp
  src/walletd/hd_wallet_manager.cpp
  src/walletd/simple_wallet.cpp
  src/walletd/crypto/wallet_crypto.cpp
  src/walletd/rpc/wallet_rpc_server.cpp
  src/walletd/rpc/wallet_rpc_handlers.cpp
  src/walletd/rpc/wallet_basic_handlers.cpp
  src/walletd/rpc/wallet_gui_handlers.cpp
  
  # Reuse HTTP server from daemon
  src/daemon/http_rpc_server.cpp
  src/daemon/rpc_auth.cpp
)

target_link_libraries(dinero-walletd PRIVATE
  dinero_wallet        # BIP39, HD, crypto
  dinero_crypto        # SHA256, RIPEMD160
  OpenSSL::SSL
  OpenSSL::Crypto
  pthread
  sqlite3
)

if(APPLE)
  target_link_libraries(dinero-walletd PRIVATE
    /opt/homebrew/lib/libjsoncpp.dylib
    /opt/homebrew/lib/libsecp256k1.dylib
    ${SECURITY_FRAMEWORK}
  )
else()
  target_link_libraries(dinero-walletd PRIVATE
    jsoncpp
    secp256k1
  )
endif()

target_include_directories(dinero-walletd PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/generated
)

# Install
install(TARGETS dinero-walletd DESTINATION bin)

message(STATUS "  ✅ dinero-walletd - Wallet service (keys, signing, RPC :20999)")
```

---

### **Week 2: Clean `dinerod` & Update GUI**

---

#### **Day 6-7: Remove Wallet from `dinerod`**

**1. Remove wallet files from dinerod**:

```cmake
# CMakeLists.txt - UPDATE dinerod target

add_executable(dinerod
  src/daemon/main.cpp
  src/daemon/http_rpc_server.cpp
  src/daemon/simple_blockchain.cpp
  src/daemon/p2p_manager.cpp
  src/daemon/transaction_pool.cpp
  src/daemon/bech32_encoder.cpp
  src/daemon/secure_random.cpp
  src/daemon/crypto_utils.cpp
  src/daemon/rpc_auth.cpp
  src/daemon/consensus_subsidy.cpp
  # ❌ REMOVED: simple_wallet.cpp
  # ❌ REMOVED: hd_wallet_manager.cpp
  # ❌ REMOVED: wallet_crypto.cpp
)

target_link_libraries(dinerod PRIVATE 
  dinero_consensus
  # ❌ REMOVED: dinero_wallet
  # ❌ REMOVED: dinero_rpc_handlers (wallet RPCs)
  /opt/homebrew/lib/libjsoncpp.dylib
  /opt/homebrew/lib/libsecp256k1.dylib
)
```

**2. Remove wallet RPC handlers from `src/daemon/main.cpp`**:

```cpp
// src/daemon/main.cpp - REMOVE these registrations

// ❌ REMOVE:
// g_rpcRegistry.registerHandler("getnewaddress", ...);
// g_rpcRegistry.registerHandler("getbalance", ...);
// g_rpcRegistry.registerHandler("signrawtransactionwithwallet", ...);
// g_rpcRegistry.registerHandler("sendtoaddress", ...);
// g_rpcRegistry.registerHandler("createhdwallet", ...);
// g_rpcRegistry.registerHandler("restorewallet", ...);

// ✅ KEEP (blockchain RPCs):
g_rpcRegistry.registerHandler("getblock", ...);
g_rpcRegistry.registerHandler("gettransaction", ...);
g_rpcRegistry.registerHandler("getpeerinfo", ...);
g_rpcRegistry.registerHandler("sendrawtransaction", ...);  // Broadcast only
g_rpcRegistry.registerHandler("listunspent", ...);  // Query any address
g_rpcRegistry.registerHandler("geteconomics", ...);
g_rpcRegistry.registerHandler("getsupply", ...);
```

**3. Verify dinerod compiles without wallet**:

```bash
cmake --build build --target dinerod -j8

# Should compile WITHOUT:
# - dinero_wallet library
# - wallet_*.cpp files
# - Any BIP39/HD code
```

---

#### **Day 8-9: Update GUI for Two RPC Clients**

**1. Add second RPC client to `gui/src/mainwindow.h`**:

```cpp
// gui/src/mainwindow.h

class MainWindow : public QMainWindow {
    Q_OBJECT
    
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    
private:
    RpcClient* dinerodClient_;   // Blockchain queries (port 20998)
    RpcClient* walletdClient_;   // Wallet operations (port 20999)
    
    // Split responsibilities:
    void queryBlockchain();  // Uses dinerodClient_
    void queryWallet();      // Uses walletdClient_
};
```

**2. Update `gui/src/mainwindow.cpp` to use two clients**:

```cpp
// gui/src/mainwindow.cpp

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // Connect to node (blockchain)
    dinerodClient_ = new RpcClient("http://127.0.0.1:20998", this);
    
    // Connect to wallet service (keys & signing)
    walletdClient_ = new RpcClient("http://127.0.0.1:20999", this);
    
    setupConnections();
}

void MainWindow::onGenerateAddress() {
    // OLD: dinerodClient_->getNewAddress();  // ❌ Wrong - node has no wallet
    // NEW:
    walletdClient_->getNewAddress();  // ✅ Correct - wallet service
}

void MainWindow::onSendTransaction() {
    QString recipient = ui->recipientEdit->text();
    double amount = ui->amountEdit->text().toDouble();
    
    // OLD: dinerodClient_->sendToAddress(recipient, amount);  // ❌ Wrong
    // NEW:
    walletdClient_->sendToAddress(recipient, amount);  // ✅ Wallet signs & broadcasts
}

void MainWindow::onGetBalance() {
    // OLD: dinerodClient_->getBalance();  // ❌ Wrong
    // NEW:
    walletdClient_->getBalance();  // ✅ Wallet tracks balance
}

void MainWindow::onGetBlockInfo() {
    // This stays with node:
    dinerodClient_->getBlock(hash);  // ✅ Correct - blockchain query
}
```

**3. Update `gui/src/rpcclient.cpp` to handle wallet errors**:

```cpp
// gui/src/rpcclient.cpp

void RpcClient::getNewAddress() {
    call("getnewaddress", QJsonArray(), [this](const QJsonObject& result) {
        if (result.contains("error")) {
            // Check if walletd is not running
            QString error = result["error"].toObject()["message"].toString();
            if (error.contains("Connection refused") || error.contains("20999")) {
                emit errorOccurred("⚠️ Wallet service not running. Start with: dinero-walletd");
            } else {
                emit errorOccurred("❌ " + error);
            }
        } else {
            QString address = result["result"].toString();
            emit addressReady(address);
        }
    });
}
```

---

#### **Day 10: Testing & Documentation**

**1. Test the separated system**:

```bash
# Terminal 1: Start node (NO wallet)
./build/dinero-walletd/dinero-walletd \
  -datadir=/Users/haydarevich/Documents/DineroCoin/data/mainnet/node \
  -rpcport=20998

# Terminal 2: Start wallet service
./build/dinero-walletd/dinero-walletd \
  -datadir=/Users/haydarevich/Documents/DineroCoin/data/mainnet/wallet \
  -rpcport=20999 \
  -dinerod=http://127.0.0.1:20998

# Terminal 3: Test wallet RPC
curl --user $(cat ~/.dinero/mainnet/.cookie) \
  --data-binary '{"method":"getnewaddress","params":[],"id":1}' \
  http://127.0.0.1:20999

# Terminal 4: Test node RPC (wallet methods should fail)
curl --user $(cat ~/.dinero/mainnet/.cookie) \
  --data-binary '{"method":"getnewaddress","params":[],"id":1}' \
  http://127.0.0.1:20998
# Should return: "Method not found" ✅

# Terminal 5: Launch GUI
./build/gui/dinero-qt6.app/Contents/MacOS/dinero-qt6
# - Generate address → calls walletd ✅
# - Get block info → calls dinerod ✅
```

**2. Verify separation**:

```bash
# Check dinerod does NOT link wallet
otool -L build/dinerod | grep -i wallet
# Should be empty ✅

# Check dinero-walletd DOES link wallet
otool -L build/dinero-walletd | grep -i wallet
# Should show dinero_wallet library ✅

# Check dinerod binary size (should be smaller)
ls -lh build/dinerod build/dinero-walletd
```

**3. Create startup scripts**:

```bash
# scripts/start-all.sh
#!/bin/bash
set -e

DATADIR="${HOME}/Documents/DineroCoin/data"

echo "🚀 Starting Dinero (separated architecture)"

# Start node
echo "📦 Starting dinerod (blockchain node)..."
./build/dinerod \
  -datadir="${DATADIR}/mainnet/node" \
  -rpcport=20998 \
  -daemon

# Wait for node to start
sleep 2

# Start wallet service
echo "🔑 Starting dinero-walletd (wallet service)..."
./build/dinero-walletd \
  -datadir="${DATADIR}/mainnet/wallet" \
  -rpcport=20999 \
  -dinerod=http://127.0.0.1:20998 \
  -daemon

# Wait for wallet to start
sleep 2

# Launch GUI
echo "🖥️  Starting dinero-qt (GUI)..."
./build/gui/dinero-qt6.app/Contents/MacOS/dinero-qt6 &

echo "✅ All services started!"
echo "   Node:   http://127.0.0.1:20998"
echo "   Wallet: http://127.0.0.1:20999"
echo "   GUI:    Running in background"
```

---

## 📝 Verification Checklist

### **Architecture Separation (All Must Pass)**

- [ ] **dinerod compiles without `dinero_wallet` library**
- [ ] **dinerod does NOT link to `dinero_wallet`**
- [ ] **dinerod binary does NOT contain wallet symbols** (check with `nm`)
- [ ] **dinerod RPC does NOT expose wallet methods**
  - [ ] `getnewaddress` → "Method not found"
  - [ ] `getbalance` → "Method not found"
  - [ ] `sendtoaddress` → "Method not found"
  - [ ] `signrawtransactionwithwallet` → "Method not found"
- [ ] **dinerod DOES expose blockchain methods**
  - [ ] `getblock` → works
  - [ ] `sendrawtransaction` → works (broadcast only)
  - [ ] `listunspent` → works (for any address)

- [ ] **dinero-walletd compiles and runs independently**
- [ ] **dinero-walletd DOES link to `dinero_wallet` library**
- [ ] **dinero-walletd RPC exposes wallet methods**
  - [ ] `getnewaddress` → generates address
  - [ ] `getbalance` → returns balance
  - [ ] `sendtoaddress` → creates, signs, broadcasts tx
- [ ] **dinero-walletd can query dinerod for blockchain data**

- [ ] **dinero-qt connects to BOTH services**
  - [ ] Wallet ops → `http://127.0.0.1:20999` (walletd)
  - [ ] Blockchain ops → `http://127.0.0.1:20998` (dinerod)
- [ ] **dinero-qt does NOT crash if walletd not running**
  - [ ] Shows error: "Wallet service not running"
- [ ] **dinero-qt does NOT crash if dinerod not running**
  - [ ] Shows error: "Node not running"

### **Security Verification**

- [ ] **Run dinerod alone (no walletd)**
  - [ ] Node runs normally
  - [ ] No wallet RPC methods available
  - [ ] No keys in memory (verify with debugger)
  
- [ ] **Run walletd alone (no dinerod)**
  - [ ] Wallet service starts
  - [ ] Can generate addresses
  - [ ] Cannot query blockchain (expected)
  
- [ ] **Symbolic analysis**:
```bash
# dinerod should NOT have these symbols:
nm build/dinerod | grep -i "HDWallet\|BIP39\|wallet_crypto"
# Empty output = success ✅

# dinero-walletd SHOULD have these symbols:
nm build/dinero-walletd | grep -i "HDWallet\|BIP39\|wallet_crypto"
# Shows symbols = success ✅
```

---

## 🎯 Success Criteria

### **Definition of Done**

**1. Separation Complete**:
- [x] Architecture document written
- [ ] `dinerod` has NO wallet code
- [ ] `dinero-walletd` exists and works
- [ ] `dinero-qt` talks to both services

**2. Security Validated**:
- [ ] Node compromise ≠ key theft (impossible - no keys in node)
- [ ] Wallet isolated from network
- [ ] GUI has no secrets

**3. Production Ready**:
- [ ] Exchanges can run `dinerod` only (no wallet risk)
- [ ] Desktop users can run all three
- [ ] Wallet can be air-gapped (future: hardware wallet support)

**4. Testing**:
- [ ] All tests pass
- [ ] Manual testing complete
- [ ] Security audit ready

---

## 📅 Timeline

**Week 1** (Days 1-5): Create `dinero-walletd`  
**Week 2** (Days 6-10): Clean `dinerod` & update GUI  
**Total**: 2 weeks (10 working days)

---

## 🚦 Current Status: DOCUMENTED (October 4, 2025)

✅ **Analysis complete**  
✅ **Architecture documented**  
✅ **Implementation plan created**  
⏳ **Ready to begin Week 1**

---

## 📚 References

- **Bitcoin Core**: `bitcoind` + `bitcoin-wallet` separation
- **Monero**: `monerod` + `monero-wallet-rpc` separation  
- **Ethereum**: `geth` + external signers (hardware wallets)

**All production cryptocurrencies separate node and wallet. Dinero will too.**

---

**Next Step**: Begin Week 1, Day 1 - Create directory structure

