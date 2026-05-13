# RPC Handler Migration to DaemonContext

**Status**: 🚧 **IN PROGRESS** (Week 2)
**Date**: 2025-11-06

---

## Overview

This document describes the migration of RPC handlers from legacy globals to context-aware service access via `DaemonContext`.

### Problem with Legacy Pattern

RPC handlers currently access services through global pointers:

```cpp
// OLD PATTERN (legacy globals)
extern ChainDB* g_chain_db_direct;
extern UTXOIndex* g_utxo_set_direct;
extern P2PManager* g_p2p;
extern WalletManager* g_wallet_manager;

din::Json rpc_legacy_getblockcount(const ExecutionContext& ctx, const din::Json& params) {
    // Direct global access ❌
    uint32_t height = dinero::storage::GetChainHeight(g_chain_db_direct);
    return static_cast<int>(height);
}
```

**Problems**:
- Hard to test (requires setting up globals)
- Unclear dependencies
- Global state makes concurrent testing difficult
- No type safety (raw pointers)

### Solution: Context-Aware Pattern

RPC handlers access services through `DaemonContext`:

```cpp
// NEW PATTERN (context-aware)
din::Json rpc_context_getblockcount(const ExecutionContext& ctx, const din::Json& params) {
    // Service access via context ✅
    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    uint32_t height = chainstate->getBlockHeight();
    return static_cast<int>(height);
}
```

**Benefits**:
- Testable with mock services
- Clear dependency tracking via DaemonContext
- Type-safe shared_ptr access
- No global state

---

## Architecture Changes

### 1. Enhanced ExecutionContext (Week 2)

**File**: `include/rpc/rpc_registry.h`

```cpp
struct ExecutionContext {
    std::string walletName;
    std::string user;
    std::string cookie;
    std::string client_id;
    std::unordered_map<std::string, std::string> metadata;

    // Week 2: Access to service layer via DaemonContext
    dinero::DaemonContext* daemon = nullptr;  // ← NEW
};
```

### 2. HttpRpcServer Wiring

**File**: `src/daemon/http_rpc_server.h`

```cpp
class HttpRpcServer {
public:
    // Week 2: Dependency injection for service access
    void set_daemon_context(dinero::DaemonContext* ctx) { daemon_context_ = ctx; }

private:
    dinero::DaemonContext* daemon_context_{nullptr};  // ← NEW
};
```

**File**: `src/daemon/http_rpc_server.cpp:263`

```cpp
ExecutionContext ctx;
ctx.walletName = "";
ctx.user = "";
ctx.cookie = "";
ctx.client_id = "http";
ctx.daemon = daemon_context_;  // ← Inject DaemonContext
```

### 3. Context-Aware RPC Handlers

**Example**: `src/rpc/methods_blockchain_context.cpp`

See the file for full examples of migrated handlers.

---

## Migration Guide

### Step 1: Identify Global Dependencies

**Before migration**, find all globals used in a handler:

```cpp
din::Json rpc_legacy_getmininginfo(const ExecutionContext& ctx, const din::Json& params) {
    // Globals used:
    extern ChainDB* g_chain_db_direct;      // → ctx.daemon->chainstate->chainDB()
    extern TxPool* g_tx_pool;                // → ctx.daemon->mempool->txPool()
    extern Config* g_config;                 // → ctx.daemon->config
    // ... convert each one ...
}
```

### Step 2: Access Services via Context

**Common conversions**:

| Legacy Global | Context-Aware Access |
|--------------|---------------------|
| `g_chain_db_direct` | `ctx.daemon->chainstate->chainDB()` |
| `g_utxo_set_direct` | `ctx.daemon->chainstate->utxoIndex()` |
| `g_blockchain` | `ctx.daemon->chainstate->blockchain()` |
| `g_wallet_manager` | `ctx.daemon->wallet->get()` |
| `g_p2p` | `ctx.daemon->p2p->get()` |
| `g_tx_pool` | `ctx.daemon->mempool->txPool()` |
| `g_logger` | `ctx.daemon->logger` |
| `g_config` | `ctx.daemon->config` |

### Step 3: Add Null Checks

Always check if services are available:

```cpp
din::Json rpc_context_method(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    // Check context
    if (!ctx.daemon) {
        result["error"] = "DaemonContext not available";
        return result;
    }

    // Check service
    if (!ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    // Cast to concrete service type
    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    // Now safe to use
    uint32_t height = chainstate->getBlockHeight();
    // ...
}
```

### Step 4: Register with Overwrite Mode

Replace legacy handlers using `RegisterMode::Overwrite`:

```cpp
void registerBlockchainMethodsContext() {
    extern RpcRegistry g_rpcRegistry;

    // Overwrite legacy handler with context-aware version
    g_rpcRegistry.registerHandler("blockchain.getblockcount",
                                 rpc_context_getblockcount,
                                 RegisterMode::Overwrite,
                                 "context-aware");
}
```

### Step 5: Test Thoroughly

1. Test each migrated method individually
2. Verify error handling (null checks work)
3. Compare output with legacy version
4. Test in regtest, testnet, and mainnet modes

---

## Migration Status

### ✅ Phase 1: Infrastructure (Complete)
- [x] Add `daemon` pointer to `ExecutionContext`
- [x] Wire `DaemonContext` through `HttpRpcServer`
- [x] Create example context-aware handlers
- [x] Document migration pattern

### 🚧 Phase 2: Handler Migration (In Progress)
- [x] Blockchain methods (4 handlers migrated)
- [ ] Wallet methods
- [ ] Mining methods
- [ ] Network methods
- [ ] Mempool methods
- [ ] Bridge methods
- [ ] Contract methods
- [ ] All other namespaces

### ⏳ Phase 3: Cleanup (Pending)
- [ ] Remove legacy method registrations
- [ ] Delete legacy handler files
- [ ] Remove global variable declarations
- [ ] Update tests to use context

---

## Example: Complete Migration

### Before (Legacy)

```cpp
// src/rpc/methods_blockchain_legacy.cpp
extern ChainDB* g_chain_db_direct;

din::Json rpc_legacy_getblockcount(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;
    result = static_cast<int>(dinero::storage::GetChainHeight(g_chain_db_direct));
    return result;
}

void registerBlockchainMethods() {
    extern RpcRegistry g_rpcRegistry;
    g_rpcRegistry.registerHandler("blockchain.getblockcount", rpc_legacy_getblockcount);
}
```

### After (Context-Aware)

```cpp
// src/rpc/methods_blockchain_context.cpp
din::Json rpc_context_getblockcount(const ExecutionContext& ctx, const din::Json& params) {
    din::Json result;

    if (!ctx.daemon || !ctx.daemon->chainstate) {
        result["error"] = "Chainstate service not available";
        return result;
    }

    auto chainstate = std::dynamic_pointer_cast<dinero::ChainstateService>(ctx.daemon->chainstate);
    if (!chainstate) {
        result["error"] = "Failed to cast chainstate service";
        return result;
    }

    uint32_t height = chainstate->getBlockHeight();
    result = static_cast<int>(height);
    return result;
}

void registerBlockchainMethodsContext() {
    extern RpcRegistry g_rpcRegistry;
    g_rpcRegistry.registerHandler("blockchain.getblockcount",
                                 rpc_context_getblockcount,
                                 RegisterMode::Overwrite,
                                 "context-aware");
}
```

**Changes**:
1. No global variable dependency
2. Service accessed via `ctx.daemon->chainstate`
3. Null checks for safety
4. Registered with `Overwrite` mode to replace legacy

---

## Testing Context-Aware Handlers

### Manual Testing

```bash
# Start daemon with context-aware handlers
./build/dinerod --regtest --datadir=/tmp/context-test

# Test blockchain methods
./build/dinero-cli -rpcport=20998 blockchain.getblockcount
./build/dinero-cli -rpcport=20998 blockchain.getblockchaininfo
```

### Unit Testing (Future)

```cpp
// Test with mock DaemonContext
TEST(RpcHandlers, GetBlockCountWithMockContext) {
    // Create mock context
    dinero::DaemonContext mock_ctx;
    auto mock_chainstate = std::make_shared<MockChainstateService>();
    mock_chainstate->setHeight(100);
    mock_ctx.chainstate = mock_chainstate;

    // Create ExecutionContext
    ExecutionContext exec_ctx;
    exec_ctx.daemon = &mock_ctx;

    // Call handler
    din::Json result = rpc_context_getblockcount(exec_ctx, din::arr());

    // Verify
    EXPECT_EQ(result.as<int>(), 100);
}
```

---

## Week 2+ Plan

### Week 2: Core Namespaces
- Migrate blockchain.* methods
- Migrate wallet.* methods
- Migrate mining.* methods
- Migrate mempool.* methods

### Week 3: Extended Namespaces
- Migrate bridge.* methods
- Migrate contract.* methods
- Migrate payment.* methods
- Migrate p2p.* methods

### Week 4: Specialized Namespaces
- Migrate multiasset.* methods
- Migrate hwallet.* methods
- Migrate market.* methods
- All remaining namespaces

### Week 5: Cleanup
- Remove all legacy handler files
- Remove global variable stubs
- Update all tests
- Production-ready context-aware RPC

---

## Next Steps

1. **Complete blockchain namespace migration** (4 methods done, ~10 remaining)
2. **Add CMakeLists.txt entry** for `methods_blockchain_context.cpp`
3. **Call `registerBlockchainMethodsContext()`** in daemon startup
4. **Wire `set_daemon_context()`** in RPCService or DaemonApp
5. **Test migrated methods** to verify they work
6. **Migrate next namespace** (wallet, mining, or mempool)

---

*End of RPC Context Migration Guide*
