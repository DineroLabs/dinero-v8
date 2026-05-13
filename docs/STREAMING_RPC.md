# Streaming RPC System

**Version:** 1.0
**Date:** 2025-11-03
**Status:** ✅ Core Infrastructure Complete

---

## Overview

The Streaming RPC System provides **real-time progress updates** for long-running operations via WebSocket. Unlike traditional blocking RPC calls, streaming RPCs return immediately and send progress events as the operation executes.

**Key Features:**
- 📊 Real-time progress updates (percentage, current/total, custom messages)
- ⏸️ Cancellation support for long-running operations
- 🔄 Automatic operation tracking and cleanup
- 🧵 Thread-safe concurrent operations
- 🎯 Type-safe progress callbacks

**Use Cases:**
- Wallet blockchain rescan with block-by-block progress
- Large transaction broadcasts
- Batch operations (multi-send, batch validation)
- Mining template generation with pre-validation
- Database maintenance operations

---

## Architecture

### Components

```
┌─────────────────────────────────────────────────────────────┐
│                      Client Application                      │
│  (WebSocket connection for real-time progress events)       │
└─────────────────────┬───────────────────────────────────────┘
                      │ WebSocket Protocol
                      ↓
┌─────────────────────────────────────────────────────────────┐
│                   WebSocket Server                           │
│  (Routes RPC calls and broadcasts progress events)          │
└─────────────────────┬───────────────────────────────────────┘
                      │ Send progress events
                      ↓
┌─────────────────────────────────────────────────────────────┐
│              StreamingRpcHandler (Base Class)                │
│  - Progress callback management                              │
│  - Operation lifecycle tracking                              │
│  - Thread-safe state management                              │
│  - Automatic cleanup                                         │
└─────────────────────┬───────────────────────────────────────┘
                      │ Inheritance
                      ↓
┌─────────────────────────────────────────────────────────────┐
│         Specialized Handlers (e.g., WalletRescanHandler)    │
│  - Implements specific long-running operation                │
│  - Calls progress_cb() periodically                          │
│  - Returns final result                                      │
└─────────────────────────────────────────────────────────────┘
```

### Event Flow

```
1. Client sends RPC request via WebSocket
   ↓
2. RPC method returns immediately with operation_id
   ↓
3. Background worker thread starts
   ↓
4. Worker calls progress_cb() periodically
   ↓
5. StreamingRpcHandler sends WebSocket events:
   - "progress" events (current, total, percentage, message)
   - "complete" event with final result
   - "error" event if operation fails
   ↓
6. Client receives real-time updates
   ↓
7. Operation completes and cleans up after 5 seconds
```

---

## API Reference

### Base Classes

#### `StreamingRpcHandler`

Base class for all streaming RPC handlers.

**Constructor:**
```cpp
StreamingRpcHandler(const std::string& operation_id, WebSocketServer* ws_server)
```

**Methods:**
```cpp
// Start a streaming operation
std::string start_operation(
    const std::string& client_id,
    std::function<din::Json(ProgressCallback)> work_fn,
    const din::Json& params = din::Json()
);

// Cancel a running operation
bool cancel_operation(const std::string& op_id);

// Check if operation is running
bool is_running(const std::string& op_id) const;

// Get operation status
din::Json get_status(const std::string& op_id) const;
```

**Progress Callback Signature:**
```cpp
using ProgressCallback = std::function<void(
    uint64_t current,      // Current progress value (e.g., blocks processed)
    uint64_t total,        // Total work to be done
    const std::string& message,  // Human-readable status
    const din::Json& extra       // Optional additional data
)>;
```

---

### Wallet Rescan

#### `walletrescan`

Rescan the blockchain for wallet transactions with real-time progress updates.

**Method:** `walletrescan`
**Requires:** WebSocket connection, HD wallet, unlocked wallet

**Parameters:**
```json
{
  "start_height": 0  // Optional: Block height to start from (default: 0)
}
```

**Initial Response:**
```json
{
  "operation_id": "op_a1b2c3d4",
  "status": "started",
  "message": "Wallet rescan started. Progress updates will be sent via WebSocket.",
  "start_height": 0,
  "streaming": true,
  "rpc_schema": "din.wallet.v1"
}
```

**Progress Events:**
```json
{
  "jsonrpc": "2.0",
  "method": "progress",
  "params": {
    "operation_id": "op_a1b2c3d4",
    "operation_type": "walletrescan",
    "current": 250,
    "total": 1000,
    "percentage": 25.0,
    "message": "Scanning block 250 / 1000",
    "extra": {
      "height": 250,
      "transactions_found": 5,
      "utxos_added": 10
    }
  }
}
```

**Completion Event:**
```json
{
  "jsonrpc": "2.0",
  "method": "complete",
  "params": {
    "operation_id": "op_a1b2c3d4",
    "operation_type": "walletrescan",
    "result": {
      "success": true,
      "blocks_scanned": 1000,
      "start_height": 0,
      "end_height": 1000,
      "transactions_found": 25,
      "utxos_added": 50
    }
  }
}
```

**Error Event:**
```json
{
  "jsonrpc": "2.0",
  "method": "error",
  "params": {
    "operation_id": "op_a1b2c3d4",
    "operation_type": "walletrescan",
    "error": "Operation cancelled by user",
    "code": -1
  }
}
```

**Example (HTTP - will fail):**
```bash
# HTTP RPC does not support streaming
dinero-cli walletrescan 0

# Returns:
{
  "error": "Streaming not available. WebSocket server not initialized.",
  "code": -32000,
  "hint": "Connect via WebSocket for real-time progress updates"
}
```

---

## Client Examples

### Python Client

```python
#!/usr/bin/env python3
import asyncio
import json
import websockets

async def wallet_rescan_with_progress():
    uri = "ws://localhost:8998/ws"

    async with websockets.connect(uri) as websocket:
        # Send walletrescan request
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "walletrescan",
            "params": {"start_height": 0}
        }

        await websocket.send(json.dumps(request))

        # Listen for progress events
        while True:
            message = await websocket.recv()
            data = json.loads(message)

            if "method" in data:
                method = data["method"]
                params = data.get("params", {})

                if method == "progress":
                    percentage = params.get("percentage", 0)
                    message_text = params.get("message", "")
                    print(f"Progress: {percentage:.1f}% - {message_text}")

                elif method == "complete":
                    result = params.get("result", {})
                    print(f"Complete! Blocks scanned: {result.get('blocks_scanned', 0)}")
                    break

                elif method == "error":
                    print(f"Error: {params.get('error', 'Unknown')}")
                    break

            elif "result" in data:
                op_id = data["result"].get("operation_id")
                print(f"Operation started: {op_id}")

asyncio.run(wallet_rescan_with_progress())
```

### JavaScript/Node.js Client

```javascript
const WebSocket = require('ws');

async function walletRescan() {
    const ws = new WebSocket('ws://localhost:8998/ws');

    ws.on('open', () => {
        // Send walletrescan request
        const request = {
            jsonrpc: '2.0',
            id: 1,
            method: 'walletrescan',
            params: { start_height: 0 }
        };

        ws.send(JSON.stringify(request));
    });

    ws.on('message', (data) => {
        const message = JSON.parse(data.toString());

        if (message.method === 'progress') {
            const { percentage, message: statusMessage } = message.params;
            console.log(`Progress: ${percentage.toFixed(1)}% - ${statusMessage}`);
        }
        else if (message.method === 'complete') {
            const { result } = message.params;
            console.log(`Complete! Blocks scanned: ${result.blocks_scanned}`);
            ws.close();
        }
        else if (message.method === 'error') {
            console.error(`Error: ${message.params.error}`);
            ws.close();
        }
        else if (message.result && message.result.operation_id) {
            console.log(`Operation started: ${message.result.operation_id}`);
        }
    });
}

walletRescan();
```

### Bash Client (with websocat)

```bash
# Install websocat: brew install websocat

# Start wallet rescan
echo '{"jsonrpc":"2.0","id":1,"method":"walletrescan","params":{"start_height":0}}' \
  | websocat ws://localhost:8998/ws

# Output:
# {"jsonrpc":"2.0","id":1,"result":{"operation_id":"op_a1b2c3d4","status":"started",...}}
# {"jsonrpc":"2.0","method":"progress","params":{"current":100,"total":1000,...}}
# {"jsonrpc":"2.0","method":"progress","params":{"current":200,"total":1000,...}}
# ...
# {"jsonrpc":"2.0","method":"complete","params":{"result":{"blocks_scanned":1000}}}
```

---

## Implementation Guide

### Creating a New Streaming RPC

**Step 1: Create Handler Class**

```cpp
// include/rpc/my_streaming_handler.h
#pragma once
#include "rpc/streaming_rpc_handler.h"

namespace dinero {
namespace rpc {

class MyStreamingHandler : public StreamingRpcHandler {
public:
    MyStreamingHandler(WebSocketServer* ws_server);

    // Public interface for starting operation
    din::Json start_my_operation(const std::string& client_id, const din::Json& params);

private:
    // Actual work function
    din::Json perform_work(const din::Json& params, ProgressCallback progress_cb);
};

} // namespace rpc
} // namespace dinero
```

**Step 2: Implement Handler**

```cpp
// src/rpc/my_streaming_handler.cpp
#include "rpc/my_streaming_handler.h"

namespace dinero {
namespace rpc {

MyStreamingHandler::MyStreamingHandler(WebSocketServer* ws_server)
    : StreamingRpcHandler("my_operation", ws_server)
{
}

din::Json MyStreamingHandler::start_my_operation(
    const std::string& client_id,
    const din::Json& params)
{
    auto work_fn = [this, params](ProgressCallback progress_cb) -> din::Json {
        return this->perform_work(params, progress_cb);
    };

    std::string op_id = start_operation(client_id, work_fn, params);

    din::Json result;
    result["operation_id"] = op_id;
    result["status"] = "started";
    return result;
}

din::Json MyStreamingHandler::perform_work(
    const din::Json& params,
    ProgressCallback progress_cb)
{
    uint64_t total_work = 1000;

    for (uint64_t i = 0; i < total_work; i++) {
        // Do some work...

        // Report progress every 100 iterations
        if (i % 100 == 0) {
            din::Json extra = din::obj();
            extra["iteration"] = i;

            std::string message = "Processing item " + std::to_string(i);
            progress_cb(i, total_work, message, extra);
        }
    }

    // Return final result
    din::Json result;
    result["success"] = true;
    result["items_processed"] = total_work;
    return result;
}

} // namespace rpc
} // namespace dinero
```

**Step 3: Add RPC Method**

```cpp
// In your RPC registration code
din::Json my_operation_impl(const ExecutionContext& ctx, const din::Json& params) {
    // Get client_id from context
    std::string client_id = ctx.client_id.empty() ? "default" : ctx.client_id;

    // Create handler
    MyStreamingHandler handler(g_websocket_server_for_streaming);

    // Start operation
    return handler.start_my_operation(client_id, params);
}

// Register method
g_rpcRegistry.registerHandler("my_operation",
    [](const ExecutionContext& ctx, const din::Json& params) {
        return my_operation_impl(ctx, params);
    },
    "streaming");
```

---

## Configuration

### Daemon Setup

The WebSocket server must be initialized at daemon startup:

```cpp
// In src/daemon/main.cpp (example)

// Create WebSocket server
auto ws_server_adapter = std::make_unique<dinero::rpc::WsServerAdapter>(ws_server);

// Set global pointer for streaming RPC
extern dinero::rpc::WebSocketServer* g_websocket_server_for_streaming;
g_websocket_server_for_streaming = ws_server_adapter.get();
```

### Client Context

WebSocket connections must provide `client_id` in the execution context:

```cpp
// When processing WebSocket RPC call
ExecutionContext ctx;
ctx.client_id = connection->get_id();  // Unique WebSocket connection ID
ctx.walletName = "default";
// ... other context fields
```

---

## Testing

### Test Script

```bash
./test_streaming_rpc.sh
```

**Test Coverage:**
- ✅ HTTP RPC returns "streaming not available" error
- ✅ WebSocket connection handling
- ✅ Progress event generation
- ✅ Completion event with results
- ✅ Error event handling
- ✅ Operation cancellation
- ✅ Thread-safe concurrent operations

### Manual Testing

```bash
# Start daemon
./build/dinerod --regtest --daemon

# Create HD wallet
./build/dinero-cli createhdwallet

# Generate blocks
./build/dinero-cli generatetoaddress 500 $(./build/dinero-cli getnewaddress)

# Test with Python client
python3 examples/streaming_rpc_client.py

# Or JavaScript client
node examples/streaming_rpc_client.js
```

---

## Performance Characteristics

### Progress Update Frequency

- **Recommended:** Report progress every 100-1000 iterations
- **Minimum:** Every 1 second (for operations > 10 seconds)
- **Maximum:** Every 100ms (avoid flooding client)

### Memory Usage

- **Per Operation:** ~1KB (operation state)
- **Concurrent Operations:** No practical limit (thread-safe)
- **Cleanup:** Automatic after 5 seconds post-completion

### Thread Safety

- All operations are thread-safe
- Uses `std::mutex` for state protection
- Background workers run on detached threads
- No blocking of RPC server

---

## Future Enhancements

### Phase 1: Additional Streaming Methods

- **`generatetoaddress`** - Mining with block-by-block progress
- **`sendmany`** - Batch transaction sending
- **`validateblockchain`** - Full blockchain validation
- **`reindexutxos`** - UTXO set reindexing

### Phase 2: Advanced Features

- **Operation Queue** - Priority-based operation scheduling
- **Rate Limiting** - Per-client operation limits
- **Persistence** - Resume operations after daemon restart
- **Metrics** - Operation statistics and performance tracking

### Phase 3: Client Libraries

- **TypeScript SDK** - `dinero-streaming-sdk`
- **Python SDK** - `dinero-py-streaming`
- **Swift SDK** - `DineroStreamingKit` (for iOS)

---

## Troubleshooting

### "Streaming not available" Error

**Problem:** HTTP RPC returns "Streaming not available. WebSocket server not initialized."

**Solution:**
1. Ensure daemon is running with WebSocket support
2. Connect via WebSocket (`ws://localhost:8998/ws`) instead of HTTP
3. Check that `g_websocket_server_for_streaming` is initialized in daemon

### WebSocket Connection Refused

**Problem:** Client cannot connect to `ws://localhost:8998/ws`

**Solution:**
1. Check daemon is running: `ps aux | grep dinerod`
2. Verify WebSocket port: Check daemon logs for WebSocket server startup
3. Check firewall rules
4. Try `telnet localhost 8998` to test connectivity

### No Progress Events Received

**Problem:** Operation starts but no progress events arrive

**Solution:**
1. Verify `client_id` is set in `ExecutionContext`
2. Check WebSocket connection is still open
3. Enable debug logging: Look for "[StreamingRPC]" messages
4. Ensure progress_cb() is being called in work function

---

## Summary

The Streaming RPC System provides enterprise-grade real-time progress updates for long-running operations:

✅ **Core Infrastructure**: StreamingRpcHandler base class with progress callbacks
✅ **Wallet Rescan**: Full implementation with block-by-block progress
✅ **Thread Safety**: Concurrent operations with proper synchronization
✅ **Client Examples**: Python, JavaScript, Bash examples provided
✅ **Testing**: Comprehensive test suite included

**Ready for production use.**

---

**Implementation Files:**
- `include/rpc/streaming_rpc_handler.h` - Base classes and interfaces
- `src/rpc/streaming_rpc_handler.cpp` - Implementation
- `src/rpc/methods_wallet.cpp` - walletrescan integration
- `test_streaming_rpc.sh` - Test suite
- `examples/streaming_rpc_client.js` - JavaScript client example

**Related Documentation:**
- `docs/WEBSOCKET_RPC_SYSTEM.md` - WebSocket infrastructure
- `docs/SESSION_MANAGEMENT.md` - Session and device tracking
- `docs/TOKEN_AUTH_COMPLETE.md` - Token authentication
- `docs/RPC_API.md` - Complete RPC API reference
