# Regtest Integration Guide

## Current Status

**Regtest RPC handlers are currently DISABLED** to maintain build stability and focus on production mainnet flow.

## Why Regtest Was Disabled

The regtest handlers (`generatetoaddress`, etc.) use `RpcRegistry` with `ExecutionContext`, but the mainnet daemon uses `HttpRpcServer` with a different handler signature. Integrating these two systems requires an adapter layer, which adds complexity before proving the need for regtest.

## Benefits of Keeping Regtest Disabled (Current State)

- **Stability**: Fewer moving parts, simpler codebase
- **Security**: Dev RPCs like `generatetoaddress` never exposed
- **Simplicity**: One build, one config, easier to reason about
- **Focus**: All engineering effort on production flow (GBT → submitblock)

## When to Enable Regtest

Enable regtest when you need:

1. **Fast CI/CD testing**: Mine blocks instantly in GitHub Actions
2. **Protocol validation**: Test coinbase construction, BIP34, merkle trees before mainnet
3. **Wallet testing**: Fund test wallets instantly to test signing/spending
4. **Edge case coverage**: Test reorgs, difficulty changes, timestamp drift without real mining

## How to Enable Regtest (Future)

### Step 1: Create RPC Adapter Layer

The regtest handlers in `src/daemon/rpc/MiningExtrasHandlers.cpp` expect:
```cpp
using RpcHandler = std::function<din::Json(const ExecutionContext&, const din::Json&)>;
void registerMiningExtras(RpcRegistry& registry, dinero::ChainDB* chaindb, dinero::WalletManager* wallet);
```

But `HttpRpcServer` expects:
```cpp
using RpcHandler = std::function<Json::Value(const Json::Value&)>;
void register_method(const std::string& method, RpcHandler handler);
```

Create an adapter in `src/daemon/rpc/rpc_adapter.h`:

```cpp
#pragma once
#include "http_rpc_server.h"
#include "rpc/rpc_registry.h"

class RpcAdapter {
public:
    // Wrap RpcRegistry handlers for HttpRpcServer
    static HttpRpcServer::RpcHandler adapt(RpcHandler registry_handler) {
        return [registry_handler](const Json::Value& params) -> Json::Value {
            ExecutionContext ctx; // Empty context for regtest
            return registry_handler(ctx, params);
        };
    }

    // Register RpcRegistry methods to HttpRpcServer
    static void bridgeRegistration(
        HttpRpcServer& http_server,
        const std::string& method,
        RpcHandler registry_handler
    ) {
        http_server.register_method(method, adapt(registry_handler));
    }
};
```

### Step 2: Update MiningExtrasHandlers

Modify `src/daemon/rpc/MiningExtrasHandlers.h` to add an HttpRpcServer-compatible registration function:

```cpp
// Bridge function for HttpRpcServer
void registerMiningExtrasHttp(
    HttpRpcServer& server,
    dinero::ChainDB* chaindb,
    dinero::WalletManager* wallet
);
```

In `MiningExtrasHandlers.cpp`:

```cpp
#include "rpc_adapter.h"

void registerMiningExtrasHttp(
    HttpRpcServer& server,
    dinero::ChainDB* chaindb,
    dinero::WalletManager* wallet
) {
    // Adapt each handler from RpcRegistry format to HttpRpcServer format

    server.register_method("generatetoaddress",
        RpcAdapter::adapt([chaindb](const ExecutionContext& ctx, const din::Json& params) -> din::Json {
            // ... existing generatetoaddress implementation ...
        })
    );

    // Repeat for other handlers (mining.getaddress, mining.setaddress, etc.)
}
```

### Step 3: Enable in main.cpp

In `src/daemon/main.cpp`, replace the disabled section with:

```cpp
// Register regtest-only RPC handlers (generatetoaddress, etc.)
// Only enable these on regtest to avoid accidental use on mainnet/testnet
if (config.regtest) {
    registerMiningExtrasHttp(*rpc_server, dinero::g_chain_db_direct, nullptr);
    dinero::g_logger.info("[regtest] Regtest RPC handlers registered (generatetoaddress, etc.)");
}
```

### Step 4: Test Isolation

Before deploying:

1. **Verify regtest isolation**:
   - Test with `--regtest` flag → `generatetoaddress` should work
   - Test without flag → `generatetoaddress` should return "method not found"

2. **Verify datadir separation**:
   - Regtest uses `<datadir>/regtest/`
   - Mainnet uses `<datadir>/`
   - No cross-contamination

3. **Verify HRP/port separation**:
   - Regtest: `rdin` addresses, RPC 20996 / P2P 21001
   - Mainnet: `din` addresses, RPC 20998 / P2P 20999

## Files Involved

- `src/daemon/rpc/MiningExtrasHandlers.cpp`: Regtest RPC handler implementations
- `src/daemon/rpc/MiningExtrasHandlers.h`: Handler registration interface
- `src/daemon/main.cpp`: Conditional registration based on `config.regtest`
- `include/rpc/rpc_registry.h`: RpcRegistry interface (for regtest handlers)
- `src/daemon/http_rpc_server.h`: HttpRpcServer interface (for mainnet)

## Architecture Notes

**Why two RPC systems?**
- `RpcRegistry` (used by regtest handlers): Supports ExecutionContext for per-request metadata
- `HttpRpcServer` (used by mainnet): Simplified interface for production

**Long-term**: Consider migrating all handlers to HttpRpcServer to eliminate the dual-system complexity.

## Testing Regtest When Enabled

```bash
# Start regtest node
./build/dinerod --regtest --datadir=/tmp/regtest_test

# Generate blocks
curl -s -X POST http://127.0.0.1:20996 \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"generatetoaddress","params":[101, "rdin1q0gqj8ush5026e5q97jkw2mhxg3sngjjlxcpjzn"]}'

# Verify chain height
curl -s -X POST http://127.0.0.1:20996 \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"test","method":"getblockcount","params":[]}'
```

## Summary

**Current approach**: Disable regtest, focus on mainnet stability.

**Future approach**: When CI/testing needs arise, create an adapter layer to bridge RpcRegistry (regtest) to HttpRpcServer (mainnet), enable with runtime guards, and test thoroughly in isolation.

This gives you stability today and flexibility tomorrow.
