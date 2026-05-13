# Dinero Architecture Lineage Analysis

**Date**: 2025-01-05
**Status**: Pre-Context Era (Custom Implementation)

---

## 🔍 Bitcoin Core Lineage Assessment

### Version Identification

**Finding**: Dinero is **NOT directly based on Bitcoin Core**
- Version: `0.9.0-beta.1` (custom versioning)
- No `CLIENT_VERSION` macros found
- No Bitcoin Core copyright headers
- Custom implementation from scratch

### Structural Analysis

| Feature | Bitcoin Core (≤0.16) | Bitcoin Core (≥0.17) | Dinero Status |
|---------|---------------------|---------------------|---------------|
| `src/node/context.h` | ❌ Not present | ✅ Present | ❌ Not present |
| `src/interfaces/` | ❌ Not present | ✅ Present | ❌ Not present |
| `configure.ac` | ✅ Present | ✅ Present | ❌ Not present |
| `CMakeLists.txt` | ❌ Not present | ❌ Not present | ✅ Present (custom) |
| `pwalletMain` global | ✅ Present | ❌ Removed | ❌ Not present |
| Global singletons | ✅ Heavy use | ⚠️ Being phased out | ✅ Heavy use |

**Conclusion**: Dinero is a **ground-up implementation** inspired by cryptocurrency design patterns but not a Bitcoin Core fork.

---

## 🏗️ Current Architecture (As-Is)

### Global Singletons Pattern

Dinero currently uses global singletons extensively:

```cpp
// include/common/logger.h
namespace dinero {
    extern Logger g_logger;  // Global logger instance
}

// Typical usage throughout codebase:
dinero::g_logger.info("Starting service...");
```

**Other Global Patterns**:
- `g_chainstate` - Blockchain state
- `g_mempool` - Transaction pool
- `g_p2p` - P2P networking
- `g_rpcServer` - RPC server
- Singleton patterns via `::instance()` methods

### Problems with Current Design

1. **Static Initialization Order Fiasco**
   - KYCManager mutex crash we just fixed
   - Unpredictable init order across translation units
   - Race conditions at startup

2. **Testability Issues**
   - Can't mock dependencies
   - Can't test components in isolation
   - Unit tests affect global state

3. **Tight Coupling**
   - Services directly reference globals
   - Hard to swap implementations
   - Circular dependencies

4. **Lifetime Management**
   - No deterministic shutdown order
   - Resource leaks possible
   - Can't restart services without full restart

---

## 🎯 Target Architecture (To-Be)

### Modern Service-Oriented Design

Inspired by Bitcoin Core 0.21+ and modern C++ best practices:

```cpp
// 1. Service Interface
class IService {
public:
    virtual bool Init(DaemonContext& ctx) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
};

// 2. Dependency Injection Container
struct DaemonContext {
    std::shared_ptr<Logger> logger;
    std::shared_ptr<Chainstate> chainstate;
    // ... all services
};

// 3. Lifecycle Manager
class DaemonApp {
    bool Init() {
        ctx_.logger = std::make_shared<Logger>();
        for (auto& s : services_) s->Init(ctx_);
    }
    bool Start() { /* start in order */ }
    void Stop() { /* stop in reverse */ }
private:
    DaemonContext ctx_;
    std::vector<std::shared_ptr<IService>> services_;
};
```

### Benefits

✅ **Deterministic initialization** - services start in defined order
✅ **Testable** - inject mocks via DaemonContext
✅ **Clear dependencies** - explicit in Init()
✅ **Safe shutdown** - reverse order stop
✅ **Modular** - add/remove services cleanly
✅ **No more singletons** - all instances managed

---

## 🗺️ Migration Strategy (5-Week Plan)

### Week 1: Foundation

**Goal**: Create core infrastructure without breaking existing code

- [x] Create `IService` interface
- [x] Create `DaemonContext` struct
- [ ] Create `DaemonApp` lifecycle manager
- [ ] Create compatibility layer (ServiceLocator)
- [ ] Document migration pattern

**Deliverables**:
- `include/core/iservice.h`
- `include/daemon/daemon_context.h`
- `src/daemon/daemon_app.cpp`
- `docs/MIGRATION_GUIDE.md`

### Week 2: Logger & Config (Proof of Concept)

**Goal**: Migrate simplest services to prove the pattern

**Logger Migration**:
```cpp
// Old: include/common/logger.h
extern Logger g_logger;

// New: include/services/logger_service.h
class LoggerService : public IService {
public:
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    void info(const std::string& msg);
};

// Compatibility: Keep g_logger pointing to ctx.logger
```

**Config Migration**:
```cpp
// Old: Direct file access
Config config;
config.load("dinero.conf");

// New: Service with context
class ConfigService : public IService {
    bool Init(DaemonContext& ctx) override {
        return LoadConfig(ctx.datadir);
    }
};
```

**Deliverables**:
- Migrated Logger to IService
- Migrated Config to IService
- Both services work via DaemonApp
- Backward compatibility maintained

### Week 3: Chainstate & Storage

**Goal**: Migrate blockchain core

**Chainstate Migration**:
```cpp
class ChainstateService : public IService {
public:
    bool Init(DaemonContext& ctx) override {
        logger_ = ctx.logger;
        db_path_ = ctx.GetServiceDataDir("chainstate");
        return OpenDatabase();
    }

    bool Start() override {
        return ValidateGenesis();
    }

    void Stop() override {
        CloseDatabase();
    }

private:
    std::shared_ptr<Logger> logger_;
    std::filesystem::path db_path_;
};
```

**Deliverables**:
- Chainstate as IService
- Mempool as IService
- RocksDB managed lifecycle
- Genesis validation in Start()

### Week 4: Wallet & P2P

**Goal**: Migrate user-facing services

**WalletManager Migration**:
```cpp
class WalletService : public IService {
public:
    bool Init(DaemonContext& ctx) override {
        chainstate_ = ctx.chainstate;
        logger_ = ctx.logger;
        return LoadWallets(ctx.datadir / "wallets");
    }

    bool Start() override {
        return StartWalletWorker();
    }

    void Stop() override {
        StopWalletWorker();
        SaveWallets();
    }

private:
    std::shared_ptr<Chainstate> chainstate_;
    std::shared_ptr<Logger> logger_;
};
```

**P2PManager Migration**:
```cpp
class P2PService : public IService {
public:
    bool Init(DaemonContext& ctx) override {
        chainstate_ = ctx.chainstate;
        mempool_ = ctx.mempool;
        return InitializeNetworking();
    }

    bool Start() override {
        return ConnectToPeers();
    }

    void Stop() override {
        DisconnectAllPeers();
    }
};
```

**Deliverables**:
- Wallet as IService
- P2P as IService
- HD wallet support maintained
- Network layer clean shutdown

### Week 5: RPC, Mining & Integration

**Goal**: Complete migration and remove compatibility layer

**RPCServer Migration**:
```cpp
class RPCService : public IService {
public:
    bool Init(DaemonContext& ctx) override {
        chainstate_ = ctx.chainstate;
        wallet_ = ctx.wallet;
        mining_ = ctx.mining;
        return RegisterAllMethods();
    }

    bool Start() override {
        return BindAndListen(ctx_.config->rpc_port);
    }

    void Stop() override {
        StopServer();
    }
};
```

**Final Integration**:
```cpp
// src/daemon/main.cpp
int main(int argc, char** argv) {
    DaemonApp app;

    if (!app.Init()) return 1;
    if (!app.Start()) return 2;

    // Wait for shutdown signal
    WaitForShutdownSignal();

    app.Stop();
    return 0;
}
```

**Deliverables**:
- All services migrated
- Remove global singletons
- Remove ServiceLocator compatibility
- Full test coverage
- Documentation complete

---

## 📊 Migration Checklist

### Phase 1: Foundation (Week 1)
- [x] IService interface created
- [x] DaemonContext struct created
- [ ] DaemonApp lifecycle manager
- [ ] ServiceLocator bridge (temporary)
- [ ] Unit test framework
- [ ] Migration guide

### Phase 2: Core Services (Weeks 2-3)
- [ ] Logger → LoggerService
- [ ] Config → ConfigService
- [ ] Chainstate → ChainstateService
- [ ] Mempool → MempoolService
- [ ] Storage layer cleanup

### Phase 3: User Services (Week 4)
- [ ] WalletManager → WalletService
- [ ] P2PManager → P2PService
- [ ] Mining → MiningService
- [ ] Metrics → MetricsService

### Phase 4: Integration (Week 5)
- [ ] RPCServer → RPCService
- [ ] Marketplace services integration
- [ ] Remove all global singletons
- [ ] Remove ServiceLocator
- [ ] Full test suite
- [ ] Performance validation

---

## 🧪 Testing Strategy

### Unit Tests
```cpp
TEST(LoggerService, InitializesCorrectly) {
    DaemonContext ctx;
    ctx.datadir = "/tmp/test";

    auto logger = std::make_shared<LoggerService>();
    ASSERT_TRUE(logger->Init(ctx));
    ASSERT_TRUE(logger->Start());

    logger->info("Test message");
    logger->Stop();
}
```

### Integration Tests
```cpp
TEST(DaemonApp, StartsAndStopsCleanly) {
    DaemonApp app;
    ASSERT_TRUE(app.Init());
    ASSERT_TRUE(app.Start());

    // Services should be running
    ASSERT_TRUE(app.IsHealthy());

    app.Stop();
    // No memory leaks, clean shutdown
}
```

---

## 🚀 Risk Mitigation

### Low-Risk Approach

1. **Incremental Migration**
   - One service at a time
   - Keep old code working
   - Feature flag new services

2. **Backward Compatibility**
   - ServiceLocator bridge during transition
   - Global symbols point to context
   - No API breaks

3. **Testing at Each Step**
   - Unit tests for each service
   - Integration tests for combinations
   - Regression tests for existing features

4. **Rollback Plan**
   - Git branches for each phase
   - Can revert individual services
   - No "big bang" replacement

---

## 📈 Success Metrics

- ✅ No static initialization crashes
- ✅ All services start/stop cleanly
- ✅ 90%+ unit test coverage
- ✅ No performance regression
- ✅ Memory usage stable or improved
- ✅ New features easy to add
- ✅ Code review time reduced

---

## 🎯 Next Steps

1. **Review this analysis** with team
2. **Approve migration plan** and timeline
3. **Start Week 1** foundation work
4. **Set up CI/CD** for migration branches
5. **Begin Logger migration** as proof of concept

**Ready to proceed?** Let's start with Week 1 - Foundation.
