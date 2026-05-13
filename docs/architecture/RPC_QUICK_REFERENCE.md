# vNext RPC Quick Reference Card

## 🚀 Quick Start: Adding a New RPC Method

### Step 1: Implement Your Handler

```cpp
// In src/rpc/my_feature.cpp
namespace din {
namespace rpc {

din::Json my_method_impl(const ExecutionContext& ctx, const din::Json& params) {
    // Your implementation here
    din::Json result;
    result["status"] = "success";
    return result;
}

} // namespace rpc
} // namespace din
```

### Step 2: Register in RpcRegistry

```cpp
// In the same file or a registration function
extern RpcRegistry g_rpcRegistry;

void registerMyFeatureRPC() {
    g_rpcRegistry.registerHandler("my.method",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return din::rpc::my_method_impl(ctx, params);
        },
        "my_feature");  // Owner tag
}
```

### Step 3: Call Registration at Daemon Startup

```cpp
// In src/daemon/main.cpp (inside main() function)

// Add to vNext RPC Registration section:
registerMyFeatureRPC();  // Your new registration

// The adapter will automatically expose it via HTTP:
RpcAdapter adapter(rpc_server.get());
adapter.bind(g_rpcRegistry);
```

### That's It! 🎉

Your method now works across:
- ✅ HTTP JSON-RPC
- ✅ WebSocket (when migrated)
- ✅ Future transports (mobile, gRPC, etc.)

---

## 📋 Common Patterns

### Pattern 1: Method with Parameters

```cpp
din::Json transfer_impl(const ExecutionContext& ctx, const din::Json& params) {
    // Validate required parameters
    if (!params.isMember("to_address")) {
        throw std::runtime_error("Missing 'to_address' parameter");
    }
    if (!params.isMember("amount")) {
        throw std::runtime_error("Missing 'amount' parameter");
    }

    std::string to_address = params["to_address"].asString();
    int64_t amount = params["amount"].asInt64();

    // Your logic here
    din::Json result;
    result["txid"] = "abc123...";
    return result;
}
```

### Pattern 2: Method Using ExecutionContext

```cpp
din::Json getbalance_impl(const ExecutionContext& ctx, const din::Json& params) {
    // Use wallet from context
    std::string wallet_name = ctx.walletName;
    if (wallet_name.empty()) {
        wallet_name = "default";
    }

    // Your logic here
    int64_t balance = get_wallet_balance(wallet_name);

    din::Json result;
    result["balance"] = balance;
    result["wallet"] = wallet_name;
    return result;
}
```

### Pattern 3: Method with Metadata

```cpp
// Register with full metadata for auto-generated docs
RpcMethodMeta meta;
meta.name = "wallet.getbalance";
meta.ns = "wallet";
meta.description = "Get the current wallet balance";

RpcParamMeta param1;
param1.name = "wallet_name";
param1.type = "string";
param1.desc = "Name of the wallet (default: 'default')";
param1.required = false;
meta.params.push_back(param1);

meta.result.type = "object";
meta.result.desc = "Balance information";

g_rpcRegistry.registerHandler("wallet.getbalance", handler, meta, "wallet");
```

---

## 🧪 Testing Your RPC Method

### Unit Test (No HTTP Needed!)

```cpp
// In tests/test_my_rpc.cpp
#include <gtest/gtest.h>
#include "rpc/rpc_registry.h"

extern RpcRegistry g_rpcRegistry;

TEST(MyRpcTest, BasicFunctionality) {
    // Setup
    ExecutionContext ctx;
    ctx.walletName = "test_wallet";

    din::Json params;
    params["amount"] = 100;

    // Execute
    din::Json result = g_rpcRegistry.invoke("my.method", ctx, params);

    // Verify
    EXPECT_EQ(result["status"].asString(), "success");
}
```

### Integration Test (With HTTP)

```cpp
TEST(MyRpcIntegration, HttpEndpoint) {
    // Start daemon with test config
    // ...

    // Make HTTP request
    auto response = http_client.post(
        "http://127.0.0.1:18998",
        R"({"jsonrpc":"2.0","method":"my.method","params":{"amount":100},"id":1})"
    );

    // Verify response
    EXPECT_EQ(response.status_code, 200);
}
```

---

## 🔍 Debugging Checklist

### Method Not Found?

1. ✅ Check registration is called at daemon startup
2. ✅ Verify method name matches exactly (case-sensitive)
3. ✅ Ensure `RpcAdapter.bind()` is called after registration
4. ✅ Check daemon logs for registration messages

### Method Registered But Not Working?

1. ✅ Check handler signature matches `RpcHandler` type
2. ✅ Verify parameters are parsed correctly
3. ✅ Add logging inside your handler to debug
4. ✅ Test with direct registry invocation first

### Build Errors?

1. ✅ Include `rpc/rpc_registry.h` in your file
2. ✅ Declare `extern RpcRegistry g_rpcRegistry;` before use
3. ✅ Add your .cpp file to CMakeLists.txt
4. ✅ Check for missing dependencies

---

## 📊 Comparison: Legacy vs vNext

### Adding "wallet.send" Method

#### Legacy Way (❌ Don't Use)

```cpp
// In main.cpp (messy, embedded in startup)
rpc_server->register_method("wallet.send", [&](const Json::Value& params) {
    // Implementation mixed with registration
    std::string to = params["to"].asString();
    int64_t amount = params["amount"].asInt64();

    // Wallet logic here...
    return result;
});
```

**Problems:**
- Mixed concerns (registration + logic)
- Hard to test (needs HTTP server)
- Can't reuse for WebSocket
- Clutters main.cpp

#### vNext Way (✅ Use This)

```cpp
// In src/rpc/wallet_methods.cpp (clean separation)
din::Json wallet_send_impl(const ExecutionContext& ctx, const din::Json& params) {
    std::string to = params["to"].asString();
    int64_t amount = params["amount"].asInt64();

    // Wallet logic here...
    return result;
}

void registerWalletRPC() {
    g_rpcRegistry.registerHandler("wallet.send",
        [](const ExecutionContext& ctx, const din::Json& params) {
            return wallet_send_impl(ctx, params);
        },
        "wallet");
}

// In main.cpp (one line!)
registerWalletRPC();
```

**Benefits:**
- Clean separation of concerns
- Easy to test (call `wallet_send_impl` directly)
- Works for HTTP, WebSocket, mobile automatically
- Organized by feature

---

## 🎨 Code Organization Best Practices

### Recommended File Structure

```
src/rpc/
├── wallet_methods.cpp        # Wallet RPC implementations
├── mining_methods.cpp        # Mining RPC implementations
├── chain_methods.cpp         # Blockchain query RPCs
├── methods_hardware_wallet.cpp  # Hardware wallet RPCs (EXAMPLE)
├── rpc_registry.cpp          # Registry implementation
└── rpc_adapter.cpp           # Adapter implementation

include/rpc/
├── wallet_methods.h          # Registration functions
├── mining_methods.h
├── chain_methods.h
├── methods_hardware_wallet.h
├── rpc_registry.h
└── rpc_adapter.h

docs/architecture/
├── RPC_MIGRATION_GUIDE.md    # This document's big brother
└── RPC_QUICK_REFERENCE.md    # This document
```

### Naming Conventions

**Method Names:**
- Use namespace prefixing: `wallet.send`, `mining.start`, `chain.getblock`
- Lowercase with dots: ✅ `wallet.getbalance`
- NOT CamelCase: ❌ `walletGetBalance`

**Handler Functions:**
- Suffix with `_impl`: `wallet_send_impl`, `getbalance_impl`
- Match RPC name: `wallet.send` → `wallet_send_impl`

**Registration Functions:**
- Prefix with `register`: `registerWalletRPC`, `registerMiningRPC`
- Group by feature: One registration function per feature area

**Owner Tags:**
- Lowercase module name: `"wallet"`, `"mining"`, `"hardware_wallet"`
- Used for debugging and tracking

---

## 🚦 Migration Checklist

Converting legacy RPC to vNext:

- [ ] Extract handler logic into separate `_impl` function
- [ ] Create registration function that uses `g_rpcRegistry`
- [ ] Call registration function in `main.cpp` (vNext section)
- [ ] Remove old `rpc_server->register_method()` call
- [ ] Update tests to use direct registry invocation
- [ ] Verify method works via HTTP using dinero-cli
- [ ] Add to migration tracking document

---

## 📞 Getting Help

### Documentation
- Full guide: `docs/architecture/RPC_MIGRATION_GUIDE.md`
- API reference: `docs/api/RPC_API.md`

### Code Examples
- Hardware wallet migration: `src/rpc/methods_hardware_wallet.cpp`
- WebSocket handlers: `src/daemon/rpc/websocket_handlers.cpp` (being migrated)

### Common Issues
- Method not found → Check registration order
- Build errors → Check includes and CMakeLists.txt
- Test failures → Use direct registry invocation

---

**Quick Reference Version:** 1.0
**Last Updated:** 2025-11-02
**Format:** Cheat Sheet / Quick Reference
