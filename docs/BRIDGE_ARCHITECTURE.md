# Bridge Architecture: Services → Legacy Globals

## The Solution

**Services themselves set the legacy globals** when they initialize. This creates a clean bridge where:

1. ✅ Modern main.cpp (113 lines) stays clean
2. ✅ Services create REAL instances
3. ✅ Services expose instances via legacy globals
4. ✅ Old code using globals works immediately
5. ✅ We can gradually migrate away from globals

## Implementation

### Modern main.cpp (Already Perfect!)

```cpp
// src/daemon/main.cpp (113 lines)
int main() {
    dinero::DaemonApp app;

    if (!app.Init()) {   // ← Services create REAL instances AND set globals
        return 1;
    }

    if (!app.Start()) {  // ← Services start up
        app.Stop();
        return 2;
    }

    // Main event loop
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    app.Stop();
    return 0;
}
```

**NO global manipulation code in main!** Services handle it.

### Services Set Globals During Init

#### P2PService (IMPLEMENTED)

```cpp
// src/daemon/services/p2p_service.cpp
bool P2PService::Init(DaemonContext& ctx) {
    // Create REAL P2PManager
    p2p_mgr_ = std::make_unique<::P2PManager>(listen_port_, external_ip_);

    // BRIDGE: Point global to our real instance
    extern ::P2PManager* dinero::g_p2p;
    dinero::g_p2p = p2p_mgr_.get();  // ← Real instance!

    logger_->info("[P2PService] Legacy global g_p2p → real P2PManager");
    return true;
}
```

**Result**: Old code using `g_p2p->broadcast()` now uses the REAL P2PManager created by the service!

#### WalletService (IMPLEMENTED)

```cpp
// src/daemon/services/wallet_service.cpp
bool WalletService::Init(DaemonContext& ctx) {
    // Create REAL WalletManager
    wallet_mgr_ = std::make_unique<WalletManager>(wallet_dir);

    // BRIDGE: Point global to our real instance
    extern WalletManager* g_wallet_manager;
    g_wallet_manager = wallet_mgr_.get();  // ← Real instance!

    logger_->info("[WalletService] Legacy global g_wallet_manager → real WalletManager");
    return true;
}
```

**Result**: Old code using `g_wallet_manager->getBalance()` now uses the REAL WalletManager!

#### ChainstateService (Template for Week 2)

```cpp
// src/daemon/services/chainstate_service.cpp
bool ChainstateService::Init(DaemonContext& ctx) {
    // Create REAL Blockchain
    blockchain_ = std::make_unique<Blockchain>(datadir_);

    // TODO Week 2: Uncomment when ChainDB accessor exists
    // extern ChainDB* g_chain_db_direct;
    // g_chain_db_direct = blockchain_->GetChainDB();  // ← Real instance!

    logger_->info("[ChainstateService] Blockchain created");
    return true;
}
```

## Why This Works

### Flow of Execution

```
1. main.cpp: app.Init()
   ↓
2. DaemonApp::Init() creates services
   ↓
3. Each service Init():
   - Creates REAL instance (P2PManager, WalletManager, etc.)
   - Points legacy global to that instance
   ↓
4. Old code using globals now uses REAL service instances!
```

### Example: Old Code Just Works

```cpp
// Somewhere in old RPC handler:
if (dinero::g_p2p) {
    g_p2p->broadcast(message);  // ← Uses REAL P2PManager from service!
}

if (g_wallet_manager) {
    balance = g_wallet_manager->getBalance();  // ← Uses REAL WalletManager!
}
```

## Benefits

1. **Clean Separation**
   - main.cpp doesn't know about globals
   - Services own their instances
   - Services decide what to expose

2. **Real Instances**
   - No null stubs
   - Services create real, working instances
   - Old code gets real functionality

3. **Gradual Migration**
   - Old code works immediately
   - Week 2+: Replace `g_thing->method()` with `ctx.service->method()`
   - Week 4: Remove legacy_globals_stub.cpp entirely

4. **Clear Ownership**
   - Services own instances (unique_ptr)
   - Globals are raw pointers for access
   - Service lifetime controls instance lifetime

## Migration Path

### Week 1 (Current)
- ✅ Services create real instances
- ✅ Services set legacy globals
- ✅ Modern main.cpp uses DaemonApp
- ✅ Old code works via globals

### Week 2-3
```cpp
// OLD CODE:
uint32_t height = g_chain_db_direct->GetHeight();

// MIGRATE TO:
uint32_t height = ctx.chainstate->get().GetHeight();
```

### Week 4
- Remove all `extern` declarations
- Remove `legacy_globals_stub.cpp`
- Pure service architecture!

## Current Status

### Implemented
- ✅ P2PService sets `dinero::g_p2p`
- ✅ WalletService sets `g_wallet_manager`
- ✅ ChainstateService has template (waiting for accessor)

### TODO Week 2
- Add ChainDB accessor to Blockchain class
- Uncomment ChainDB global bridge
- Add any other missing global bridges
- Start migrating RPC handlers to use ctx

## The Key Insight

> **Services create REAL instances and expose them via globals.**
> **This is NOT about stubs - it's about bridges to real functionality.**
> **The globals are a temporary bridge, not the destination.**

The service architecture is already working and creating real instances. We just needed to connect the legacy globals to those real instances!
