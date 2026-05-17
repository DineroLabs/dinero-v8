# Dinero Clean Architecture - Complete Separation

**Date**: October 4, 2025  
**Status**: 🎯 **PRODUCTION ARCHITECTURE DEFINED**  
**Priority**: Critical for mainnet

---

## 🏗️ Five-Component Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      dinero-qt (GUI)                         │
│  - Pure UI, NO business logic                               │
│  - Spawns & controls external processes                     │
│  - NO Qt linked into node/wallet/miner                      │
└────┬──────────────┬──────────────┬─────────────┬────────────┘
     │ RPC :20999   │ RPC :20998   │ RPC :20997  │ Process
     │ (wallet)     │ (node)       │ (mining)    │ Control
     ▼              ▼              ▼             ▼
┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐
│ walletd  │  │ dinerod  │  │  miner   │  │  stratum   │
│ (keys)   │  │ (chain)  │  │  (CPU)   │  │ (optional) │
└──────────┘  └──────────┘  └──────────┘  └────────────┘
     ▲              ▲              │             ▲
     │              └──────────────┘             │
     │         RPC: submitblock                  │
     │                                           │
     └───────────────────────────────────────────┘
              RPC: getblocktemplate
```

---

## 📦 Component Definitions

### **1. dinerod (Headless Node)** 🖥️

**Purpose**: Blockchain consensus and P2P network  
**Port**: 20998 (RPC), 20999 (P2P)

**What it DOES**:
- ✅ Blockchain storage & validation
- ✅ UTXO set management
- ✅ P2P network (block/tx relay)
- ✅ Mempool
- ✅ Block validation
- ✅ RPC server (blockchain queries)

**What it NEVER does**:
- ❌ Wallet (no keys, no signing)
- ❌ Mining (no block generation)
- ❌ GUI (no Qt, no graphics)
- ❌ Stratum (no pool protocol)

**RPC Methods**:
```
# Blockchain queries
getblock, getblockchaininfo, getbestblockhash

# Transaction broadcast (NOT signing)
sendrawtransaction

# Network
getpeerinfo, addnode, getnetworkinfo

# UTXO queries (for ANY address, not wallet-specific)
listunspent, gettxout

# Mining coordination
getblocktemplate, submitblock
```

**Dependencies**:
```cmake
target_link_libraries(dinerod PRIVATE
  dinero_consensus    # Block validation, UTXO
  dinero_network      # P2P
  dinero_crypto       # SHA256, RIPEMD160
  # ❌ NO dinero_wallet
  # ❌ NO dinero_miner
  # ❌ NO Qt
)
```

**Build**:
```bash
add_executable(dinerod
  src/daemon/main.cpp
  src/daemon/simple_blockchain.cpp
  src/daemon/p2p_manager.cpp
  src/daemon/transaction_pool.cpp
  src/daemon/http_rpc_server.cpp
  # ❌ NO wallet files
  # ❌ NO mining files
)
```

---

### **2. dinero-walletd (Wallet Service)** 🔑

**Purpose**: Key management and transaction signing  
**Port**: 20997 (RPC)

**What it DOES**:
- ✅ BIP39 mnemonic generation/restoration
- ✅ HD key derivation (BIP32/84)
- ✅ Address generation
- ✅ Private key storage (encrypted)
- ✅ Transaction signing
- ✅ Balance tracking (queries dinerod)

**What it NEVER does**:
- ❌ Blockchain storage
- ❌ P2P networking
- ❌ Block validation
- ❌ Mining
- ❌ GUI (no Qt)

**RPC Methods**:
```
# Wallet management
createwallet, loadwallet, encryptwallet, walletpassphrase

# Address generation
getnewaddress, getaddressinfo, listaddresses

# Transaction operations
createrawtransaction, signrawtransactionwithwallet, sendtoaddress

# Balance & history
getbalance, listtransactions, listunspent (wallet-specific)
```

**Dependencies**:
```cmake
target_link_libraries(dinero-walletd PRIVATE
  dinero_wallet       # BIP39, HD, crypto
  dinero_crypto
  # ❌ NO dinero_consensus
  # ❌ NO dinero_network
  # ❌ NO dinero_miner
  # ❌ NO Qt
)
```

**Communication**:
- **Queries dinerod** via RPC for blockchain data (UTXOs, tx broadcast)
- **Never stores blockchain** - stateless except for keys

---

### **3. dinero-miner (Standalone CLI Miner)** ⛏️

**Purpose**: CPU mining client  
**Port**: None (pure client, talks RPC to dinerod)

**What it DOES**:
- ✅ Get block template from dinerod
- ✅ Hash computation (SHA256d)
- ✅ Submit solved blocks to dinerod
- ✅ Progress reporting
- ✅ Multi-threaded mining

**What it NEVER does**:
- ❌ Blockchain storage
- ❌ P2P networking
- ❌ Wallet operations
- ❌ Block validation (dinerod does this)
- ❌ GUI (pure CLI)

**Usage**:
```bash
# Standalone mining
./dinero-miner \
  --url http://127.0.0.1:20998 \
  --user $(cat .cookie) \
  --threads 8 \
  --address din1qmining...

# Or launched by GUI
dinero-qt spawns: dinero-miner --url ... --threads 4
```

**Dependencies**:
```cmake
target_link_libraries(dinero-miner PRIVATE
  dinero_crypto       # SHA256 only
  # ❌ NO dinero_consensus
  # ❌ NO dinero_network
  # ❌ NO dinero_wallet
  # ❌ NO Qt
)
```

**Key Point**: **EXTERNAL PROCESS** - GUI never embeds mining code

---

### **4. dinero-stratum (Optional Pool Server)** 🏊

**Purpose**: Stratum mining pool server  
**Port**: 3333 (Stratum), 20998 (dinerod RPC)  
**Platform**: Ubuntu/Linux servers

**What it DOES**:
- ✅ Stratum v1 protocol
- ✅ Share validation
- ✅ Difficulty adjustment
- ✅ Payout tracking
- ✅ Connects to dinerod for block submission

**What it NEVER does**:
- ❌ Blockchain storage (queries dinerod)
- ❌ Wallet operations (pool uses walletd separately)
- ❌ GUI

**Usage**:
```bash
# Mining pool setup
./dinero-stratum \
  --port 3333 \
  --dinerod http://127.0.0.1:20998 \
  --payout-address din1qpool...

# Miners connect with standard tools:
cpuminer -a sha256d -o stratum+tcp://pool.dinero.com:3333 -u worker1
xmrig --url pool.dinero.com:3333
```

**Dependencies**:
```cmake
target_link_libraries(dinero-stratum PRIVATE
  dinero_stratum      # Stratum protocol
  dinero_crypto
  # ❌ NO Qt
  # ❌ NO wallet (separate walletd for payouts)
)
```

**Optional**: Only built for server deployments

---

### **5. dinero-qt (Pure GUI)** 🖥️

**Purpose**: User interface only  
**Port**: None (spawns other processes)

**What it DOES**:
- ✅ Display blockchain data (from dinerod RPC)
- ✅ Display wallet data (from walletd RPC)
- ✅ Spawn & monitor miners
- ✅ User input & interaction
- ✅ Process management

**What it NEVER does**:
- ❌ Blockchain storage (queries dinerod)
- ❌ Key management (queries walletd)
- ❌ Mining computation (spawns miner)
- ❌ Block validation (dinerod does this)

**Process Management**:
```cpp
// gui/src/processmanager.cpp

class ProcessManager {
public:
    // Spawn external processes
    void startNode(const QString& datadir);
    void startWallet(const QString& datadir);
    void startMiner(int threads, const QString& address);
    
    // Monitor & control
    bool isNodeRunning() const;
    bool isWalletRunning() const;
    bool isMinerRunning() const;
    
    void stopAll();
    
private:
    QProcess* nodeProcess_;
    QProcess* walletProcess_;
    QProcess* minerProcess_;
    
    RpcClient* nodeRpc_;    // Talk to dinerod
    RpcClient* walletRpc_;  // Talk to walletd
};
```

**Startup Flow**:
```cpp
// main.cpp (GUI)
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    
    ProcessManager pm;
    
    // Start backend services
    pm.startNode(datadir + "/node");      // Spawns dinerod
    pm.startWallet(datadir + "/wallet");  // Spawns dinero-walletd
    
    // Wait for services to be ready
    while (!pm.isNodeRunning() || !pm.isWalletRunning()) {
        QThread::msleep(100);
    }
    
    // Create GUI (pure Qt, no business logic)
    MainWindow w(&pm);
    w.show();
    
    return app.exec();
}
```

**Dependencies**:
```cmake
target_link_libraries(dinero-qt PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::Network    # For RPC client only
  # ❌ NO dinero_consensus
  # ❌ NO dinero_wallet
  # ❌ NO dinero_miner
  # ❌ Pure Qt application
)
```

---

## 🎯 Benefits of Clean Separation

### **1. Security** 🔒

**Problem (Current)**:
```
Attacker compromises dinerod → Gets keys + blockchain + network
```

**Solution (Separated)**:
```
Attacker compromises dinerod → Gets blockchain only (no keys)
Attacker compromises walletd → Gets keys only (no network, isolated)
Attacker compromises miner → Gets nothing (no keys, no blockchain)
```

### **2. Deployment Flexibility** 🚀

**Desktop User**:
```bash
# All-in-one
dinero-qt   # Spawns dinerod + walletd + miner
```

**Exchange/Service**:
```bash
# Node only (no wallet risk)
dinerod --datadir /var/lib/dinero

# Wallet in separate secure environment
dinero-walletd --datadir /secure/wallet --dinerod http://node:20998
```

**Mining Pool**:
```bash
# Pool server
dinero-stratum --port 3333 --dinerod http://node:20998

# Miners connect
cpuminer -o stratum+tcp://pool:3333
```

### **3. Build Optimization** ⚡

```bash
# Headless server (minimal dependencies)
cmake -DBUILD_GUI=OFF -DBUILD_MINER=OFF
make dinerod           # Node only

# Desktop (full stack)
cmake -DBUILD_GUI=ON -DBUILD_MINER=ON
make all              # Everything

# Mining-only server
cmake -DBUILD_GUI=OFF -DBUILD_NODE=OFF
make dinero-miner     # Miner only
```

### **4. Testing Isolation** 🧪

```bash
# Test node without wallet
./dinerod --regtest
curl http://127.0.0.1:20998 ...

# Test wallet without node (mock RPC)
./dinero-walletd --dinerod http://mock:20998

# Test miner without network
./dinero-miner --url http://testnet:20998
```

---

## 📋 Migration Checklist

### **Phase 1: Extract Components** (Week 1)

- [ ] **Create dinero-walletd**
  - [ ] Move wallet files from daemon/ to walletd/
  - [ ] Create wallet RPC server
  - [ ] Remove wallet from dinerod

- [ ] **Create dinero-miner**
  - [ ] Extract mining from daemon/mining.cpp
  - [ ] Standalone CLI with getblocktemplate
  - [ ] Remove mining from dinerod

- [ ] **Update dinero-qt**
  - [ ] Add ProcessManager
  - [ ] Spawn dinerod as external process
  - [ ] Spawn dinero-walletd as external process
  - [ ] Spawn dinero-miner as external process

### **Phase 2: Clean Dependencies** (Week 2)

- [ ] **dinerod CMake cleanup**
  - [ ] Remove dinero_wallet link
  - [ ] Remove dinero_miner link
  - [ ] Remove Qt link
  - [ ] Verify: nm dinerod | grep -i "wallet\|mining\|qt" → empty

- [ ] **dinero-walletd CMake**
  - [ ] Link dinero_wallet only
  - [ ] No consensus, no network, no Qt

- [ ] **dinero-miner CMake**
  - [ ] Link dinero_crypto only
  - [ ] No consensus, no wallet, no Qt

- [ ] **dinero-qt CMake**
  - [ ] Link Qt only
  - [ ] No consensus, no wallet, no miner

### **Phase 3: Testing** (Days 11-14)

- [ ] **Unit tests**
  - [ ] dinerod: blockchain validation
  - [ ] walletd: key derivation, signing
  - [ ] miner: hash computation

- [ ] **Integration tests**
  - [ ] GUI spawns all processes
  - [ ] Wallet queries node for UTXOs
  - [ ] Miner submits blocks to node
  - [ ] All RPC communications work

- [ ] **Security audit**
  - [ ] dinerod has no wallet symbols
  - [ ] walletd isolated from network
  - [ ] miner has no sensitive data

---

## 📁 Final Project Structure

```
src/
├── daemon/           # dinerod (node only)
│   ├── main.cpp
│   ├── simple_blockchain.cpp
│   ├── p2p_manager.cpp
│   └── http_rpc_server.cpp
│
├── walletd/          # dinero-walletd (wallet service)
│   ├── main.cpp
│   ├── wallet_service.cpp
│   ├── hd_wallet_manager.cpp
│   └── wallet_rpc_server.cpp
│
├── miner/            # dinero-miner (CPU miner)
│   ├── main.cpp
│   ├── mining_core.cpp
│   └── stratum_client.cpp (optional)
│
├── stratum/          # dinero-stratum (pool server, optional)
│   ├── main.cpp
│   ├── stratum_server.cpp
│   └── share_validator.cpp
│
└── gui/              # dinero-qt (pure UI)
    ├── main.cpp
    ├── mainwindow.cpp
    └── processmanager.cpp
```

---

## 🎯 Success Criteria

**Separation Complete When**:

1. **Binary Independence**:
   ```bash
   otool -L dinerod | grep -i "wallet\|mining\|qt" → EMPTY ✅
   otool -L dinero-walletd | grep -i "consensus\|network\|qt" → EMPTY ✅
   otool -L dinero-miner | grep -i "wallet\|consensus\|qt" → EMPTY ✅
   ```

2. **Deployment Scenarios Work**:
   - ✅ Exchange runs dinerod alone (no wallet risk)
   - ✅ Desktop runs all three (dinero-qt spawns others)
   - ✅ Pool runs dinerod + dinero-stratum
   - ✅ Remote miner runs dinero-miner only

3. **Security Validated**:
   - ✅ Node compromise ≠ key theft (impossible)
   - ✅ Wallet isolated from network
   - ✅ Miner has no sensitive data

4. **Build Matrix**:
   ```bash
   # Headless server
   cmake -DBUILD_GUI=OFF
   make dinerod dinero-walletd dinero-miner

   # Desktop
   cmake -DBUILD_GUI=ON
   make all

   # Pool server
   cmake -DBUILD_STRATUM=ON
   make dinerod dinero-stratum
   ```

---

## 📚 References

**Bitcoin Core**:
- `bitcoind` = node only
- `bitcoin-wallet` = separate tool
- `bitcoin-qt` = GUI spawns bitcoind

**Monero**:
- `monerod` = node only
- `monero-wallet-rpc` = separate service
- `monero-wallet-gui` = spawns both

**Ethereum**:
- `geth` = node only
- External signers (Metamask, hardware wallets)
- Never embeds wallet in node

**Dinero follows industry best practices.**

---

## ⏱️ Timeline

**Total**: 2 weeks

**Week 1**: Extract components (walletd, miner, qt process mgmt)  
**Week 2**: Clean dependencies, testing, documentation

**Target**: Complete before mainnet beta (Q1 2026)

---

## 🚀 Next Steps

1. ✅ Run `move_duplicates.sh` (cleanup)
2. ✅ Create `src/walletd/` directory structure
3. ✅ Create `src/miner/` directory structure
4. ✅ Extract wallet from daemon
5. ✅ Extract mining from daemon
6. ✅ Update GUI to spawn processes
7. ✅ Test separated architecture

**Let's build a production-grade cryptocurrency!** 🚀

