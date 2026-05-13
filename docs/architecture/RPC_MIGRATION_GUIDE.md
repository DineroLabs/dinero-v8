# DineroCoin RPC Architecture: Legacy to vNext Migration Guide

## 📬 The Post Office Analogy

Understanding the transition from legacy RPC to vNext is easier with a real-world analogy.

### 🏢 The Old Post Office (Legacy RPC)

Imagine you built your first post office — the **HttpRpcServer**.

It works fine:
- People write letters (JSON requests)
- Drop them off at the post office
- The post office delivers them to the right office (wallet, mining, or chain code)

**But there are limitations:**

1. **Single Building Problem**
   - There's only one building — it handles all letters (HTTP only)
   - Can't send letters by WebSocket, Bluetooth, or any new method
   - The building only understands paper mail

2. **Manual Wiring**
   - Every time you add a new kind of letter (a new RPC command), you must change the building's wiring manually
   - Adding `getbalance`? Edit the main building blueprints
   - Adding `submitblock`? More manual wiring

3. **Testing Difficulty**
   - Testing the post office is hard
   - You have to actually walk there and post a letter just to see if it works
   - No way to test in isolation

4. **Code Organization**
   - It's like an old, busy post office: functional, but messy inside
   - Duplicate code for each transport type
   - Hard to maintain and extend

### 🏙️ The New System (vNext RpcRegistry)

Now imagine you build a **central address book for the whole city** — the `RpcRegistry`.

**How it works:**

1. **Central Registration**
   - Every department (wallet, mining, web) registers what messages it can handle in one place
   - Instead of every building keeping its own address book, everyone checks one shared registry

2. **Multiple Modern Post Offices**
   - You can build multiple delivery systems — HTTP, WebSocket, Bluetooth, mobile
   - They all use the same address book
   - No need to rewire anything

3. **Plug-and-Play**
   - New post offices just "plug in" to the central registry
   - No code duplication
   - Add once, works everywhere

### 🔌 The Universal Translator (RpcAdapter)

The `RpcAdapter` is like a **universal translator** at each post office:

```
┌─────────────────────────────────────────┐
│     Central Address Book                │
│     (RpcRegistry)                       │
│  - getbalance → handler                 │
│  - submitblock → handler                │
│  - exportpsbttofile → handler           │
└─────────────────┬───────────────────────┘
                  │
          ┌───────┴────────┐
          │  RpcAdapter    │  ← Translates for each transport
          │  (Translator)  │
          └───────┬────────┘
                  │
    ┌─────────────┼─────────────┐
    ▼             ▼             ▼
┌────────┐   ┌─────────┐   ┌──────────┐
│  HTTP  │   │WebSocket│   │  Mobile  │
│  Post  │   │  Post   │   │   API    │
│ Office │   │ Office  │   │ (future) │
└────────┘   └─────────┘   └──────────┘
```

**The Adapter ensures:**
- HTTP gets messages in its preferred format
- WebSocket gets messages in its format
- Mobile APIs get type-safe native calls
- **But they all use the same central registry**

---

## 📊 Real Difference in Simple Terms

| Feature | Old Way (Legacy) | New Way (vNext) |
|---------|------------------|-----------------|
| **Registration** | Every transport keeps its own list of commands | All commands live in one shared registry |
| **Testing** | Hard — must start the full daemon, bind ports | Easy — call functions directly in unit tests |
| **Adding Commands** | Manual editing of main.cpp, duplicate code | Modules self-register, no duplication |
| **Transport Support** | HTTP only | HTTP, WebSocket, mobile, gRPC, anything |
| **Code Organization** | Messy, full of duplication | Clean, organized, DRY principle |
| **Future-Proofing** | Locked into HTTP | Ready for any new protocol |

---

## 💻 Code Examples

### Before (Legacy)

```cpp
// In main.cpp - register for HTTP
rpc_server->register_method("exportpsbttofile", [](const Json::Value& params) {
    // Handler implementation
    return exportPsbtToFile(params);
});

// If you want WebSocket support, DUPLICATE everything:
// In websocket_handlers.cpp
ws_server->register_method("exportpsbttofile", [](const Json::Value& params) {
    // SAME handler code, different server!
    return exportPsbtToFile(params);
});

// Want mobile API? DUPLICATE AGAIN:
// In mobile_api.cpp
mobile_api->register_method("exportpsbttofile", [](const Json::Value& params) {
    // SAME handler code, third time!
    return exportPsbtToFile(params);
});
```

**Problem:** 3x the code, 3x the bugs, 3x the maintenance.

### After (vNext)

```cpp
// In methods_hardware_wallet.cpp - register ONCE
g_rpcRegistry.registerHandler("exportpsbttofile",
    [](const ExecutionContext& ctx, const din::Json& params) {
        return din::rpc::exportpsbttofile_impl(ctx, params);
    },
    "hardware_wallet");  // Owner tag for tracking

// In main.cpp - bind to ALL transports automatically
registerHardwareWalletRPC();  // Registers to central registry

RpcAdapter http_adapter(http_server);
http_adapter.bind(g_rpcRegistry);  // HTTP ✅

RpcAdapter ws_adapter(ws_server);
ws_adapter.bind(g_rpcRegistry);    // WebSocket ✅

RpcAdapter mobile_adapter(mobile_api);
mobile_adapter.bind(g_rpcRegistry); // Mobile ✅
```

**Result:** One registration, works everywhere!

---

## 🧪 Testing Improvements

### Legacy Testing

```cpp
// test_rpc_legacy.cpp
TEST(RpcTest, GetBalance) {
    // Must start full HTTP server
    HttpRpcServer server("127.0.0.1", 18998);
    server.start();

    // Must make actual HTTP request
    auto response = http_client.post("http://127.0.0.1:18998",
        R"({"method":"getbalance","params":[]})");

    // Parse response
    Json::Value result;
    Json::Reader reader;
    reader.parse(response.body, result);

    EXPECT_GT(result["balance"].asInt64(), 0);

    server.stop();  // Clean up
}
```

**Problems:**
- ❌ Slow (network overhead)
- ❌ Flaky (port conflicts in CI)
- ❌ Hard to debug (multiple layers)
- ❌ Can't test without full daemon

### vNext Testing

```cpp
// test_rpc_vnext.cpp
TEST(RpcTest, GetBalance) {
    // Direct registry invocation - no HTTP!
    ExecutionContext ctx;
    ctx.walletName = "test_wallet";

    din::Json params;  // Empty params

    // Direct function call
    din::Json result = g_rpcRegistry.invoke("getbalance", ctx, params);

    EXPECT_GT(result["balance"].asInt64(), 0);
}
```

**Benefits:**
- ✅ Fast (no network)
- ✅ Reliable (no ports)
- ✅ Simple (direct call)
- ✅ Works in minimal test environment

---

## 🚀 Real-World Impact: Hardware Wallet Migration

### What We Migrated

We successfully migrated all hardware wallet RPC methods from legacy to vNext:

```cpp
// These methods now work across ALL transports:
✅ exportpsbttofile   - Export PSBT for air-gapped signing
✅ importpsbtfromfile - Import signed PSBT
✅ analyzepsbt        - Analyze PSBT signing status
✅ enumeratehwdevices - List connected USB hardware wallets
```

### Before Migration

```
src/rpc/methods_hardware_wallet.cpp
├── Implementation functions (exportpsbttofile_impl, etc.)
└── registerHardwareWalletHandlers()
    └── Manually binds to HttpRpcServer only

Result: Only works via HTTP JSON-RPC
```

### After Migration

```
src/rpc/methods_hardware_wallet.cpp
├── Implementation functions (exportpsbttofile_impl, etc.)
└── registerHardwareWalletRPC()
    └── Registers to g_rpcRegistry

src/rpc/rpc_adapter.cpp
└── RpcAdapter automatically exposes to:
    ├── HttpRpcServer ✅
    ├── WebSocket (future) ✅
    └── Mobile API (future) ✅

Result: Works across ALL transports automatically!
```

---

## 🎯 Strategic Benefits

### 1. **Extensibility**

**Scenario:** Exchange integration needs gRPC API

- **Old way:** Rewrite all 50+ RPC methods for gRPC (months of work)
- **New way:** Write `GrpcAdapter`, all methods work instantly (days of work)

### 2. **Mobile/Web Integration**

**Scenario:** Build iOS/Android wallet app

- **Old way:** Parse HTTP JSON-RPC 2.0 manually in Swift/Kotlin
- **New way:** Generate type-safe SDK from `RpcRegistry` metadata

### 3. **Testing & CI**

**Scenario:** Add 20 new RPC methods

- **Old way:** Each method needs integration test (slow CI, flaky tests)
- **New way:** Unit test each method directly (fast CI, reliable)

### 4. **Code Maintainability**

**Scenario:** Fix bug in `getbalance` handler

- **Old way:** Fix in HTTP handler, WebSocket handler, mobile API (3 places)
- **New way:** Fix once in registry (1 place)

---

## 📈 Migration Status

### ✅ Phase 1: Infrastructure & Proof of Concept (COMPLETED)

- [x] Created `RpcAdapter` bridge layer
- [x] Migrated hardware wallet RPC methods to vNext
- [x] Integrated with daemon startup
- [x] Validated dual operation (legacy + vNext coexist)
- [x] Build system updated
- [x] Documentation written

**Files Changed:**
- `include/rpc/rpc_adapter.h` - NEW
- `src/rpc/rpc_adapter.cpp` - NEW
- `src/rpc/methods_hardware_wallet.cpp` - MIGRATED
- `src/daemon/main.cpp` - UPDATED
- `CMakeLists.txt` - UPDATED

### ✅ Phase 2: WebSocket RPC Migration (COMPLETED)

- [x] Migrated `registerWebSocketHandlers()` to vNext pattern
- [x] Created `registerWebSocketManagementRPC()` for vNext registry
- [x] Registered 5 WebSocket management methods in `g_rpcRegistry`
- [x] Removed legacy HTTP-specific handler code
- [x] Validated methods work via HTTP through RpcAdapter

**Actual Impact:** ~270 lines of duplicate code eliminated

**Files Changed:**
- `src/daemon/rpc/websocket_handlers.cpp` - Migrated to vNext pattern
- `include/daemon/rpc/websocket_handlers.h` - Updated declarations
- `src/daemon/main.cpp` - Integrated vNext registration

**Methods Migrated:**
- `wsSubscribe` - Subscribe to WebSocket topics
- `wsReplay` - Replay historical events
- `wsGetConnections` - List active connections
- `wsGetTopicStats` - Get topic statistics
- `wsGetStatus` - Get system status

### 🔄 Phase 3: Wallet RPC Migration (IN PROGRESS)

**Challenge Identified**: Wallet RPCs require access to local main() variables (`g_hd_wallet`, `g_wallet_locked`, `g_utxo_set_direct`)

**Scope**: 38+ wallet RPC methods including:
- Core: `getbalance`, `getnewaddress`, `sendtoaddress`, `listunspent`
- HD Wallet: `createhdwallet`, `restorewallet`, `deriveaddress`
- Security: `walletlock`, `walletunlock`, `encryptwallet`
- PSBT: `walletcreatefundedpsbt`, `walletprocesspsbt`, `signrawtransactionwithwallet`
- Import/Export: `dumpprivkey`, `importprivkey`, `dumpwallet`, `importwallet`
- Advanced: `walletrescan`, `backupwallet`, `exportcsv`, `generateqrcode`

**Recommended Strategy**:
1. Refactor wallet globals to be truly global (not local to main())
2. Create dedicated `src/rpc/methods_wallet.cpp` similar to hardware wallet pattern
3. Migrate methods in batches (core → security → advanced)
4. Comprehensive testing at each stage

**Status**: Infrastructure prepared, awaiting architectural refactor

### 📅 Phase 4: Full Migration (FUTURE)

- [ ] Migrate remaining mining RPC methods
- [ ] Migrate blockchain query RPC methods (`getblock`, `getblockchaininfo`, etc.)
- [ ] Migrate mining RPC methods (`getblocktemplate`, `submitblock`, etc.)
- [ ] Migrate chain RPC methods (`getblock`, `getblockchaininfo`, etc.)
- [ ] Remove legacy bridge functions entirely
- [ ] Deprecate direct `HttpRpcServer` registration

**Estimated Impact:** ~1000 lines of duplicate code eliminated

---

## 🧠 Why This Matters for DineroCoin's Future

When DineroCoin grows with:
- Mobile wallets (iOS, Android)
- Hardware wallet integrations (Ledger, Trezor)
- Exchange listings (institutional APIs)
- Block explorers (high-performance gRPC)
- Web wallets (WebSocket real-time updates)

**Every new connection type just "plugs in"** to the central registry.

You don't rewrite code; you just say:

```cpp
g_rpcRegistry.registerHandler("my_new_method", handler);
```

And **boom** — that method now works over:
- HTTP JSON-RPC ✅
- WebSocket streaming ✅
- Mobile native API ✅
- gRPC for exchanges ✅
- Any future protocol ✅

---

## 🚦 In One Sentence

The new system (vNext RpcRegistry) turns your RPCs from **one big tangled box of wires** into a **clean plug-and-play control panel** — easier to expand, easier to test, and ready for the future.

---

## 📚 Additional Resources

### Related Documentation
- `docs/api/RPC_API.md` - Full RPC method reference
- `docs/architecture/ADAPTER_PATTERN.md` - Deep dive on RpcAdapter
- `docs/testing/RPC_TESTING.md` - Testing guide for vNext RPC

### Code References
- `include/rpc/rpc_registry.h` - RpcRegistry interface
- `src/rpc/rpc_adapter.cpp` - Adapter implementation
- `src/rpc/methods_hardware_wallet.cpp` - Migration example

### Design Decisions
- [ADR-001](../adr/001-rpc-registry-migration.md) - Why migrate to vNext
- [ADR-002](../adr/002-adapter-pattern.md) - Adapter design choices

---

## 🤝 Contributing

When adding new RPC methods:

1. **Register in RpcRegistry** (preferred):
   ```cpp
   g_rpcRegistry.registerHandler("my_method", handler, "my_module");
   ```

2. **NOT in HttpRpcServer** (legacy):
   ```cpp
   // ❌ DON'T DO THIS (unless migrating legacy code)
   rpc_server->register_method("my_method", handler);
   ```

This ensures your methods work across all transports automatically!

---

**Document Version:** 1.0
**Last Updated:** 2025-11-02
**Authors:** DineroCoin Core Team
**Status:** Living Document
