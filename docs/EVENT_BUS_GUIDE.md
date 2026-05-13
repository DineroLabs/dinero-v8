# DineroCoin Event Bus / WebSocket Bridge

## Overview

The Event Bus / WebSocket Bridge provides **real-time push notifications** for blockchain and wallet events over WebSocket connections. This enables clients to receive instant updates about transactions, blocks, balance changes, and more without polling.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Client Applications                       │
│  (Web UI, Mobile Apps, Trading Bots, Monitoring Tools)     │
└────────────────────────┬────────────────────────────────────┘
                         │ WebSocket Connection
                         ▼
┌─────────────────────────────────────────────────────────────┐
│              WebSocket Event Bridge                          │
│  • Client subscription management                           │
│  • Event filtering & routing                                │
│  • RPC handlers (subscribe/unsubscribe)                     │
└────────────────────────┬────────────────────────────────────┘
                         │ Pub/Sub
                         ▼
┌─────────────────────────────────────────────────────────────┐
│                    Event Bus (Core)                         │
│  • Central pub/sub system                                   │
│  • Event filtering & matching                               │
│  • Thread-safe delivery                                     │
└────────────────────────┬────────────────────────────────────┘
                         │ Publish Events
        ┌────────────────┼────────────────┐
        ▼                ▼                ▼
┌───────────────┐ ┌──────────────┐ ┌─────────────┐
│ Wallet Layer  │ │  Mempool     │ │  Blockchain │
│ (RPC Methods) │ │  Processor   │ │  Processor  │
└───────────────┘ └──────────────┘ └─────────────┘
```

## Event Types

### Transaction Events
- `transaction_received` - New transaction entered mempool
- `transaction_confirmed` - Transaction included in block
- `transaction_rejected` - Transaction rejected by mempool
- `wallet_incoming_tx` - Incoming transaction to wallet
- `wallet_outgoing_tx` - Outgoing transaction from wallet

### Block Events
- `new_block` - New block added to chain
- `block_orphaned` - Block became orphaned (chain reorg)

### Wallet Events
- `wallet_balance_changed` - Wallet balance updated
- `wallet_new_address` - New address generated

### Mempool Events
- `mempool_size_changed` - Mempool size changed
- `mempool_fee_changed` - Fee estimates updated

### Chain Events
- `chain_reorg` - Blockchain reorganization occurred
- `chain_syncing` - Chain sync in progress
- `chain_synced` - Chain fully synchronized

### Mining Events
- `mining_started` - Mining started
- `mining_stopped` - Mining stopped
- `mining_block_found` - New block found

## WebSocket RPC Methods

### 1. Subscribe to Events

```javascript
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "ws_subscribe",
  "params": {
    "filter": {
      "event_types": ["transaction_received", "transaction_confirmed"],
      "addresses": ["din1q..."],           // Optional: filter by address
      "min_amount": 100000000,              // Optional: minimum 1 DIN
      "confirmed_only": false               // Optional: only confirmed txs
    }
  }
}
```

**Response:**
```javascript
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "success": true,
    "subscription_id": "sub_a1b2c3d4e5f6",
    "message": "Subscribed to events successfully"
  }
}
```

### 2. Unsubscribe from Events

```javascript
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "ws_unsubscribe",
  "params": {
    "subscription_id": "sub_a1b2c3d4e5f6"
  }
}
```

### 3. List Active Subscriptions

```javascript
{
  "jsonrpc": "2.0",
  "id": 3,
  "method": "ws_list_subscriptions"
}
```

**Response:**
```javascript
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "success": true,
    "count": 2,
    "subscriptions": [
      {
        "created_at": 1699000000000,
        "events_received": 15,
        "event_types": ["transaction_received", "transaction_confirmed"],
        "addresses": ["din1q..."]
      }
    ]
  }
}
```

### 4. Get Available Event Types

```javascript
{
  "jsonrpc": "2.0",
  "id": 4,
  "method": "ws_event_types"
}
```

## Event Notification Format

When an event occurs, subscribers receive a notification:

```javascript
{
  "type": "event",
  "subscription_id": "sub_a1b2c3d4e5f6",
  "data": {
    "event_type": "transaction_received",
    "event_id": "abc123def456",
    "timestamp": 1699000000000,
    "txid": "deadbeef...",
    "amount": 100000000,
    "fee": 1000,
    "confirmations": 0,
    "addresses": ["din1qsender...", "din1qreceiver..."]
  }
}
```

## Usage Examples

### Python Client

```python
import asyncio
import websockets
import json

async def subscribe_to_transactions():
    uri = "ws://localhost:19999"

    async with websockets.connect(uri) as ws:
        # Subscribe to transaction events
        await ws.send(json.dumps({
            "jsonrpc": "2.0",
            "id": 1,
            "method": "ws_subscribe",
            "params": {
                "filter": {
                    "event_types": ["transaction_received", "transaction_confirmed"]
                }
            }
        }))

        # Get subscription response
        response = await ws.recv()
        print("Subscribed:", json.loads(response))

        # Listen for events
        while True:
            message = await ws.recv()
            event = json.loads(message)

            if event.get("type") == "event":
                data = event["data"]
                print(f"New transaction: {data['txid']}")
                print(f"Amount: {data['amount']} una")
                print(f"Confirmations: {data['confirmations']}")

asyncio.run(subscribe_to_transactions())
```

### JavaScript Client (Browser)

```javascript
const ws = new WebSocket('ws://localhost:19999');

ws.onopen = () => {
  // Subscribe to wallet events
  ws.send(JSON.stringify({
    jsonrpc: '2.0',
    id: 1,
    method: 'ws_subscribe',
    params: {
      filter: {
        event_types: ['wallet_balance_changed', 'wallet_incoming_tx']
      }
    }
  }));
};

ws.onmessage = (event) => {
  const data = JSON.parse(event.data);

  if (data.type === 'event') {
    const eventData = data.data;

    switch (eventData.event_type) {
      case 'wallet_balance_changed':
        console.log(`Balance changed: ${eventData.old_balance} → ${eventData.new_balance}`);
        updateBalanceUI(eventData.new_balance);
        break;

      case 'wallet_incoming_tx':
        console.log(`Incoming: ${eventData.amount} una`);
        showNotification(`Received ${eventData.amount / 1e8} DIN`);
        break;
    }
  }
};
```

## Integration Guide for Developers

### Publishing Events from Wallet Code

```cpp
#include "rpc/event_publishers.h"

// After sending transaction
void MyWallet::SendTransaction(...) {
    // ... transaction logic ...

    // Publish event
    dinero::rpc::publish_wallet_outgoing_tx(
        txid,
        amount_una,
        fee_una,
        {to_address}
    );
}

// After receiving transaction
void MyWallet::OnTransactionReceived(...) {
    // ... processing logic ...

    dinero::rpc::publish_wallet_incoming_tx(
        txid,
        amount_una,
        {my_address}
    );
}

// After balance update
void MyWallet::UpdateBalance(...) {
    uint64_t old_balance = current_balance;
    current_balance = new_balance;

    dinero::rpc::publish_balance_change(
        address,
        old_balance,
        new_balance
    );
}
```

### Publishing Events from Mempool

```cpp
#include "rpc/event_publishers.h"

// When transaction accepted to mempool
void Mempool::AcceptTransaction(const Transaction& tx) {
    // ... validation and acceptance ...

    dinero::rpc::publish_tx_received(
        tx.GetTxid(),
        tx.GetTotalOut(),
        tx.GetFee(),
        tx.GetAffectedAddresses()
    );
}

// When transaction rejected
void Mempool::RejectTransaction(const Transaction& tx, const std::string& reason) {
    dinero::rpc::publish_tx_rejected(tx.GetTxid(), reason);
}

// Periodic mempool stats update
void Mempool::UpdateStats() {
    auto stats = GetMempoolStats();

    dinero::rpc::publish_mempool_update(
        stats.tx_count,
        stats.total_size,
        stats.min_fee,
        stats.median_fee,
        stats.max_fee
    );
}
```

### Publishing Events from Block Processor

```cpp
#include "rpc/event_publishers.h"

// When new block added to chain
void BlockProcessor::ProcessNewBlock(const Block& block) {
    // ... block processing ...

    // Publish block event
    dinero::rpc::publish_new_block(
        block.GetHash(),
        block.nHeight,
        block.vtx.size()
    );

    // Publish confirmation events for all transactions
    for (const auto& tx : block.vtx) {
        dinero::rpc::publish_tx_confirmed(
            tx.GetTxid(),
            tx.GetTotalOut(),
            tx.GetAffectedAddresses()
        );
    }
}
```

## Event Filtering

Clients can filter events in multiple ways:

### By Event Type
```javascript
{
  "filter": {
    "event_types": ["transaction_received", "new_block"]
  }
}
```

### By Address
```javascript
{
  "filter": {
    "addresses": ["din1qmyaddress..."]
  }
}
```

### By Amount
```javascript
{
  "filter": {
    "min_amount": 100000000  // Only txs >= 1 DIN
  }
}
```

### Confirmed Only
```javascript
{
  "filter": {
    "confirmed_only": true  // Only confirmed transactions
  }
}
```

### Combined Filters
```javascript
{
  "filter": {
    "event_types": ["transaction_received", "transaction_confirmed"],
    "addresses": ["din1qmyaddress..."],
    "min_amount": 50000000,
    "confirmed_only": false
  }
}
```

## Use Cases

### 1. Real-Time Wallet UI
- Instant balance updates
- Transaction notifications
- Confirmation status

### 2. Payment Processing
- Monitor incoming payments
- Automatic confirmation tracking
- Failed transaction alerts

### 3. Trading Bots
- Track mempool fee rates
- Monitor large transactions
- Detect chain reorgs

### 4. Block Explorers
- Live transaction feed
- Real-time block updates
- Network statistics

### 5. Mobile Wallets
- Push notifications for incoming txs
- Background sync efficiency
- Battery-friendly updates

## Performance Characteristics

- **Thread-safe:** All event publishing and subscription operations are thread-safe
- **Non-blocking:** Event delivery doesn't block publishers
- **Filtered delivery:** Events only sent to matching subscribers
- **Connection cleanup:** Automatic cleanup on WebSocket disconnect
- **Statistics tracking:** Per-client and global event statistics

## Testing

Use the provided test client:

```bash
# Basic transaction monitoring
python3 test_event_bus.py

# Watch specific address
python3 test_event_bus.py din1qmyaddress...

# Run all demos
python3 test_event_bus.py
# Then select: 1 (transactions), 2 (high-value), 3 (address), 4 (blocks)
```

## Files

### Core Implementation
- `include/rpc/event_bus.h` - Event bus interface
- `src/rpc/event_bus.cpp` - Event bus implementation
- `include/rpc/websocket_event_bridge.h` - WebSocket bridge interface
- `src/rpc/websocket_event_bridge.cpp` - WebSocket bridge implementation

### Integration Helpers
- `include/rpc/event_publishers.h` - Convenience functions for publishing events

### Testing & Examples
- `test_event_bus.py` - Python test client with multiple demos
- `docs/EVENT_BUS_GUIDE.md` - This documentation

## Next Steps

1. **Add event publishing to existing components:**
   - Integrate into `sendtoaddress` RPC method
   - Add to mempool transaction acceptance
   - Wire into block processor

2. **Extended monitoring:**
   - Add metrics and logging
   - Create admin dashboard
   - Implement rate limiting

3. **Mobile SDK integration:**
   - DineroKit Swift bindings
   - Android Kotlin bindings
   - React Native wrapper

4. **Advanced features:**
   - Event replay from specific timestamp
   - Historical event queries
   - Event persistence for offline clients

---

**Built with:** C++17, WebSocket++, JSON for Modern C++
**Status:** ✅ Ready for integration and testing
**Version:** 1.0.0 (Phase 6 - Event Bus Implementation)
