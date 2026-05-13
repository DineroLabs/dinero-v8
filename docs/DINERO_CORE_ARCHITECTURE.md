# Dinero Core Architecture - Service-Oriented Design

**Last Updated**: Week 6 (Post-P2P Migration)  
**Status**: ✅ Production-Ready, Fully Context-Driven

---

## 🏗️ Architecture Topology

```
DaemonApp (Lifecycle Orchestrator)
 │
 ├── ChainstateService (Blockchain + ChainDB)
 ├── MempoolService (Transaction Pool)
 ├── WalletService (HD Wallet Management)
 ├── P2PService (Network Layer)
 ├── RPCService (JSON-RPC API)
 ├── MiningService (Block Production)
 └── IConsensusEngine (PowConsensusEngine)
```

### **Service Access Pattern**

All RPC handlers access services **exclusively via DaemonContext**:

```cpp
// ✅ CORRECT: Context-driven access
ctx.daemon->wallet->get()
ctx.daemon->p2p->get()
ctx.daemon->chainstate->chainDB()
ctx.daemon->mempool->mempool()
ctx.daemon->mining->createBlockTemplate(ctx)
ctx.daemon->chainstate->blockchain()

// ❌ WRONG: Global state (eliminated)
g_wallet_manager
g_p2p
g_chain_db_direct
g_mempool
```

---

## 🎯 Design Principles

### **1. Dependency Injection via DaemonContext**

Every service receives its dependencies through the `DaemonContext`:

```cpp
struct DaemonContext {
    std::shared_ptr<LoggerService> logger;
    std::shared_ptr<ConfigService> config;
    std::shared_ptr<ChainstateService> chainstate;
    std::shared_ptr<MempoolService> mempool;
    std::shared_ptr<WalletService> wallet;
    std::shared_ptr<P2PService> p2p;
    std::shared_ptr<RPCService> rpc;
    std::shared_ptr<MiningService> mining;
    std::shared_ptr<MetricsService> metrics;
    std::shared_ptr<IConsensusEngine> consensus;
};
```

### **2. Service Isolation**

Each service implements `IService` interface:

```cpp
class IService {
public:
    virtual ~IService() = default;
    virtual bool Init(const DaemonContext& ctx) = 0;
    virtual bool Start() = 0;
    virtual bool Stop() = 0;
    virtual std::string Name() const = 0;
};
```

### **3. Zero Global State**

- ✅ **No globals in core code**: All services accessed via context
- ✅ **Bridge pattern eliminated**: Legacy globals removed from active code
- ✅ **Multi-instance safe**: Can spawn multiple daemons without conflicts

---

## 🔄 Service Lifecycle

### **Initialization Flow**

```
DaemonApp::Init()
  ├─ Create LoggerService
  ├─ Create ConfigService
  ├─ Create ChainstateService (depends on Logger, Config)
  ├─ Create MempoolService (depends on Chainstate)
  ├─ Create WalletService (depends on Chainstate)
  ├─ Create P2PService (depends on Chainstate, Mempool)
  ├─ Create RPCService (depends on all services)
  ├─ Create MiningService (depends on Chainstate, Mempool)
  ├─ Create MetricsService
  └─ Create PowConsensusEngine (depends on Chainstate, Mining)
```

### **Service Dependencies**

```
ChainstateService ──┐
                    ├──> MempoolService
                    ├──> WalletService
                    ├──> P2PService
                    └──> MiningService

MempoolService ────> MiningService

All Services ──────> RPCService (for RPC handlers)
```

---

## 📡 RPC Handler Pattern

### **ExecutionContext Access**

RPC handlers receive `ExecutionContext` which contains `DaemonContext`:

```cpp
din::Json rpc_getpeerinfo(const ExecutionContext& ctx, const din::Json& params) {
    // Access P2P service via context
    if (ctx.daemon && ctx.daemon->p2p) {
        auto& p2p_mgr = ctx.daemon->p2p->get();
        auto peers = p2p_mgr.get_connected_peers();
        // ... process peers
    }
    return result;
}
```

### **Legacy RPCServer Pattern** (Being Migrated)

For handlers using `RPCServer& server`:

```cpp
Json::Value rpc_submitblock(dinero::RPCServer& server, const Json::Value& params) {
    // Get context from server
    const auto& ctx = server.getExecutionContext();
    if (ctx.daemon && ctx.daemon->p2p) {
        auto& p2p_mgr = ctx.daemon->p2p->get();
        // ... use P2P service
    }
    return response;
}
```

---

## 🧪 Testing Architecture

### **TestDaemonContext**

Isolated test environment with mockable services:

```cpp
struct TestDaemonContext {
    std::shared_ptr<LoggerService> logger;
    std::shared_ptr<ConfigService> config;
    std::shared_ptr<MockChainstateService> chainstate;
    std::shared_ptr<MockWalletService> wallet;
    // ... other mock services
    
    DaemonContext make() const {
        DaemonContext ctx;
        ctx.logger = logger;
        ctx.config = config;
        ctx.chainstate = chainstate;
        ctx.wallet = wallet;
        return ctx;
    }
};
```

### **Testing Benefits**

✅ **Multi-Instance Testing**
- Spawn multiple test daemons with isolated P2P contexts
- No port conflicts (each daemon has its own P2P service)
- Test network partitioning scenarios

✅ **Mock Services**
- Mock P2P in `TestDaemonContext` for offline RPC testing
- Inject fake peer lists for network simulation
- Control service behavior deterministically

✅ **CI/CD Safety**
- Run RPC integration tests in CI safely
- No global state conflicts between test runs
- Parallel test execution possible

✅ **Isolation**
- Each test gets its own `DaemonContext`
- Services don't leak state between tests
- Deterministic test execution

---

## 🚀 Migration Status

### **✅ Completed Migrations**

1. **P2P Service** ✅
   - `BlockAcceptor::NotifyBlockConnected()` uses `ctx_->p2p->get()`
   - `BlockBroadcastVerifier` uses `ctx_->p2p->get()`
   - All RPC handlers migrated (`getpeerinfo`, `getconnectioncount`, `submitblock`)

2. **Chainstate Service** ✅
   - `BlockAssembler` uses injected `ChainDB*`
   - `MiningTemplateValidator` uses injected `ChainDB*`
   - All RPC handlers use `ctx.daemon->chainstate->chainDB()`

3. **Mempool Service** ✅
   - `Mining` uses injected `m_mempool`
   - Fee calculation uses context-injected mempool

4. **Wallet Service** ✅
   - All wallet RPC handlers use `ctx.daemon->wallet->get()`
   - Bridge pattern removed

### **📊 Global State Elimination**

| Component | Before | After |
|-----------|--------|-------|
| P2P | `g_p2p` global | `ctx.daemon->p2p->get()` |
| ChainDB | `g_chain_db_direct` global | `ctx.daemon->chainstate->chainDB()` |
| Wallet | `g_wallet_manager` global | `ctx.daemon->wallet->get()` |
| Mempool | `g_mempool` global | `ctx.daemon->mempool->mempool()` |

---

## 🎓 Comparison: Bitcoin Core 25+ Pattern

Dinero Core now follows the same architectural evolution as Bitcoin Core:

**Bitcoin Core 25+**:
- `NodeContext` holds all services
- RPC handlers access via `NodeContext`
- Zero global state in core code
- Multi-instance testing support

**Dinero Core**:
- `DaemonContext` holds all services ✅
- RPC handlers access via `ExecutionContext.daemon` ✅
- Zero global state in core code ✅
- Multi-instance testing support ✅

This is the **"Service Bus" pattern** that modern blockchain frameworks evolved toward.

---

## 📈 Benefits Realized

### **1. Maintainability**
- Clear dependency graph
- Easy to understand service relationships
- No hidden global dependencies

### **2. Testability**
- Mock any service for testing
- Isolated test environments
- Deterministic test execution

### **3. Extensibility**
- Add new services without touching existing code
- Swap consensus engines (`IConsensusEngine`)
- Plugin architecture ready

### **4. Reliability**
- No global state corruption
- Multi-instance safe
- Clean shutdown guaranteed

### **5. Professionalism**
- Industry-standard architecture
- Matches Bitcoin Core patterns
- Production-ready design

---

## 🔮 Future Enhancements

With this architecture in place, we can now:

1. **Multi-Daemon Testing**: Run multiple daemons in parallel tests
2. **Network Simulation**: Inject mock P2P services for testing
3. **Consensus Swapping**: Swap PoW for PoS/hybrid via `IConsensusEngine`
4. **Service Plugins**: Add new services without core changes
5. **CI/CD Integration**: Safe parallel test execution

---

**Status**: ✅ **Architecture Complete** - Production-ready, fully context-driven design! 🎉

