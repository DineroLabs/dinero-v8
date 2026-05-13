# Global Singleton Removal Plan

**Goal**: Eliminate all global singletons and static instances, migrating to DaemonContext-based dependency injection.

---

## Current Singletons Identified

### Core Singletons (Already Have Service Wrappers)
These are already wrapped but still using singletons internally:

✅ **Logger** - `g_logger` global
- Wrapped by: `LoggerService`
- Status: Service wrapper complete, can migrate

✅ **Mempool** - Direct instantiation in main.cpp
- Wrapped by: `MempoolService`
- Status: Service wrapper complete, can migrate

✅ **Wallet** - `WalletManager` direct instantiation
- Wrapped by: `WalletService`
- Status: Service wrapper complete, can migrate

✅ **P2P** - `P2PManager` direct instantiation
- Wrapped by: `P2PService`
- Status: Service wrapper complete, can migrate

✅ **Mining** - Direct inst antiation
- Wrapped by: `MiningService`
- Status: Service wrapper complete, can migrate

✅ **Metrics** - Direct instantiation
- Wrapped by: `MetricsService`
- Status: Service wrapper complete, can migrate

✅ **RPC** - `RPCServer` direct instantiation
- Wrapped by: `RPCService`
- Status: Service wrapper complete, can migrate

### Optional Feature Singletons
These already have infrastructure in DaemonContext:

🔧 **EventBus** - `dinero::rpc::EventBus::instance()`
- Location: `include/rpc/event_bus.h`
- Usage: WebSocket event notifications
- Status: Can reference via DaemonContext

🔧 **FiatBridgeManager** - `dinero::bridge::FiatBridgeManager::instance()`
- Location: `include/bridge/fiat_bridge_manager.h`
- Usage: Fiat/crypto conversions
- Status: Can reference via DaemonContext

🔧 **MarketplaceManager** - `din::MarketplaceManager::instance()`
- Location: `include/p2p/marketplace_manager.h`
- Usage: P2P marketplace
- Status: Can reference via DaemonContext

🔧 **EscrowManager** - `dinero::p2p::EscrowManager::instance()`
- Location: `include/p2p/escrow_manager.h`
- Usage: Time-locked escrows
- Status: Can reference via DaemonContext

### Additional Singletons (Need Service Wrappers)

⚠️ **TokenManager** - `dinero::rpc::TokenManager::instance()`
- Location: `include/rpc/token_manager.h`
- Usage: API token authentication
- Action: Create service wrapper

⚠️ **SessionManager** - `dinero::rpc::SessionManager::instance()`
- Location: `include/rpc/session_manager.h`
- Usage: RPC session management
- Action: Create service wrapper

⚠️ **ContractRegistry** - `dinero::contracts::ContractRegistry::instance()`
- Location: `include/contracts/contract_registry.h`
- Usage: Smart contract registration
- Action: Create service wrapper

⚠️ **ARPManager** - `dinero::daemon::ARPManager::instance()`
- Location: `include/daemon/arp_manager.h`
- Usage: Anchor Reference Price system
- Action: Create service wrapper

⚠️ **KYCManager** - `dinero::p2p::KYCManager::instance()`
- Location: `include/p2p/kyc_manager.h`
- Usage: KYC verification
- Action: Create service wrapper

⚠️ **PaymentAdapter** - Singleton pattern
- Location: `include/p2p/payment_adapter.h`
- Usage: Payment monitoring
- Action: Create service wrapper

---

## Migration Strategy

### Phase 1: Update main.cpp to Use DaemonApp ✅ NEXT
**Goal**: Replace legacy initialization with DaemonApp lifecycle

**Steps**:
1. ✅ Create backup of current main.cpp
2. ⚠️ Replace direct component instantiation with DaemonApp
3. ⚠️ Access components via `ctx_.service->get()` pattern
4. ⚠️ Test that daemon starts correctly

**Before**:
```cpp
// Direct instantiation (legacy)
P2PManager p2p;
RPCServer rpc;
WalletManager wallet;
```

**After**:
```cpp
// DaemonApp lifecycle
dinero::DaemonApp app;
if (!app.Init()) return 1;
if (!app.Start()) return 1;

// Access via context
auto& p2p = app.GetContext().p2p->get();
auto& rpc = app.GetContext().rpc->get();
auto& wallet = app.GetContext().wallet->get();
```

### Phase 2: Create Service Wrappers for Remaining Singletons
**Goal**: Wrap remaining singletons in IService pattern

#### 2.1 TokenManagerService
```cpp
class TokenManagerService : public IService {
public:
    std::string Name() const override { return "TokenManager"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;
    TokenManager& get() { return *token_mgr_; }
private:
    std::unique_ptr<TokenManager> token_mgr_;
};
```

#### 2.2 SessionManagerService
```cpp
class SessionManagerService : public IService {
    // Similar pattern...
};
```

#### 2.3 ContractRegistryService
```cpp
class ContractRegistryService : public IService {
    // Similar pattern...
};
```

#### 2.4 ARPManagerService
```cpp
class ARPManagerService : public IService {
    // Similar pattern...
};
```

#### 2.5 KYCManagerService
```cpp
class KYCManagerService : public IService {
    // Similar pattern...
};
```

### Phase 3: Update RPC Handlers to Use DaemonContext
**Goal**: Pass DaemonContext to all RPC methods

**Current Pattern (uses globals)**:
```cpp
Json::Value getblockcount(const Json::Value& params) {
    // Uses global g_chain_db_direct
    auto height = g_chain_db_direct->getHeight();
    return height;
}
```

**New Pattern (uses context)**:
```cpp
Json::Value getblockcount(const Json::Value& params, DaemonContext& ctx) {
    // Uses context
    auto height = ctx.chainstate->GetHeight();
    return height;
}
```

**Implementation**:
1. Add `DaemonContext&` parameter to all RPC method signatures
2. Store context reference in RPCServer for handler callbacks
3. Update all 150+ RPC methods incrementally

### Phase 4: Remove Singleton instance() Methods
**Goal**: Make constructors public, remove static instance()

**Before**:
```cpp
class EventBus {
public:
    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }
private:
    EventBus() = default;  // Private constructor
};
```

**After**:
```cpp
class EventBus {
public:
    EventBus() = default;  // Public constructor
    ~EventBus() = default;
    // Remove instance() method
};
```

### Phase 5: Remove Global Variables
**Goal**: Eliminate all global state

**Globals to Remove**:
- `g_logger` - Replace with `ctx.logger`
- `g_chain_db_direct` - Replace with `ctx.chainstate`
- `g_rpcRegistry` - Replace with context-stored registry
- `g_daemon_start_time` - Move to DaemonContext or DaemonApp
- `g_mempool` - Replace with `ctx.mempool`
- `g_wallet_manager` - Replace with `ctx.wallet`

---

## Execution Plan

### Week 2 (Current)
- [x] Create service wrappers for core components
- [x] Build DaemonContext infrastructure
- [x] Build DaemonApp lifecycle orchestrator
- [ ] **Update main.cpp to use DaemonApp** ← CURRENT TASK
- [ ] Test daemon startup/shutdown with new architecture

### Week 3
- [ ] Create service wrappers for remaining singletons
  - [ ] TokenManagerService
  - [ ] SessionManagerService
  - [ ] ContractRegistryService
  - [ ] ARPManagerService
  - [ ] KYCManagerService
- [ ] Add new services to DaemonContext
- [ ] Update DaemonApp to initialize new services

### Week 4
- [ ] Migrate RPC handlers to use DaemonContext
  - [ ] Add context parameter to RPC method signatures
  - [ ] Update RPCServer to pass context to handlers
  - [ ] Migrate blockchain RPC methods
  - [ ] Migrate wallet RPC methods
  - [ ] Migrate mining RPC methods
  - [ ] Migrate network RPC methods
  - [ ] Migrate all remaining RPC methods

### Week 5
- [ ] Remove all singleton instance() methods
- [ ] Remove all global variables
- [ ] Final testing and validation
- [ ] Performance benchmarking
- [ ] Documentation updates

---

## Testing Strategy

### After Each Phase
1. **Compilation Test**: Ensure no build errors
2. **Startup Test**: Verify daemon starts without crashes
3. **RPC Test**: Verify all RPC methods work
4. **P2P Test**: Verify peer connectivity
5. **Mining Test**: Verify block generation (regtest)
6. **Wallet Test**: Verify wallet operations
7. **Shutdown Test**: Verify clean shutdown

### Integration Tests
```bash
# Start daemon
./dinerod --regtest --datadir=/tmp/test-context

# Test RPC connectivity
./dinero-cli --regtest getblockcount

# Generate blocks
./dinero-cli --regtest generatetoaddress 10 <address>

# Check wallet
./dinero-cli --regtest wallet.getbalance

# Stop daemon
./dinero-cli --regtest stop
```

---

## Rollback Strategy

If issues arise during migration:

1. **Keep Legacy main.cpp as backup**: `main.cpp.legacy`
2. **Use Git branches**: Create `feature/remove-globals` branch
3. **Gradual migration**: Test each phase independently
4. **Compatibility layer**: Keep singleton wrappers temporarily if needed

---

## Success Criteria

✅ Zero global variables
✅ Zero singleton instance() methods
✅ All services accessed via DaemonContext
✅ All RPC handlers use DaemonContext
✅ Clean Init/Start/Stop lifecycle
✅ No static initialization order issues
✅ 100% test pass rate
✅ No performance regression

---

## Current Status

- ✅ Phase 1 Infrastructure Complete (Week 1)
- ✅ Service wrappers created (9 core services)
- ✅ DaemonContext created
- ✅ DaemonApp created
- ⚠️ **NEXT**: Update main.cpp to use DaemonApp

---

**Ready to Begin**: Phase 1 - Update main.cpp
