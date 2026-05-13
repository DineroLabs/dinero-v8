# Payment Monitor Integration Guide

## Daemon Integration Required

The Payment Monitor API needs to be initialized in the daemon at startup. Here's how to integrate it:

### Step 1: Initialize PaymentMonitor in main.cpp

Add to daemon startup (around where RPC server is initialized):

```cpp
#include "rpc/payment_monitor.h"
#include "rpc/methods_payment.h"

// In main() after EventBus and WebSocket are initialized:

// Initialize PaymentMonitor
extern dinero::rpc::PaymentMonitor* g_payment_monitor;
auto payment_monitor = std::make_unique<dinero::rpc::PaymentMonitor>(
    &event_bus,           // EventBus instance
    ws_server_adapter.get()  // WebSocket server
);
g_payment_monitor = payment_monitor.get();

dinero::g_logger.info("✅ Payment Monitor initialized");
```

### Step 2: Register Payment RPC Methods

Add after other RPC method registrations:

```cpp
// Register payment RPC methods
registerPaymentRPC();
```

### Step 3: Cleanup on Shutdown

Add to daemon shutdown:

```cpp
// Cleanup payment monitor
g_payment_monitor = nullptr;
payment_monitor.reset();
```

## Testing Without Full Integration

If the daemon hasn't been updated yet, the test will show:

```json
{
  "error": "Payment monitor not initialized",
  "code": -32000
}
```

This is expected and means the integration step above is needed.

## Quick Test

```bash
# After integration, test basic functionality:
./build/dinero-cli payment.watch '{"address":"din1qtest..."}'

# Should return:
{
  "watch_id": "watch_abc123...",
  "status": "watching",
  "address": "din1qtest...",
  "rpc_schema": "din.payment.v1"
}
```

## EventBus Requirements

The PaymentMonitor needs these events to be published:

1. **TransactionReceived** - When tx hits mempool
   - Must include: txid, amount, affected_addresses

2. **NewBlock** - When block confirmed
   - Updates confirmation counts

If these events aren't being published, the monitor won't detect payments.

## Verification Checklist

- [ ] EventBus initialized
- [ ] WebSocket server running
- [ ] PaymentMonitor created with both dependencies
- [ ] `g_payment_monitor` global set
- [ ] `registerPaymentRPC()` called
- [ ] Transaction events being published to EventBus
- [ ] Block events being published

## Debugging

Enable debug logging:

```cpp
dinero::g_logger.set_level(LogLevel::DEBUG);
```

Look for these log messages:
```
[PaymentMonitor] Initialized with EventBus subscriptions
[Payment RPC] Registered methods (4 methods)
[PaymentMonitor] Started watching address: din1q...
[PaymentMonitor] Payment detected: txid... to din1q... (watch: watch_...)
```

## Current Status

**Build:** ✅ Complete - All code compiles
**Tests:** ⏳ Pending daemon integration
**Docs:** 📝 In progress

Once daemon integration is complete, run:
```bash
./test_payment_monitor.sh
```
