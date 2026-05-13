# Service Wrapper Pattern

**Week 1 Migration Task** - Create service wrappers for existing components

## Overview

The Service Wrapper pattern allows us to adapt existing Dinero components (singletons, global managers) into the new IService-based architecture **without modifying their code**.

This enables:
- ✅ Incremental migration (one service at a time)
- ✅ Backward compatibility during transition
- ✅ Easy rollback if issues arise
- ✅ Both old and new code working simultaneously

## Pattern Structure

```cpp
// include/daemon/services/<component>_service.h

#pragma once
#include "daemon/iservice.h"
#include "<original_header>.h"  // Existing component
#include <memory>

namespace dinero {

/**
 * <Component>Service - IService wrapper for <Component>
 *
 * Wraps existing <Component> into IService lifecycle:
 * - Init() wires dependencies from DaemonContext
 * - Start() calls original initialization
 * - Stop() calls original cleanup
 *
 * Dependencies: [List what this service needs from ctx]
 */
class <Component>Service : public IService {
public:
    <Component>Service() = default;

    std::string Name() const override { return "<ComponentName>"; }

    bool Init(DaemonContext& ctx) override {
        // Store dependencies from context
        logger_ = ctx.logger;
        config_ = ctx.config;

        // Create/initialize wrapped component
        wrapped_ = std::make_unique<<Component>>();
        return wrapped_ != nullptr;
    }

    bool Start() override {
        // Call original start/init method
        return wrapped_->initialize();  // or whatever the original method is
    }

    void Stop() override {
        // Call original cleanup
        wrapped_->shutdown();  // or whatever the original method is
        wrapped_.reset();
    }

    // Expose access to wrapped component
    <Component>& get() { return *wrapped_; }
    const <Component>& get() const { return *wrapped_; }

    // Optional: Forward commonly used methods
    // ReturnType someMethod(Args...) { return wrapped_->someMethod(...); }

private:
    std::unique_ptr<<Component>> wrapped_;
    std::shared_ptr<LoggerService> logger_;
    std::shared_ptr<ConfigService> config_;
    // ... other dependencies
};

} // namespace dinero
```

## Example: ChainstateService

### Before (Old Code)

```cpp
// src/daemon/main.cpp
#include "consensus/chainstate.h"

Chainstate g_chainstate;  // Global singleton

int main() {
    g_chainstate.initialize("~/.dinero");
    // ... use g_chainstate everywhere
    g_chainstate.shutdown();
}
```

### After (Service Wrapper)

```cpp
// include/daemon/services/chainstate_service.h
#pragma once
#include "daemon/iservice.h"
#include "consensus/chainstate.h"
#include <memory>

namespace dinero {

class ChainstateService : public IService {
public:
    std::string Name() const override { return "Chainstate"; }

    bool Init(DaemonContext& ctx) override {
        logger_ = ctx.logger;
        config_ = ctx.config;

        // Create chainstate (was global before)
        chainstate_ = std::make_unique<Chainstate>();
        return true;
    }

    bool Start() override {
        std::string datadir = config_->DataDir();
        if (!chainstate_->initialize(datadir)) {
            logger_->error("[ChainstateService] Failed to initialize");
            return false;
        }
        logger_->info("[ChainstateService] Blockchain loaded: " +
                     std::to_string(chainstate_->getBestHeight()));
        return true;
    }

    void Stop() override {
        logger_->info("[ChainstateService] Shutting down chainstate");
        chainstate_->shutdown();
        chainstate_.reset();
    }

    // Expose wrapped chainstate
    Chainstate& get() { return *chainstate_; }

    // Forward common methods
    uint64_t getBestHeight() const { return chainstate_->getBestHeight(); }

private:
    std::unique_ptr<Chainstate> chainstate_;
    std::shared_ptr<LoggerService> logger_;
    std::shared_ptr<ConfigService> config_;
};

} // namespace dinero
```

### Usage in DaemonApp

```cpp
// src/daemon/daemon_app.cpp
bool DaemonApp::Init() {
    // ... logger, config ...

    // Create chainstate service
    ctx_.chainstate = std::make_shared<ChainstateService>();
    services_.push_back(ctx_.chainstate);

    // Initialize all services
    for (auto& service : services_) {
        if (!service->Init(ctx_)) {
            return false;
        }
    }
    return true;
}
```

## Migration Checklist

For each component being wrapped:

- [ ] Create `include/daemon/services/<component>_service.h`
- [ ] Implement Init() to wire dependencies from ctx
- [ ] Implement Start() to call original initialization
- [ ] Implement Stop() to call original cleanup
- [ ] Add to DaemonContext struct
- [ ] Add to DaemonApp::Init() in dependency order
- [ ] Add to services_ vector
- [ ] Test startup and shutdown
- [ ] Verify original functionality preserved

## Service Dependencies

Service initialization order matters:

```
1. Logger (no dependencies)
2. Config (depends on Logger)
3. Chainstate (depends on Logger, Config)
4. Mempool (depends on Chainstate, Logger)
5. WalletManager (depends on Chainstate, Logger, Config)
6. P2PManager (depends on Chainstate, Mempool, Logger)
7. RPCServer (depends on all above)
8. MiningCoordinator (depends on Chainstate, Mempool, Logger)
9. Metrics (depends on Logger)
```

## Week 1 Services to Wrap

| Service | Original Component | Header | Dependencies |
|---------|-------------------|--------|--------------|
| ✅ LoggerService | Logger | common/logger.h | None |
| ✅ ConfigService | Config | - | Logger |
| ⏳ ChainstateService | Chainstate | consensus/chainstate.h | Logger, Config |
| ⏳ MempoolService | Mempool | daemon/mempool.h | Chainstate, Logger |
| ⏳ WalletService | WalletManager | wallet/wallet_manager.h | Chainstate, Logger, Config |
| ⏳ P2PService | P2PManager | daemon/p2p_network.h | Chainstate, Mempool, Logger |
| ⏳ RPCService | RPCServer | daemon/rpc_server.h | All above |
| ⏳ MiningService | MiningCoordinator | daemon/mining.h | Chainstate, Mempool, Logger |
| ⏳ MetricsService | Metrics | - | Logger |

## Benefits

**Immediate Benefits**:
- No changes to existing component code
- Can test each service independently
- Easy to revert if issues found
- Old and new code coexist

**Long-term Benefits**:
- Clear migration path
- Deterministic initialization order
- No more static initialization bugs
- Testable with dependency injection

## Next Steps

1. ✅ Create service wrapper template (this document)
2. ⏳ Create ChainstateService (first proof-of-concept)
3. Test ChainstateService in isolation
4. Create remaining 8 service wrappers
5. Verify all services start/stop cleanly
6. Update main.cpp to use DaemonApp

**Timeline**: Week 1 (5 working days)

---

**See Also**:
- `docs/ARCHITECTURE_MIGRATION_PLAN.md` - Full 5-week plan
- `include/core/iservice.h` - IService interface
- `include/daemon/daemon_context.h` - Dependency container
- `include/daemon/daemon_app.h` - Lifecycle manager
