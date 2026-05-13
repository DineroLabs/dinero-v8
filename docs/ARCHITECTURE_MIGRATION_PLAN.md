# DineroCoin Architecture Migration Plan
## Option B: Proper Service-Based Architecture (5-Week Plan)

---

## 🎯 **Executive Summary**

**Goal**: Eliminate all singleton/global state and move to a clean context-driven architecture using `DaemonContext` + `IService`.

**Why This Matters**:
- ✅ No more static initialization order bugs
- ✅ Deterministic startup/shutdown sequence
- ✅ Full testability (mockable subsystems)
- ✅ Clear dependency graph
- ✅ Production-grade stability

**Current State**:
- ✅ Foundation exists: `IService`, `DaemonContext`, `DaemonApp` interfaces defined
- ✅ `LoggerService` and `ConfigService` already implemented
- ❌ Existing components (RPCServer, P2PManager, etc.) still use globals
- ❌ Main.cpp still uses old initialization pattern

---

## 📊 **Current vs. Target Architecture**

### **Before (Current)**
```
main.cpp
 ├── g_logger = Logger::GetInstance()
 ├── g_config = Config::GetInstance()
 ├── g_chainstate = Chainstate::GetInstance()
 ├── g_wallet = WalletManager::GetInstance()
 ├── g_rpc = RPCServer::GetInstance()
 └── g_p2p = P2PManager::GetInstance()

Problems:
 - Unordered initialization (race conditions)
 - Hidden dependencies (who needs what?)
 - Cannot unit test
 - Shutdown crashes
```

### **After (Target)**
```
main.cpp
 └── DaemonApp app
      ├── app.Init()
      │    ├── Logger::Init(ctx)
      │    ├── Config::Init(ctx)
      │    ├── Chainstate::Init(ctx)
      │    ├── Mempool::Init(ctx)
      │    ├── WalletManager::Init(ctx)
      │    ├── P2PManager::Init(ctx)
      │    ├── RPCServer::Init(ctx)
      │    ├── MiningCoordinator::Init(ctx)
      │    └── Metrics::Init(ctx)
      ├── app.Start()
      └── app.Stop()  (reverse order)

Benefits:
 ✅ Deterministic order
 ✅ Explicit dependencies
 ✅ Testable (mock any service)
 ✅ Clean shutdown
```

---

## 🗓️ **5-Week Migration Timeline**

| Week | Phase | Components | Deliverable |
|------|-------|-----------|------------|
| **1** | Foundation | DaemonApp + Service Wrappers | Core lifecycle working |
| **2** | Phase 1 | Logger + Config | Base services integrated |
| **3** | Phase 2 | Chainstate + Mempool + WalletManager | State layer working |
| **4** | Phase 3 | P2PManager + RPCServer | Networking layer integrated |
| **5** | Phase 4 | MiningCoordinator + Metrics + Cleanup | All globals removed |

---

## 📋 **Detailed Week-by-Week Plan**

---

### **Week 1: Foundation & Service Wrappers**

#### **Objectives**:
1. Implement `DaemonApp::Init()`, `Start()`, `Stop()`
2. Create service wrapper pattern for existing components
3. Get basic lifecycle working (even if components still use globals internally)

#### **Tasks**:

##### **Task 1.1: Complete DaemonApp Implementation**
**File**: `src/daemon/daemon_app.cpp`

```cpp
bool DaemonApp::Init() {
    // Phase 1: Core services (no dependencies)
    ctx_.logger = std::make_shared<LoggerService>("dinero.log");
    ctx_.config = std::make_shared<ConfigService>();

    // Phase 2: Data layer
    ctx_.chainstate = std::make_shared<ChainstateService>();
    ctx_.mempool = std::make_shared<MempoolService>();
    ctx_.wallet = std::make_shared<WalletManagerService>();

    // Phase 3: Network layer
    ctx_.p2p = std::make_shared<P2PManagerService>();
    ctx_.rpc = std::make_shared<RPCServerService>();

    // Phase 4: Optional services
    ctx_.mining = std::make_shared<MiningCoordinatorService>();
    ctx_.metrics = std::make_shared<MetricsService>();

    // Initialize all services in order
    services_ = {
        ctx_.logger, ctx_.config, ctx_.chainstate, ctx_.mempool,
        ctx_.wallet, ctx_.p2p, ctx_.rpc, ctx_.mining, ctx_.metrics
    };

    for (auto& service : services_) {
        if (!service->Init(ctx_)) {
            ctx_.logger->error("Failed to init: " + service->Name());
            return false;
        }
        ctx_.logger->info("[DaemonApp] Initialized: " + service->Name());
    }

    return true;
}

bool DaemonApp::Start() {
    ctx_.logger->info("[DaemonApp] Starting all services...");

    for (auto& service : services_) {
        if (!service->Start()) {
            ctx_.logger->error("Failed to start: " + service->Name());
            return false;
        }
        ctx_.logger->info("[DaemonApp] Started: " + service->Name());
    }

    started_ = true;
    return true;
}

void DaemonApp::Stop() {
    if (!started_) return;

    ctx_.logger->info("[DaemonApp] Stopping all services...");

    // Stop in REVERSE order
    for (auto it = services_.rbegin(); it != services_.rend(); ++it) {
        (*it)->Stop();
        ctx_.logger->info("[DaemonApp] Stopped: " + (*it)->Name());
    }

    started_ = false;
}
```

##### **Task 1.2: Create Service Wrapper Pattern**

For each existing component, create a thin wrapper that implements `IService`:

**Example: ChainstateService wrapper**
```cpp
// include/daemon/services/chainstate_service.h
class ChainstateService : public IService {
public:
    std::string Name() const override { return "Chainstate"; }

    bool Init(DaemonContext& ctx) override {
        logger_ = ctx.logger;
        config_ = ctx.config;

        // Create actual Chainstate (may still use globals internally)
        chainstate_ = std::make_unique<dinero::Chainstate>();
        return true;
    }

    bool Start() override {
        logger_->info("[Chainstate] Opening blockchain database...");
        return chainstate_->Initialize();
    }

    void Stop() override {
        logger_->info("[Chainstate] Closing blockchain database...");
        chainstate_->Shutdown();
    }

    // Accessor for internal chainstate
    dinero::Chainstate& get() { return *chainstate_; }

private:
    std::shared_ptr<LoggerService> logger_;
    std::shared_ptr<ConfigService> config_;
    std::unique_ptr<dinero::Chainstate> chainstate_;
};
```

**Create wrappers for**:
- ✅ `LoggerService` (already exists)
- ✅ `ConfigService` (already exists)
- 🔨 `ChainstateService`
- 🔨 `MempoolService`
- 🔨 `WalletManagerService`
- 🔨 `P2PManagerService`
- 🔨 `RPCServerService`
- 🔨 `MiningCoordinatorService`
- 🔨 `MetricsService`

##### **Task 1.3: Update main.cpp to use DaemonApp**

```cpp
// src/daemon/main.cpp
#include "daemon/daemon_app.h"

int main(int argc, char* argv[]) {
    // Parse command line args (lightweight, no side effects)
    ParseCommandLine(argc, argv);

    // Create and initialize daemon
    dinero::DaemonApp app;

    if (!app.Init()) {
        std::cerr << "Failed to initialize daemon\n";
        return 1;
    }

    if (!app.Start()) {
        std::cerr << "Failed to start daemon\n";
        app.Stop();
        return 2;
    }

    // Run event loop or wait for shutdown signal
    WaitForShutdownSignal();

    app.Stop();
    return 0;
}
```

#### **Week 1 Deliverable**:
✅ DaemonApp initializes, starts, and stops all services
✅ Clean lifecycle order visible in logs
✅ No crashes on shutdown

---

### **Week 2: Phase 1 - Logger + Config**

#### **Objectives**:
- Logger and Config are already implemented as services
- Ensure all other services use them via context (not globals)

#### **Tasks**:

##### **Task 2.1: Remove Global Logger**
- Search for: `g_logger`, `Logger::GetInstance()`
- Replace with: `ctx.logger->info(...)` in service Init/Start methods
- Verify: No remaining global logger references

##### **Task 2.2: Remove Global Config**
- Search for: `g_config`, `GetConfig()`
- Replace with: `ctx.config->GetString(...)` in services
- Verify: All config access goes through context

##### **Task 2.3: Test Independent Logger/Config**
```cpp
// Unit test example
TEST(ServiceTest, LoggerIndependent) {
    DaemonContext ctx;
    ctx.logger = std::make_shared<LoggerService>("/tmp/test.log");
    ctx.logger->Init(ctx);
    ctx.logger->Start();
    ctx.logger->info("Test message");
    ctx.logger->Stop();
    // Verify log file contains message
}
```

#### **Week 2 Deliverable**:
✅ Logger + Config work as true services
✅ No global `g_logger` or `g_config` remaining
✅ Unit tests prove independence

---

### **Week 3: Phase 2 - Chainstate + Mempool + WalletManager**

#### **Objectives**:
- Wire state layer to use context
- Remove blockchain/wallet globals

#### **Tasks**:

##### **Task 3.1: ChainstateService Implementation**

```cpp
// src/daemon/services/chainstate_service.cpp
bool ChainstateService::Init(DaemonContext& ctx) {
    logger_ = ctx.logger;
    config_ = ctx.config;

    std::string datadir = config_->DataDir();
    logger_->info("[Chainstate] Using datadir: " + datadir);

    chainstate_ = std::make_unique<dinero::Chainstate>();
    chainstate_->SetLogger(logger_.get());  // Inject logger
    chainstate_->SetDataDir(datadir);

    return true;
}

bool ChainstateService::Start() {
    logger_->info("[Chainstate] Loading blockchain database...");
    bool success = chainstate_->LoadFromDisk();

    if (success) {
        uint32_t height = chainstate_->GetHeight();
        logger_->info("[Chainstate] Loaded " + std::to_string(height) + " blocks");
    }

    return success;
}

void ChainstateService::Stop() {
    logger_->info("[Chainstate] Flushing blockchain state...");
    chainstate_->Flush();
    logger_->info("[Chainstate] Closed cleanly");
}
```

##### **Task 3.2: MempoolService Implementation**

```cpp
bool MempoolService::Init(DaemonContext& ctx) {
    logger_ = ctx.logger;
    chainstate_ = ctx.chainstate;  // Depends on chainstate

    mempool_ = std::make_unique<dinero::Mempool>();
    mempool_->SetLogger(logger_.get());
    mempool_->SetChainstate(&chainstate_->get());

    return true;
}
```

##### **Task 3.3: WalletManagerService Implementation**

```cpp
bool WalletManagerService::Init(DaemonContext& ctx) {
    logger_ = ctx.logger;
    config_ = ctx.config;
    chainstate_ = ctx.chainstate;

    wallet_manager_ = std::make_unique<dinero::WalletManager>();
    wallet_manager_->SetLogger(logger_.get());
    wallet_manager_->SetChainstate(&chainstate_->get());

    return true;
}
```

#### **Week 3 Deliverable**:
✅ Chainstate, Mempool, WalletManager work as services
✅ Dependencies injected via context
✅ No global blockchain/wallet state

---

### **Week 4: Phase 3 - P2PManager + RPCServer**

#### **Objectives**:
- Wire networking layer to context
- Remove RPC/P2P globals

#### **Tasks**:

##### **Task 4.1: P2PManagerService Implementation**

```cpp
// include/daemon/services/p2p_manager_service.h
class P2PManagerService : public IService {
public:
    std::string Name() const override { return "P2PManager"; }

    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Event hooks
    std::function<void(const Block&)> OnNewBlock;
    std::function<void(const Tx&)> OnNewTx;

    P2PManager& get() { return *p2p_; }

private:
    std::shared_ptr<LoggerService> logger_;
    std::shared_ptr<ConfigService> config_;
    std::shared_ptr<ChainstateService> chainstate_;
    std::shared_ptr<MempoolService> mempool_;
    std::unique_ptr<P2PManager> p2p_;
};
```

```cpp
// src/daemon/services/p2p_manager_service.cpp
bool P2PManagerService::Init(DaemonContext& ctx) {
    logger_ = ctx.logger;
    config_ = ctx.config;
    chainstate_ = ctx.chainstate;
    mempool_ = ctx.mempool;

    uint16_t p2p_port = config_->P2PPort();
    std::string external_ip = config_->GetString("externalip", "");

    p2p_ = std::make_unique<P2PManager>(p2p_port, external_ip);

    // Wire event hooks
    OnNewBlock = [this](const Block& b) {
        logger_->info("[P2P] New block received: " + b.hash);
        chainstate_->get().ProcessBlock(b);
    };

    OnNewTx = [this](const Tx& tx) {
        logger_->info("[P2P] New tx received: " + tx.txid);
        mempool_->get().AddTransaction(tx);
    };

    return true;
}

bool P2PManagerService::Start() {
    logger_->info("[P2P] Starting P2P network...");

    // Add seed nodes
    auto seeds = config_->GetString("addnode", "");
    // Parse and add seeds...

    return p2p_->start();
}

void P2PManagerService::Stop() {
    logger_->info("[P2P] Stopping P2P network...");
    p2p_->stop();
}
```

##### **Task 4.2: RPCServerService Implementation**

```cpp
// include/daemon/services/rpc_server_service.h
class RPCServerService : public IService {
public:
    std::string Name() const override { return "RPCServer"; }

    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    void Notify(const std::string& topic, const Json::Value& payload);

    dinero::RPCServer& get() { return *rpc_; }

private:
    DaemonContext* ctx_ = nullptr;
    std::unique_ptr<dinero::RPCServer> rpc_;

    void RegisterCommands();
};
```

```cpp
// src/daemon/services/rpc_server_service.cpp
bool RPCServerService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;

    int rpc_port = ctx.config->RPCPort();
    rpc_ = std::make_unique<dinero::RPCServer>();

    // Inject dependencies into RPC server
    rpc_->setBlockchain(&ctx.chainstate->get());
    rpc_->setMempool(std::shared_ptr<Mempool>(&ctx.mempool->get()));
    rpc_->setWalletManager(&ctx.wallet->get());

    RegisterCommands();
    return true;
}

void RPCServerService::RegisterCommands() {
    // Example: blockchain.getblockcount
    rpc_->registerMethod("blockchain.getblockcount", [this](const std::string& params) {
        uint32_t height = ctx_->chainstate->get().GetHeight();
        Json::Value result;
        result["height"] = height;
        return result.toStyledString();
    });

    // Example: wallet.getbalance
    rpc_->registerMethod("wallet.getbalance", [this](const std::string& params) {
        double balance = ctx_->wallet->get().GetTotalBalance();
        Json::Value result;
        result["balance"] = balance;
        return result.toStyledString();
    });

    // ... register all RPC methods ...
}

bool RPCServerService::Start() {
    ctx_->logger->info("[RPC] Starting RPC server on port " +
                       std::to_string(ctx_->config->RPCPort()));
    rpc_->start();
    return true;
}

void RPCServerService::Stop() {
    ctx_->logger->info("[RPC] Stopping RPC server...");
    rpc_->shutdown();
}

void RPCServerService::Notify(const std::string& topic, const Json::Value& payload) {
    // WebSocket broadcast
    rpc_->broadcastEvent(topic, payload);
}
```

##### **Task 4.3: Wire P2P → RPC Events**

```cpp
// In DaemonApp::Start(), wire events:
bool DaemonApp::Start() {
    // ... start services ...

    // Wire P2P events to RPC notifications
    ctx_.p2p->OnNewBlock = [this](const Block& b) {
        Json::Value payload;
        payload["height"] = b.height;
        payload["hash"] = b.hash;
        payload["time"] = b.timestamp;
        ctx_.rpc->Notify("chain.block", payload);
    };

    ctx_.p2p->OnNewTx = [this](const Tx& tx) {
        Json::Value payload;
        payload["txid"] = tx.txid;
        payload["size"] = tx.size;
        ctx_.rpc->Notify("mempool.tx", payload);
    };

    return true;
}
```

#### **Week 4 Deliverable**:
✅ P2P and RPC work as services
✅ RPC handlers use `ctx` instead of globals
✅ WebSocket events flow through context
✅ No global `g_rpc` or `g_p2p`

---

### **Week 5: Phase 4 - Mining + Metrics + Cleanup**

#### **Objectives**:
- Add mining and metrics services
- Remove ALL remaining globals
- Final testing and validation

#### **Tasks**:

##### **Task 5.1: MiningCoordinatorService**

```cpp
bool MiningCoordinatorService::Init(DaemonContext& ctx) {
    logger_ = ctx.logger;
    chainstate_ = ctx.chainstate;
    mempool_ = ctx.mempool;

    mining_ = std::make_unique<MiningCoordinator>();
    mining_->SetLogger(logger_.get());
    mining_->SetChainstate(&chainstate_->get());
    mining_->SetMempool(&mempool_->get());

    return true;
}
```

##### **Task 5.2: MetricsService**

```cpp
bool MetricsService::Init(DaemonContext& ctx) {
    logger_ = ctx.logger;
    metrics_ = std::make_unique<Metrics>();
    return true;
}

bool MetricsService::Start() {
    logger_->info("[Metrics] Starting Prometheus exporter on :9090");
    return metrics_->StartExporter(9090);
}
```

##### **Task 5.3: Remove ALL Globals**

Search and destroy:
```bash
# Find remaining globals
grep -r "GetInstance()" src/ include/
grep -r "^extern.*g_" src/ include/
grep -r "static.*Instance" src/ include/

# Expected result: NONE (except in third-party code)
```

##### **Task 5.4: Integration Testing**

```cpp
TEST(DaemonAppTest, FullLifecycle) {
    dinero::DaemonApp app;

    ASSERT_TRUE(app.Init());
    ASSERT_TRUE(app.Start());

    // Test RPC calls
    auto& rpc = app.GetContext().rpc->get();
    auto result = rpc.handleRequest(R"({"method":"getblockcount"})");
    EXPECT_TRUE(result.find("height") != std::string::npos);

    app.Stop();
}
```

#### **Week 5 Deliverable**:
✅ Mining and Metrics work as services
✅ **ZERO global state remaining**
✅ All tests pass
✅ Production-ready architecture

---

## 🎯 **Success Criteria**

### **Architecture Validation**:
```cpp
// This code should compile and work:
DaemonContext ctx1;
DaemonContext ctx2;

// Run TWO daemons in the same process (for testing)
dinero::DaemonApp daemon1, daemon2;
daemon1.Init();  // Uses ctx1
daemon2.Init();  // Uses ctx2 (isolated)

// Both work independently!
daemon1.Start();
daemon2.Start();
```

### **Metrics**:
| Metric | Before | After |
|--------|--------|-------|
| Global variables | ~20 | 0 |
| Singleton classes | ~10 | 0 |
| Init order bugs | Multiple | 0 |
| Shutdown crashes | Frequent | 0 |
| Testable services | 0% | 100% |

---

## 🧪 **Testing Strategy**

### **Unit Tests (Per Service)**:
```cpp
TEST(ChainstateServiceTest, InitWithMockLogger) {
    DaemonContext ctx;
    ctx.logger = std::make_shared<MockLogger>();
    ctx.config = std::make_shared<MockConfig>();

    auto chainstate = std::make_shared<ChainstateService>();
    ASSERT_TRUE(chainstate->Init(ctx));
    ASSERT_TRUE(chainstate->Start());

    EXPECT_EQ(chainstate->get().GetHeight(), 0);

    chainstate->Stop();
}
```

### **Integration Tests (Full App)**:
```cpp
TEST(IntegrationTest, RPCCallsWorkAfterStart) {
    dinero::DaemonApp app;
    app.Init();
    app.Start();

    // Test blockchain RPC
    auto result = app.GetContext().rpc->get().handleRequest(...);
    EXPECT_TRUE(result.contains("height"));

    app.Stop();
}
```

---

## 📝 **Implementation Checklist**

### **Week 1**:
- [ ] Implement `DaemonApp::Init/Start/Stop`
- [ ] Create service wrapper template
- [ ] Create all 9 service wrapper headers
- [ ] Update `main.cpp` to use `DaemonApp`
- [ ] Verify clean startup/shutdown logs

### **Week 2**:
- [ ] Remove all `g_logger` references
- [ ] Remove all `g_config` references
- [ ] Write unit tests for Logger
- [ ] Write unit tests for Config

### **Week 3**:
- [ ] Implement `ChainstateService`
- [ ] Implement `MempoolService`
- [ ] Implement `WalletManagerService`
- [ ] Remove blockchain globals
- [ ] Test state layer independently

### **Week 4**:
- [ ] Implement `P2PManagerService`
- [ ] Implement `RPCServerService`
- [ ] Wire P2P → RPC events
- [ ] Remove networking globals
- [ ] Test RPC calls end-to-end

### **Week 5**:
- [ ] Implement `MiningCoordinatorService`
- [ ] Implement `MetricsService`
- [ ] Remove ALL remaining globals
- [ ] Run full integration tests
- [ ] Verify zero globals remain

---

## 🚀 **Migration Safety**

### **Rollback Plan**:
Each week's changes are isolated. If issues arise:
1. **Week 1**: Rollback is trivial (only main.cpp changed)
2. **Week 2**: Can keep old logger/config alongside new
3. **Week 3+**: Services are isolated, can revert individual components

### **Backward Compatibility**:
During migration, both old and new code can coexist:
```cpp
// Old code (deprecated but still works during migration)
auto& wallet = WalletManager::GetInstance();

// New code (preferred)
auto& wallet = ctx.wallet->get();
```

---

## 📚 **References**

### **Bitcoin Core NodeContext Migration**:
- [PR #16839](https://github.com/bitcoin/bitcoin/pull/16839) - Introduce NodeContext
- [PR #18698](https://github.com/bitcoin/bitcoin/pull/18698) - Remove globals from RPC

### **Dependency Injection Patterns**:
- Martin Fowler: [Inversion of Control Containers](https://martinfowler.com/articles/injection.html)
- Google's Abseil: [Dependency Injection](https://abseil.io/docs/cpp/guides/dependency-injection)

---

## ✅ **Final State**

After 5 weeks, your daemon will have:

```cpp
// Clean, testable, production-grade architecture
DaemonApp app;
app.Init();   // Deterministic, explicit dependencies
app.Start();  // Ordered, predictable
// ... run ...
app.Stop();   // Clean shutdown, no leaks
```

**Zero globals. Zero singletons. Zero initialization order bugs.**

This is the **real architectural solution** to the "singleton soup" problem.

---

**Next Step**: Confirm this plan, then I'll generate the complete implementation code for Week 1.
