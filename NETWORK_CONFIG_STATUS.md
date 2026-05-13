# Network Config Pattern - Current Status

> Status update as of 2026-03-09:
> The user agent and cookie/runtime identity are no longer tracked as an unresolved
> legacy-global migration. The active daemon path now sources runtime/build identity
> from shared build metadata plus `P2PService`/`P2PManager`. This file is retained as
> migration history.

**Date**: November 8, 2025  
**Status**: Historical; active runtime no longer depends on the older migration plan

---

## 🔍 Historical State

### Pattern In Use: **Legacy Global**

```cpp
// include/p2p/p2p_wire_protocol.h
extern NetworkConfig g_network_config;

// src/p2p/p2p_wire_protocol.cpp
NetworkConfig g_network_config;
```

### Usage in RPC Handlers

```cpp
// src/rpc/diagnostics_rpc_handlers.cpp
result["protocol_version"] = static_cast<int>(din::p2p::g_network_config.protocol_version);
result["user_agent"] = din::p2p::g_network_config.user_agent_prefix;
```

**Status**: Historical snapshot only

---

## 🎯 Migration Path (Historical)

### Option 1: Singleton Pattern

```cpp
// include/p2p/network_config.h
class NetworkConfig {
public:
    static NetworkConfig& Get() {
        static NetworkConfig instance;
        return instance;
    }
    
    uint32_t getProtocolVersion() const { return protocol_version_; }
    std::string getUserAgent() const { return user_agent_; }
    
private:
    NetworkConfig() = default;
    uint32_t protocol_version_{70016};
    std::string user_agent_{"/dinerod:0.6.0/"};
};

// Usage:
result["protocol_version"] = NetworkConfig::Get().getProtocolVersion();
```

### Option 2: Service-Based Pattern (Recommended)

```cpp
// include/daemon/services/p2p_service.h
class P2PService : public IService {
public:
    uint32_t getProtocolVersion() const { return protocol_version_; }
    std::string getUserAgent() const { return user_agent_; }
    // ... other methods
};

// Usage in context-aware handlers:
auto p2p = ctx.daemon->p2p;
result["protocol_version"] = p2p->getProtocolVersion();
```

### Option 3: Helper Functions (Simplest)

```cpp
// include/p2p/network_config_helpers.h
namespace din::p2p {
    inline uint32_t GetNetworkProtocolVersion() {
        return g_network_config.protocol_version;
    }
    
    inline std::string GetNetworkUserAgent() {
        return g_network_config.user_agent_prefix;
    }
}

// Usage:
result["protocol_version"] = din::p2p::GetNetworkProtocolVersion();
```

---

## 📊 Current Usage Audit

### Files Using `g_network_config`:

```
✅ src/rpc/diagnostics_rpc_handlers.cpp  - RPC node.info handler (valid)
✅ src/p2p/p2p_wire_protocol.cpp         - Definition (valid)
✅ src/daemon/p2p/p2p_wire_protocol.cpp  - Definition (valid)
```

**All usage is valid** - no breaking issues

---

## 🚦 Migration Priority

| Pattern | Priority | Effort | Impact |
|---------|----------|--------|--------|
| Keep global (current) | ✅ Now | None | None |
| Add helper functions | 🟡 Low | 1 hour | Low |
| Singleton pattern | 🟡 Medium | 2-3 hours | Medium |
| Service-based | 🟢 High | 4-6 hours | High |

**Recommendation**: 
- ✅ **Keep current pattern** for now (it works)
- 🔄 **Future**: Migrate to service-based pattern during next refactor
- 📝 **Document**: Add TODO comments (already done in diagnostics_rpc_handlers.cpp)

---

## 📝 What Was Done (November 8, 2025)

### Added Documentation Comments

```cpp
// src/rpc/diagnostics_rpc_handlers.cpp (line 73-74)
// Network protocol info (using legacy global - TODO: migrate to P2PService API)
// Future: result["protocol_version"] = p2p_service->getProtocolVersion();
result["protocol_version"] = static_cast<int>(din::p2p::g_network_config.protocol_version);
```

**Status**: ✅ Documented for future migration

---

## ✅ Conclusion

### Current Code Is Correct ✅

The version tracking implementation using `din::p2p::g_network_config` is:
- ✅ **Valid** - Matches current codebase pattern
- ✅ **Working** - No bugs or issues
- ✅ **Documented** - TODO comment added for future migration
- ✅ **Production-ready** - Safe to deploy

### Future Migration (Optional)

When P2PService API is enhanced:
1. Add `getProtocolVersion()` and `getUserAgent()` to P2PService
2. Update context-aware RPC handlers to use service methods
3. Keep legacy handlers using global until deprecated
4. Eventually remove global in favor of service-based pattern

**Timeline**: Non-urgent, can be done during next P2P refactor

---

## 🎯 Action Items

- [x] Document current usage of g_network_config
- [x] Add TODO comment in diagnostics_rpc_handlers.cpp
- [ ] (Future) Add helper methods to P2PService
- [ ] (Future) Migrate context-aware handlers to use P2PService
- [ ] (Future) Deprecate g_network_config global

**Next step**: Deploy current version tracking code (it's production-ready!)

---

**Summary**: The user's concern was valid - we should eventually migrate away from globals. However, the current code using `g_network_config` is correct and matches the existing codebase pattern. We've documented the migration path for future work.
