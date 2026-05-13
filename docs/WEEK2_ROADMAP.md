# Week 2 Roadmap: From Bridge to Pure Service Architecture

## Current Status ✅

- ✅ **Week 1 Complete**: Bridge architecture working
- ✅ **Binary Built**: 48MB dinerod executable
- ✅ **Services Running**: All 9 services initialize and start
- ✅ **Bridge Working**: Legacy globals point to real service instances
- ✅ **Daemon Runs**: Clean startup with proper logging

## Week 2 Goals

### Phase 1: Testing & Validation (Days 1-2)

#### A. Test All Services
```bash
# 1. Test RPC Server
./build/dinero-cli -datadir=/tmp/test getblockcount
./build/dinero-cli -datadir=/tmp/test getinfo
./build/dinero-cli -datadir=/tmp/test rpc.listmethods

# 2. Test P2P Networking
./build/dinero-cli -datadir=/tmp/test getpeerinfo
./build/dinero-cli -datadir=/tmp/test addnode "127.0.0.1:21001" "add"

# 3. Test Mining
./build/dinero-cli -datadir=/tmp/test mining.start "din1q..."
./build/dinero-cli -datadir=/tmp/test mining.info

# 4. Test Wallet
./build/dinero-cli -datadir=/tmp/test wallet.create "test"
./build/dinero-cli -datadir=/tmp/test wallet.getbalance
```

#### B. Verify Bridge Pattern
```bash
# Check that old code using globals works
grep -r "g_chain_db_direct->" src/rpc/ --include="*.cpp"
grep -r "g_wallet_manager->" src/rpc/ --include="*.cpp"
grep -r "g_p2p->" src/daemon/ --include="*.cpp"

# Verify services set globals during Init()
# Look for log lines: "Legacy global ... → real instance"
```

#### C. Check for Issues
- [ ] Memory leaks (valgrind/instruments)
- [ ] Null pointer crashes
- [ ] Service initialization failures
- [ ] Shutdown cleanup issues

### Phase 2: Begin Migration (Days 3-4)

#### Goal: Start replacing global access with DaemonContext access

#### Strategy: Migrate One RPC Handler at a Time

**Example Migration:**

```cpp
// BEFORE (Week 1 - using globals):
Json::Value handle_getblockcount(const Json::Value& params) {
    if (!g_chain_db_direct) {
        throw std::runtime_error("ChainDB not initialized");
    }
    uint32_t height = g_chain_db_direct->GetHeight();
    return Json::Value(height);
}

// AFTER (Week 2 - using DaemonContext):
Json::Value handle_getblockcount(const Json::Value& params, DaemonContext& ctx) {
    auto chainstate = ctx.chainstate;
    if (!chainstate) {
        throw std::runtime_error("Chainstate service not available");
    }
    uint32_t height = chainstate->GetHeight();
    return Json::Value(height);
}
```

#### Migration Checklist

**RPC Handlers (High Priority)**
- [ ] `src/rpc/methods_blockchain.cpp` - Use `ctx.chainstate`
- [ ] `src/rpc/methods_wallet.cpp` - Use `ctx.wallet`
- [ ] `src/rpc/methods_network.cpp` - Use `ctx.p2p`
- [ ] `src/rpc/methods_mining.cpp` - Use `ctx.mining`

**Mining Code (Medium Priority)**
- [ ] `src/daemon/mining.cpp` - Use `ctx.chainstate` instead of global blockchain

**P2P Code (Medium Priority)**
- [ ] `src/daemon/block_acceptor.cpp` - Use `ctx.p2p` instead of `g_p2p`

**Wallet Code (Low Priority)**
- [ ] Internal wallet code mostly already isolated

#### How to Add DaemonContext to RPC Handlers

**Step 1: Update RPC method signature**
```cpp
// Add DaemonContext& parameter to all handlers
using RpcHandler = std::function<Json::Value(const Json::Value&, DaemonContext&)>;
```

**Step 2: Pass context through RPC server**
```cpp
// In HttpRpcServer or RpcRegistry:
class RpcRegistry {
    DaemonContext& ctx_;  // Store reference
public:
    RpcRegistry(DaemonContext& ctx) : ctx_(ctx) {}

    Json::Value call(const std::string& method, const Json::Value& params) {
        auto handler = lookup(method);
        return handler(params, ctx_);  // Pass context
    }
};
```

**Step 3: RPCService passes its context**
```cpp
bool RPCService::Init(DaemonContext& ctx) {
    rpc_registry_ = std::make_unique<RpcRegistry>(ctx);  // Pass context
    // Register handlers...
}
```

### Phase 3: Service Accessor Methods (Day 5)

Some services need accessor methods to expose their instances:

#### ChainstateService
```cpp
class ChainstateService : public IService {
    std::unique_ptr<Blockchain> blockchain_;
    std::unique_ptr<ChainDB> chain_db_;
    std::unique_ptr<UTXOIndex> utxo_index_;
public:
    // Add accessors
    Blockchain& GetBlockchain() { return *blockchain_; }
    ChainDB& GetChainDB() { return *chain_db_; }
    UTXOIndex& GetUTXOIndex() { return *utxo_index_; }
};
```

#### P2PService
```cpp
class P2PService : public IService {
    std::unique_ptr<P2PManager> p2p_mgr_;
public:
    // Add accessor
    P2PManager& GetP2PManager() { return *p2p_mgr_; }
};
```

### Phase 4: Remove Legacy Globals (Day 6-7)

Once all code is migrated:

#### Step 1: Remove global assignments from services
```cpp
// REMOVE from ChainstateService::Init():
// extern ChainDB* g_chain_db_direct;
// g_chain_db_direct = chain_db_.get();

// REMOVE from P2PService::Init():
// extern P2PManager* dinero::g_p2p;
// dinero::g_p2p = p2p_mgr_.get();
```

#### Step 2: Remove legacy_globals_stub.cpp
```bash
# Remove from CMakeLists.txt
- src/daemon/legacy_globals_stub.cpp

# Delete the file
rm src/daemon/legacy_globals_stub.cpp
```

#### Step 3: Remove extern declarations
```bash
# Find all extern declarations for legacy globals
grep -r "extern.*g_chain_db_direct" src/
grep -r "extern.*g_wallet_manager" src/
grep -r "extern.*g_p2p" src/

# Remove them all
```

#### Step 4: Verify nothing breaks
```bash
# Full rebuild
rm -rf build && cmake -B build && cmake --build build

# Test everything
./run_tests.sh
./build/dinerod --regtest --datadir=/tmp/test
```

## Week 2 Success Criteria

- [ ] All RPC commands work using DaemonContext
- [ ] All mining operations work using DaemonContext
- [ ] All P2P operations work using DaemonContext
- [ ] No code references legacy globals
- [ ] legacy_globals_stub.cpp deleted
- [ ] All tests pass
- [ ] Daemon runs stably for 24+ hours

## Optional: Week 3 Goals (If Week 2 Completes Early)

### Remove Singleton Pattern Entirely

Many classes still have `instance()` methods even though they're created by services:

```cpp
// BEFORE (Singleton pattern):
class EventBus {
    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }
};

// AFTER (Pure dependency injection):
class EventBus {
    // No instance() method
    // Created by service, passed via DaemonContext
};
```

**Classes to Update:**
- EventBus
- FiatBridgeManager
- MarketplaceManager
- EscrowManager
- TokenManager
- SessionManager
- ContractRegistry
- ARPManager
- KYCManager
- PaymentAdapter

## Tips for Migration

### 1. **One File at a Time**
Don't try to migrate everything at once. Pick one RPC handler file, migrate it, test it, commit it.

### 2. **Keep Bridge While Migrating**
Don't remove legacy_globals_stub.cpp until EVERYTHING is migrated. The bridge lets old and new code coexist.

### 3. **Use Compiler Errors as Guide**
Once you remove a global, the compiler will tell you every place that used it. Fix them one by one.

### 4. **Write Migration Tests**
Before migrating a component, write a test that uses it. After migration, the test should still pass.

### 5. **Document As You Go**
Add comments explaining why you changed from globals to context:
```cpp
// MIGRATED Week 2: Now uses ctx.chainstate instead of g_chain_db_direct
uint32_t height = ctx.chainstate->GetHeight();
```

## Current File Structure

```
src/daemon/
├── main.cpp                     (113 lines, clean ✅)
├── daemon_app.cpp               (Creates services ✅)
├── daemon_context.h             (DI container ✅)
├── legacy_globals_stub.cpp      (BRIDGE - remove Week 2)
├── services/
│   ├── config_service.cpp       (✅ Working)
│   ├── chainstate_service.cpp   (✅ Sets g_chain_db_direct)
│   ├── wallet_service.cpp       (✅ Sets g_wallet_manager)
│   ├── mempool_service.cpp      (✅ Working)
│   ├── p2p_service.cpp          (✅ Sets g_p2p)
│   ├── mining_service.cpp       (✅ Working)
│   ├── rpc_service.cpp          (✅ Working)
│   └── metrics_service.cpp      (✅ Working)
```

## Key Principle

> **The bridge is temporary scaffolding, not the final architecture.**
>
> Week 1: Build the bridge (services set globals)
> Week 2: Cross the bridge (migrate to DaemonContext)
> Week 3: Remove the bridge (delete legacy_globals_stub.cpp)

**We're currently at the end of Week 1. Week 2 starts now!** 🚀
