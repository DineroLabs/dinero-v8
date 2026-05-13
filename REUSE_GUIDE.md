# Duplicates Folder - Code Reuse Guide

**Date**: October 4, 2025  
**Status**: 🔍 **CODE MINING RESOURCE**

---

## 🎯 Purpose

The `duplicates/` folder contains **29 files** with potentially useful code.  
**Before writing new code** → **Check here first** → **Reuse existing logic**

---

## 📊 What's in duplicates/

```
duplicates/
├── consensus/        4 files - Genesis, premine, chain management
├── privacy/          3 files - Coinjoin implementations
├── wallet/           2 files - Wallet descriptors
├── cli/              5 files - CLI implementations, WS client
├── mining/           4 files - Mining engines, GBT work manager
├── storage/          3 files - Database writers, schema managers
├── daemon/           8 files - Main implementations, block acceptor
├── auth/             1 file  - Authentication storage
└── README.md         Documentation

Total: 29 files = ~200KB of potentially reusable code
```

---

## 🔍 Quick Reference: Where to Find What

### **Need Wallet Code?**

**Location**: `duplicates/wallet/` + `duplicates/daemon/main_clean.cpp`

```bash
# Descriptor wallet implementation
duplicates/wallet/descriptor_wallet.cpp.backup

# Simple wallet logic
duplicates/wallet/wallet.cpp.bak

# Alternative main with different wallet structure
duplicates/daemon/main_clean.cpp  # 60KB, different approach
```

**What's useful**:
- Descriptor parsing logic
- Wallet encryption patterns
- Address generation flows
- Balance calculation methods

---

### **Need Mining Code?**

**Location**: `duplicates/mining/` + `duplicates/daemon/`

```bash
# Mining engine implementations
duplicates/mining/mining_engine.cpp.backup
duplicates/mining/mining.cpp.bak

# GetBlockTemplate work manager
duplicates/mining/gbt_work_manager.cpp.backup
duplicates/mining/gbt_work_manager.cpp.bak  # 2 versions!
```

**What's useful**:
- Block template construction
- Mining work distribution
- Hash rate tracking
- Difficulty adjustment logic
- Nonce iteration patterns

**Example to extract**:
```cpp
// From gbt_work_manager.cpp.backup
// Reusable: Block template generation
class GBTWorkManager {
    Json::Value createBlockTemplate(const std::string& mining_address);
    bool validateBlockTemplate(const Json::Value& tmpl);
    // ... more useful methods
};
```

---

### **Need CLI/RPC Client Code?**

**Location**: `duplicates/cli/`

```bash
# Main CLI implementations
duplicates/cli/main_backup.cpp
duplicates/cli/main_new.cpp.bak

# WebSocket client
duplicates/cli/ws_client.cpp.bak

# Node info validation
duplicates/cli/NodeinfoValidator.cpp.bak

# Retry logic
duplicates/cli/retry.cpp.bak
```

**What's useful**:
- Command-line parsing patterns
- RPC client implementations
- WebSocket connection handling
- Retry/reconnection logic
- JSON-RPC error handling

**Example to extract**:
```cpp
// From ws_client.cpp.bak
// Reusable: WebSocket connection with retry
class WebSocketClient {
    bool connect(const std::string& url);
    void onMessage(std::function<void(const std::string&)> callback);
    void reconnect();  // Auto-retry logic
};
```

---

### **Need Consensus/Chain Logic?**

**Location**: `duplicates/consensus/`

```bash
# Genesis block & premine
duplicates/consensus/genesis_premine.cpp.bak
duplicates/consensus/genesis_premine_test.cpp.bak

# Chain manager
duplicates/consensus/chain_manager.cpp.bak

# Commitment system
duplicates/consensus/commitment.cpp.bak
```

**What's useful**:
- Genesis block creation logic
- Premine distribution patterns
- Chain reorganization handling
- Block commitment schemes

---

### **Need Privacy/Coinjoin Code?**

**Location**: `duplicates/privacy/`

```bash
# Coinjoin adapters
duplicates/privacy/coinjoin_adapter_jm.cpp.bak
duplicates/privacy/coinjoin_adapter_generic.cpp.bak

# Coinjoin factory
duplicates/privacy/coinjoin_factory.cpp.bak
```

**What's useful**:
- Coinjoin protocol implementations
- JoinMarket integration patterns
- Generic coinjoin adapters
- Privacy transaction construction

---

### **Need Storage/Database Code?**

**Location**: `duplicates/storage/`

```bash
# Atomic writers
duplicates/storage/atomic_block_writer.cpp.bak

# Schema management
duplicates/storage/schema_manager.cpp.bak

# Backup systems
duplicates/storage/backup_manager.cpp.bak
```

**What's useful**:
- Atomic write patterns (important for crash safety)
- Database schema versioning
- Backup/restore logic
- Transaction rollback handling

---

### **Need Alternative main.cpp Patterns?**

**Location**: `duplicates/daemon/`

```bash
# Complete main (backup)
duplicates/daemon/main.cpp.backup  # Previous version

# Minimal main
duplicates/daemon/main_clean.cpp   # 60KB, cleaner structure

# Basic main
duplicates/daemon/main_simple.cpp  # 8KB, very minimal
```

**Comparison**:

| File | Size | Features | Use Case |
|------|------|----------|----------|
| `main.cpp` (active) | 300KB | Full wallet RPC, complete | Production |
| `main_clean.cpp` | 60KB | Cleaner structure, minimal | Learning, refactoring ideas |
| `main_simple.cpp` | 8KB | Very basic | Testing, prototyping |

**What to reuse from main_clean.cpp**:
```cpp
// Line ~1-100: Clean initialization pattern
bool initialize_services(const Config& config) {
    if (!init_blockchain(config.datadir)) return false;
    if (!init_p2p(config.port)) return false;
    if (!init_rpc(config.rpcport)) return false;
    return true;
}

// Line ~200-300: Signal handling
void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
}

// Line ~400-500: Graceful shutdown
void shutdown_services() {
    g_rpc_server->stop();
    g_p2p_manager->disconnect_all();
    g_blockchain->save_state();
}
```

---

### **Need Block Acceptor Logic?**

**Location**: `duplicates/daemon/`

```bash
duplicates/daemon/block_acceptor.cpp.backup
```

**What's useful**:
- Block validation pipeline
- Orphan block handling
- Header validation
- Merkle root computation

---

### **Need Auth/Security Code?**

**Location**: `duplicates/auth/`

```bash
duplicates/auth/auth_store.cpp.bak
```

**What's useful**:
- RPC authentication patterns
- Cookie generation
- Credential storage
- Security best practices

---

## 🛠️ How to Reuse Code

### **Step 1: Search for Functionality**

```bash
# Example: Looking for block template code
cd duplicates/
grep -r "blocktemplate\|getblocktemplate" . -i

# Example: Looking for wallet encryption
grep -r "encrypt\|argon2\|aes" . -i

# Example: Looking for retry logic
grep -r "retry\|reconnect" . -i
```

### **Step 2: Extract Relevant Functions**

```bash
# Open the file
code duplicates/mining/gbt_work_manager.cpp.backup

# Find the function you need
# Extract just that function
# Copy to active codebase
```

### **Step 3: Adapt and Integrate**

1. **Copy function** from duplicates/ file
2. **Update includes** to match active codebase
3. **Fix namespaces** if needed
4. **Test** the extracted code
5. **Document** where it came from

**Example**:
```cpp
// src/walletd/wallet_encryption.cpp
// Extracted from: duplicates/wallet/wallet.cpp.bak, lines 150-200

bool encryptWalletSeed(const std::vector<uint8_t>& seed, 
                       const std::string& password,
                       std::vector<uint8_t>& encrypted_out) {
    // Original code from wallet.cpp.bak
    // ... implementation ...
}
```

---

## 📋 Reuse Checklist

Before writing new code:

- [ ] **Search duplicates/** for similar functionality
- [ ] **Read relevant .bak files** for implementation ideas
- [ ] **Extract reusable functions** (don't copy entire files)
- [ ] **Adapt to current architecture** (may need refactoring)
- [ ] **Test extracted code** thoroughly
- [ ] **Document source** in comments
- [ ] **Remove from duplicates** if fully integrated (optional)

---

## 🎯 High-Value Extractions

### **Priority 1: Wallet RPC** (For dinero-walletd)

**Source**: `duplicates/daemon/main.cpp.backup` lines 2939-5500

**What to extract**:
- Complete wallet RPC handlers
- HD address generation
- Transaction signing
- Balance calculation

**Destination**: `src/walletd/rpc/wallet_rpc_handlers.cpp`

---

### **Priority 2: Mining Work Manager** (For dinero-miner)

**Source**: `duplicates/mining/gbt_work_manager.cpp.backup`

**What to extract**:
- Block template parsing
- Work distribution logic
- Share validation
- Difficulty calculation

**Destination**: `src/miner/work_manager.cpp`

---

### **Priority 3: WebSocket Client** (For CLI tools)

**Source**: `duplicates/cli/ws_client.cpp.bak`

**What to extract**:
- WebSocket connection handling
- Auto-reconnect logic
- Message parsing
- Error handling

**Destination**: `src/cli/ws_client.cpp`

---

### **Priority 4: Atomic Writer** (For blockchain)

**Source**: `duplicates/storage/atomic_block_writer.cpp.bak`

**What to extract**:
- Atomic write pattern
- Crash safety logic
- Rollback handling

**Destination**: `src/daemon/blockchain_writer.cpp`

---

## 💡 Pattern Library

### **Common Patterns Found in Duplicates**

#### **1. Initialization Pattern** (from main_clean.cpp)

```cpp
// Good: Fail-fast initialization
bool initialize() {
    if (!component1.init()) {
        log_error("Component 1 failed");
        return false;
    }
    if (!component2.init()) {
        log_error("Component 2 failed");
        component1.shutdown();  // Cleanup
        return false;
    }
    return true;
}
```

#### **2. Retry Pattern** (from retry.cpp.bak)

```cpp
// Good: Exponential backoff retry
template<typename Func>
bool retry_with_backoff(Func func, int max_attempts = 5) {
    int delay = 100;  // Start with 100ms
    for (int i = 0; i < max_attempts; i++) {
        if (func()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        delay *= 2;  // Exponential backoff
    }
    return false;
}
```

#### **3. Atomic Write Pattern** (from atomic_block_writer.cpp.bak)

```cpp
// Good: Write to temp, then atomic rename
bool atomic_write(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    
    // Write to temp file
    std::ofstream f(tmp);
    f << data;
    f.close();
    
    // Atomic rename
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}
```

---

## 🔧 Tools for Mining duplicates/

### **1. Smart Search**

```bash
# Function to search duplicates easily
search_dup() {
    grep -r "$1" duplicates/ -i --color=always -n | less -R
}

# Usage:
search_dup "signrawtransaction"
search_dup "getblocktemplate"
search_dup "encrypt.*wallet"
```

### **2. Diff Against Active Code**

```bash
# Compare backup vs active to see what changed
diff -u duplicates/daemon/main.cpp.backup src/daemon/main.cpp | less

# See just the added lines
diff -u duplicates/daemon/main.cpp.backup src/daemon/main.cpp | grep "^+"
```

### **3. Extract Function**

```bash
# Extract a specific function from a file
sed -n '/^bool someFunction/,/^}/p' duplicates/path/file.cpp.bak
```

---

## ⚠️ Important Notes

### **Don't Copy Blindly**

❌ **Bad**: Copy entire file from duplicates/  
✅ **Good**: Extract specific functions/patterns

### **Why Files Were Backed Up**

- **Testing alternatives** - May have been experiments
- **Before refactoring** - Safety backup
- **Different approach** - May have better patterns for some cases

### **Modernize Extracted Code**

Code in duplicates/ may be older. When extracting:
- Update to C++17 features
- Use current error handling patterns
- Match current coding style
- Add modern logging

---

## 📊 Extraction Log

Keep track of what you've reused:

| Source File | Function/Pattern | Destination | Date | Notes |
|-------------|------------------|-------------|------|-------|
| `mining/gbt_work_manager.cpp.backup` | `createBlockTemplate()` | `src/miner/work.cpp` | 2025-10-04 | Adapted for standalone miner |
| `cli/ws_client.cpp.bak` | Reconnect logic | `src/cli/ws_client.cpp` | 2025-10-04 | Added exponential backoff |
| ... | ... | ... | ... | ... |

---

## 🎯 Success Metrics

**Reuse is working when**:
- ✅ Less new code written (reusing proven logic)
- ✅ Faster development (not reinventing)
- ✅ Fewer bugs (reusing tested code)
- ✅ Better patterns (learning from experiments)

---

## 🚀 Next Actions

1. **Before writing new mining code** → Check `duplicates/mining/`
2. **Before writing new wallet code** → Check `duplicates/wallet/` + `main_clean.cpp`
3. **Before writing new CLI code** → Check `duplicates/cli/`
4. **Before writing new storage code** → Check `duplicates/storage/`

**Rule**: 🔍 **Search duplicates/ first, code second**

---

**Remember**: These 29 files represent **months of development work** - mine them wisely! 💎

