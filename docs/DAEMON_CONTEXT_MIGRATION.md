# Daemon Context Migration Guide

## Problem Statement

The daemon was crashing with `mutex lock failed: Invalid argument` due to **static initialization order fiasco**.

Singletons with mutex members were being accessed before their mutexes were constructed, causing undefined behavior.

## Solution: Context-Driven Architecture

Replace "web of singletons" with dependency injection via `DaemonContext`.

---

## Architecture Overview

### Before (Singletons)
```cpp
// Scattered throughout codebase:
auto& wallet = WalletManager::instance();  // ❌ Hidden dependencies
auto& bus = EventBus::instance();          // ❌ Static init order bug
auto& p2p = P2PManager::instance();        // ❌ Hard to test
```

### After (Context-Driven)
```cpp
// Single source of truth:
DaemonContext ctx;
ctx.wallet = std::make_shared<WalletManager>();
ctx.event_bus = std::make_shared<EventBus>();
ctx.p2p = std::make_shared<P2PManager>();

// Explicit dependencies:
ctx.p2p->Init(ctx);  // ✅ Can access ctx.chainstate, ctx.logger, etc.
```

---

## Key Components

### 1. IService Interface
```cpp
class IService {
    virtual std::string Name() const = 0;
    virtual bool Init(DaemonContext& ctx) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
};
```

Every subsystem implements this interface.

### 2. DaemonContext
```cpp
struct DaemonContext {
    std::shared_ptr<Logger> logger;
    std::shared_ptr<Config> config;
    std::shared_ptr<Chainstate> chainstate;
    std::shared_ptr<WalletManager> wallet;
    std::shared_ptr<P2PManager> p2p;
    std::shared_ptr<RPCServer> rpc;
    // ... etc
};
```

All services live here instead of as globals.

### 3. DaemonApp
```cpp
class DaemonApp {
    bool Init();   // Create & initialize services
    bool Start();  // Start services in order
    void Stop();   // Stop services in reverse order
};
```

Controls deterministic lifecycle.

---

## Migration Plan (Incremental)

### Phase 1: Foundation (Week 1)
- [x] Create `IService` interface
- [x] Create `DaemonContext` struct
- [x] Create `DaemonApp` skeleton
- [ ] Implement `Logger` as first `IService`
- [ ] Implement `Config` as second `IService`

### Phase 2: Core Services (Week 2)
- [ ] Migrate `Chainstate` to `IService`
- [ ] Migrate `Mempool` to `IService`
- [ ] Migrate `WalletManager` to `IService`
- [ ] Update RPC handlers to use `ctx.wallet` instead of `WalletManager::instance()`

### Phase 3: Networking (Week 3)
- [ ] Migrate `P2PManager` to `IService`
- [ ] Migrate `RPCServer` to `IService`
- [ ] Fix all `instance()` calls in P2P code

### Phase 4: Optional Modules (Week 4)
- [ ] Migrate `EventBus` to `IService`
- [ ] Migrate `FiatBridgeManager` to `IService`
- [ ] Migrate `MarketplaceManager` to `IService`
- [ ] Migrate `EscrowManager` to `IService`

### Phase 5: Cleanup (Week 5)
- [ ] Remove all `::instance()` methods
- [ ] Remove all static mutexes
- [ ] Add unit tests using mock contexts
- [ ] Document new patterns

---

## Example: Migrating a Service

### Before (Singleton)
```cpp
// wallet_manager.h
class WalletManager {
public:
    static WalletManager& instance() {
        static WalletManager inst;
        return inst;
    }
private:
    WalletManager() = default;
    std::mutex mutex_;  // ❌ Static init order bug
};

// Usage:
auto& wallet = WalletManager::instance();
```

### After (IService)
```cpp
// wallet_manager.h
class WalletManager : public IService {
public:
    WalletManager() = default;

    std::string Name() const override { return "WalletManager"; }

    bool Init(DaemonContext& ctx) override {
        logger_ = ctx.logger;
        chainstate_ = ctx.chainstate;
        return true;
    }

    bool Start() override {
        // Open database, load wallets
        return true;
    }

    void Stop() override {
        // Flush, close DB
    }

private:
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<Chainstate> chainstate_;

    // No more static mutex - safe to use
    std::mutex mutex_;
};

// Usage:
auto& wallet = *ctx.wallet;
```

---

## Benefits

### ✅ No More Static Init Bugs
All services constructed in `DaemonApp::Init()` in deterministic order.

### ✅ Testability
```cpp
TEST(RPCServer, GetBalance) {
    DaemonContext ctx;
    ctx.wallet = std::make_shared<MockWallet>();
    ctx.chainstate = std::make_shared<MockChainstate>();

    RPCServer rpc;
    rpc.Init(ctx);
    // Test RPC calls with mocks
}
```

### ✅ Clear Dependencies
No hidden global state - every dependency is explicit in `Init()`.

### ✅ Graceful Shutdown
Services stopped in reverse initialization order automatically.

### ✅ Plugin-Ready
New modules can register themselves:
```cpp
ctx.custom_module = std::make_shared<MyCustomService>();
services_.push_back(ctx.custom_module);
```

---

## Compatibility Layer (Temporary)

During migration, keep old singletons but forward to context:

```cpp
// wallet_manager.cpp (temporary bridge)
static DaemonContext* g_ctx = nullptr;

WalletManager& WalletManager::instance() {
    if (!g_ctx) throw std::runtime_error("Context not initialized");
    return *g_ctx->wallet;
}

void WalletManager::SetContext(DaemonContext* ctx) {
    g_ctx = ctx;
}
```

This allows incremental migration without breaking existing code.

---

## Timeline

- **Week 1**: Foundation classes
- **Week 2**: Core services (chainstate, wallet, mempool)
- **Week 3**: Networking (P2P, RPC)
- **Week 4**: Optional modules (marketplace, bridge, etc.)
- **Week 5**: Remove compatibility layer, add tests

**Total**: 5 weeks for complete migration

---

## Status: In Progress

- [x] `IService` interface created
- [x] `DaemonContext` struct created
- [x] `DaemonApp` skeleton created
- [ ] First service migrated (Logger)
- [ ] Tests added
- [ ] Documentation complete

---

## References

- Bitcoin Core uses similar pattern: `NodeContext` in `src/node/context.h`
- Ethereum uses `Node` struct in `core/node/node.go`
- This is industry-standard for blockchain daemons
