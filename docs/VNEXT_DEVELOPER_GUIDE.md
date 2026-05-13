# vNext Developer Guide

## Overview

DineroCoin has transitioned to a **vNext-only** architecture. The legacy RPC server has been completely removed and replaced with a modern, secure, and maintainable vNext system.

## Key Changes

### ✅ What's New in vNext

- **Modern HTTP/JSON-RPC**: Uses Beast HTTP server with proper JSON-RPC 2.0
- **Structured Logging**: JSON-formatted logs with trace IDs
- **Enhanced Security**: Cookie-based authentication, proper CORS handling
- **Clean Architecture**: Single source of truth for RPC methods
- **Better Error Handling**: Consistent error codes and messages

### ❌ What's Removed (Legacy)

- **Legacy RPC Server**: `RPCServer` class and `g_rpc_server` global
- **WebSocket Library**: `din_ws` dependency
- **Duplicate Handlers**: Multiple implementations of the same RPC methods
- **Unified Ports**: JSON-RPC and HTTP both use port 20999 (no separate RPC port)

## Development Setup

### Building vNext-Only

```bash
# Default build (vNext-only)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dinerod

# Explicit vNext-only with strict mode
cmake -S . -B build \
  -DDIN_ENABLE_LEGACY_RPC=OFF \
  -DDIN_STRICT_RPC=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dinerod
```

### Running the Daemon

```bash
# Start daemon (JSON-RPC and HTTP both on port 20999)
./build/bin/dinerod --regtest --datadir=/tmp/dinero-test --httpport=20999

# Check health (HTTP endpoint)
curl http://127.0.0.1:20999/healthz

# Make JSON-RPC calls (with auth, same port)
COOKIE=$(cut -d: -f2 /tmp/dinero-test/regtest/.cookie)
AUTH="Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)"
curl -X POST -H "Content-Type: application/json" -H "$AUTH" \
  -d '{"jsonrpc":"2.0","method":"help","params":[],"id":1}' \
  http://127.0.0.1:20999/
```

## Adding New RPC Methods

### 1. Register in vNext Registry

Add your method to `src/daemon/main.cpp` in the vNext registration section:

```cpp
g_rpcRegistry.registerHandler("your.method", [](const ::ExecutionContext& ctx, const din::Json& params) -> din::Json {
    // Your implementation here
    return result;
});
```

### 2. Follow vNext Patterns

- Use `din::Json` for parameters and return values
- Include proper error handling with `throw std::runtime_error("descriptive message")`
- Add to the `help` method response
- Include in tests

### 3. Never Use Legacy Patterns

❌ **Don't do this:**
```cpp
// Legacy patterns - these are forbidden
#include "daemon/rpc_server.h"  // Without #if DIN_ENABLE_LEGACY_RPC
extern std::unique_ptr<dinero::RPCServer> g_rpc_server;
g_rpc_server->registerMethod("method", handler);
```

✅ **Do this instead:**
```cpp
// vNext patterns
g_rpcRegistry.registerHandler("method", [](const ::ExecutionContext& ctx, const din::Json& params) -> din::Json {
    // Implementation
});
```

## Testing

### Smoke Test

Run the comprehensive smoke test:

```bash
./scripts/vnext-smoke-test.sh
```

### CI Validation

The CI automatically validates:
- No legacy symbols in source code
- vNext-only mode confirmation
- RPC surface integrity
- Auth enforcement
- Mining functionality

### Manual Testing

```bash
# Start daemon
./build/bin/dinerod --regtest --datadir=/tmp/test --httpport=20999

# Test health
curl http://127.0.0.1:20999/healthz | jq '.rpc_mode'

# Test RPC
COOKIE=$(cut -d: -f2 /tmp/test/regtest/.cookie)
AUTH="Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)"
curl -X POST -H "Content-Type: application/json" -H "$AUTH" \
  -d '{"jsonrpc":"2.0","method":"getbuildinfo","params":[],"id":1}' \
  http://127.0.0.1:20999/ | jq '.result.rpc_mode'
```

## Troubleshooting

### Build Issues

**Error: "Legacy RPC header included with DIN_ENABLE_LEGACY_RPC=OFF"**
- Solution: Guard the include with `#if DIN_ENABLE_LEGACY_RPC`

**Error: "Duplicate RPC method registration"**
- Solution: Remove duplicate registrations or use `DIN_STRICT_RPC=OFF` for development

### Runtime Issues

**Error: "Method not found"**
- Check if the method is registered in the vNext registry
- Verify the method name matches exactly

**Error: "Unauthorized"**
- Ensure you're using the correct cookie authentication
- Check that the daemon is running and accessible

## Migration Guide

### From Legacy to vNext

If you have legacy RPC code:

1. **Remove legacy includes**:
   ```cpp
   // Remove
   #include "daemon/rpc_server.h"
   
   // Add guard if needed
   #if DIN_ENABLE_LEGACY_RPC
   #include "daemon/rpc_server.h"
   #endif
   ```

2. **Update method registration**:
   ```cpp
   // Legacy
   g_rpc_server->registerMethod("method", handler);
   
   // vNext
   g_rpcRegistry.registerHandler("method", [](const ::ExecutionContext& ctx, const din::Json& params) -> din::Json {
       return handler(ctx, params);
   });
   ```

3. **Update parameter handling**:
   ```cpp
   // Legacy
   Json::Value result = handler(params);
   
   // vNext
   din::Json result = handler(ctx, params);
   ```

## Contributing

### Pre-commit Hooks

Install pre-commit hooks to catch legacy symbols:

```bash
pip install pre-commit
pre-commit install
```

### Code Review Checklist

- [ ] No legacy symbols (`RPCServer`, `g_rpc_server`, `din_ws`)
- [ ] Proper error handling with descriptive messages
- [ ] Method registered in vNext registry
- [ ] Added to help response
- [ ] Tests included
- [ ] Documentation updated

### CI Requirements

All PRs must pass:
- vNext-only build
- Legacy symbol scan
- Health check validation
- RPC surface validation
- Auth enforcement test
- Mining verification test

## Architecture

### vNext Components

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│   HTTP Client   │───▶│  Beast Server    │───▶│  RPC Registry   │
└─────────────────┘    └──────────────────┘    └─────────────────┘
                                │                        │
                                ▼                        ▼
                       ┌──────────────────┐    ┌─────────────────┐
                       │  Auth Middleware │    │  RPC Handlers   │
                       └──────────────────┘    └─────────────────┘
                                │                        │
                                ▼                        ▼
                       ┌──────────────────┐    ┌─────────────────┐
                       │  Execution Ctx   │    │  Business Logic │
                       └──────────────────┘    └─────────────────┘
```

### Key Files

- `src/daemon/main.cpp` - vNext RPC registration
- `src/rpc/rpc_registry.cpp` - RPC method registry
- `src/http/beast_server.cpp` - HTTP server implementation
- `src/daemon/execution_context.h` - Request context
- `.github/workflows/vnext-only.yml` - CI validation

## Address API

The Address API provides Bech32 address validation and decoding functionality.

### `wallet.validateaddress`

Validates a Bech32 address and returns detailed information about its structure.

**Parameters:**
- `address` (string): The Bech32 address to validate

**Response:**
```json
{
  "address": "rdin1q...",
  "isvalid": true,
  "iswitness": true,
  "isscript": false,
  "witness_version": 0,
  "witness_program": "68f926c852932c64715f5fef05cdafba74eaf3f1",
  "ismine": true,
  "account": "default"
}
```

**Fields:**
- `address`: The input address
- `isvalid`: Whether the address is valid Bech32 format
- `iswitness`: Whether this is a witness address (always true for valid addresses)
- `isscript`: Whether this is a P2WSH script (false for P2WPKH, true for P2WSH)
- `witness_version`: Witness version (0 for P2WPKH/P2WSH, >=1 for Bech32m)
- `witness_program`: 20-byte (P2WPKH) or 32-byte (P2WSH) witness program in hex
- `ismine`: Whether the address belongs to the active wallet
- `account`: Wallet account name (if `ismine` is true)

**Validation Rules:**
- HRP must match the active network (regtest: "rdin", testnet: "tdin", mainnet: "din")
- Case-sensitive: mixed-case addresses are rejected
- Witness version 0 only: Bech32m (witver >= 1) is explicitly rejected
- Program length: 20 bytes (P2WPKH) or 32 bytes (P2WSH) for witness version 0
- Checksum validation: Invalid checksums are rejected

**Examples:**
```bash
# Valid P2WPKH address
curl -X POST -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["rdin1qtest1234567890123456789012345678901234567890"],"id":1}' \
  http://127.0.0.1:20999/

# Invalid address (wrong HRP)
curl -X POST -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["din1qtest1234567890123456789012345678901234567890"],"id":1}' \
  http://127.0.0.1:20999/
```

**Error Handling:**
- Invalid addresses return `isvalid: false`
- Malformed addresses return `null` for all fields
- Network mismatches are rejected with `isvalid: false`

## A→Z End-to-End Testing

### Running the A→Z Test

The comprehensive A→Z test validates the entire vNext system from build to broadcast:

```bash
# Run locally
bash scripts/run-a2z.sh

# With nightly soak testing (1000 blocks + 50 PSBT loops)
NIGHTLY_SOAK=true bash scripts/run-a2z.sh
```

### What A→Z Tests

The A→Z test provides **hermetic, self-cleaning validation** of:

- **Build**: vNext-only compilation with strict flags
- **Daemon**: Startup, health check, unified port (20999)
- **Auth**: Cookie authentication, unauthorized rejection
- **RPC Surface**: 70+ methods, no duplicates, no legacy aliases
- **Wallet**: Create/load, address generation, balance tracking
- **Mining**: Block generation, coinbase maturity (105 blocks)
- **UTXO Validation**: Zero placeholders, P2WPKH structure, OP_0+PUSH20 prefix
- **PSBT Flow**: Create → fund → sign → finalize → extract → broadcast
- **Address Stack**: Unique witness programs, Bech32 validation corpus
- **Error Handling**: Invalid methods, malformed transactions
- **DB Integrity**: Schema validation, foreign key constraints
- **Performance**: Timing for startup, mining, PSBT operations

### Reading A→Z Output

```bash
==> Build dinerod (vNext-only + strict)
Using existing binary: ./build/bin/dinerod

==> Launch daemon (unified JSON-RPC+HTTP=20999)
⏱️ Startup time: 3s

==> Health check
true

==> RPC surface (help, count, no legacy, no dupes)
Method count: 70

==> Mine 105 blocks to mature coinbase
⏱️ Mining time: 12s

==> UTXO placeholder guard
✅ 0 zero placeholders
✅ DEADBEEF only in premine (height=1)

==> PSBT create → fund → sign → finalize → extract → broadcast
Broadcast TXID: 4bf691cb2a466fa68d4e27c25b3ab5482288f08963d6d675d919667f015a83f5
⏱️ PSBT round-trip time: 2s

==> Address generation & validation
✅ 5 unique addresses with valid witness programs

==> Bech32 validation corpus
✅ Bech32 validation corpus passed

==> Performance summary
📊 Total A→Z time: 25s
   - Startup: 3s
   - Mining: 12s
   - PSBT: 2s

✅ All A→Z checks passed.
```

### Troubleshooting A→Z Failures

**Build Failures:**
- Check CMake configuration: `-DDIN_ENABLE_LEGACY_RPC=OFF -DDIN_STRICT_RPC=ON`
- Ensure clean working tree or use `-DALLOW_DIRTY=ON`

**Daemon Startup Failures:**
- Check port availability (20999)
- Verify binary exists: `./build/bin/dinerod`
- Check logs in `/tmp/din-a2z-*/regtest/daemon.log`

**RPC Surface Failures:**
- Duplicate registrations: Check for multiple handler registrations
- Method count mismatch: Verify all vNext methods are registered
- Legacy aliases: Ensure no `legacy_*` methods in output

**UTXO Validation Failures:**
- Placeholder scripts: Check mining address generation
- Wrong structure: Verify P2WPKH format (44 hex chars, 0014 prefix)
- DEADBEEF outside premine: Should only appear at height=1

**PSBT Failures:**
- Insufficient funds: Ensure coinbase maturity (105+ blocks)
- Invalid PSBT: Check wallet synchronization
- Broadcast failure: Verify transaction validity

**Address Validation Failures:**
- Non-unique witness programs: Check HD wallet derivation
- Invalid addresses: Verify Bech32 encoding
- Wrong HRP: Should be `rdin` for regtest

### CI Integration

A→Z runs automatically on:
- **Push/PR**: Linux + macOS with Release and ASan/UBSan builds
- **Nightly**: Extended soak testing with 1000 blocks + 50 PSBT loops

**Merge Gate**: A→Z must pass green before merging core changes.

## Support

For questions or issues:
1. Check this guide first
2. Run the A→Z test: `bash scripts/run-a2z.sh`
3. Check CI logs for validation failures
4. Review daemon logs in test output
5. Open an issue with detailed error information

---

**Remember**: Legacy RPC is permanently disabled. All new development must use the vNext architecture.
