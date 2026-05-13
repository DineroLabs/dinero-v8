# WebSocket RPC - Quick Start Guide

**Get started with DineroCoin's real-time WebSocket API in 5 minutes.**

---

## 1. Start the Daemon

```bash
# Start in regtest mode for testing
./build/dinerod --regtest --daemon

# Or mainnet
./build/dinerod --daemon
```

Default ports:
- HTTP RPC: `8998` (mainnet) / `18998` (regtest)
- WebSocket: `8999` (mainnet) / `18999` (regtest)

---

## 2. Get Authentication Cookie

```bash
# Read cookie file
cat ~/.dinero/.cookie

# Output format:
# __cookie__:abc123xyz789...
```

---

## 3. Connect (Python Example)

```python
import asyncio
import websockets
import json
import base64

async def connect():
    # Read cookie
    with open('/path/to/.cookie', 'r') as f:
        cookie = f.read().strip()

    # Create auth header
    auth = 'Basic ' + base64.b64encode(cookie.encode()).decode()

    # Connect with authentication
    async with websockets.connect(
        'ws://localhost:18999',
        extra_headers={'Authorization': auth}
    ) as ws:

        # Call RPC method
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "getblockcount"
        }
        await ws.send(json.dumps(request))
        response = json.loads(await ws.recv())

        print(f"Block height: {response['result']}")

asyncio.run(connect())
```

---

## 4. Subscribe to Events

```python
# Subscribe to new blocks
subscribe_request = {
    "jsonrpc": "2.0",
    "id": 2,
    "method": "ws_subscribe",
    "params": {
        "filter": {
            "event_types": ["new_block"]
        }
    }
}
await ws.send(json.dumps(subscribe_request))
response = json.loads(await ws.recv())

print(f"Subscribed: {response['result']['subscription_id']}")

# Listen for events
while True:
    message = json.loads(await ws.recv())
    if message.get('type') == 'event':
        print(f"New block: {message['data']}")
```

---

## 5. Discover Available Methods

```python
# List all RPC methods
discover_request = {
    "jsonrpc": "2.0",
    "id": 3,
    "method": "rpc.discover"
}
await ws.send(json.dumps(discover_request))
response = json.loads(await ws.recv())

methods = response['result']['methods']
print(f"Available methods: {len(methods)}")

for method in methods:
    print(f"  - {method['name']} ({method['category']})")
```

---

## Available Methods (57 Total)

### Blockchain
- `getblockcount` - Current height
- `getbestblockhash` - Tip block hash

### Wallet
- `createhdwallet` - Create new HD wallet
- `getnewaddress` - Generate address
- `getbalance` - Check balance
- `sendtoaddress` - Send transaction
- `listtransactions` - Transaction history
- `listunspent` - UTXO list
- And 20+ more...

### WebSocket
- `ws_subscribe` - Subscribe to events
- `ws_unsubscribe` - Unsubscribe
- `ws_list_subscriptions` - List subscriptions
- `ws_event_types` - Available events

### Discovery
- `rpc.discover` - List all methods
- `rpc.info` - Server information

---

## Available Events (17 Total)

### Transactions
- `transaction_received` - New transaction
- `transaction_confirmed` - Confirmed in block
- `wallet_incoming_tx` - Incoming payment
- `wallet_outgoing_tx` - Outgoing payment

### Blocks
- `new_block` - New block mined
- `block_orphaned` - Chain reorg

### Network
- `mempool_size_changed` - Mempool update
- `chain_syncing` - Sync status
- `mining_started` - Mining active

---

## JavaScript/Node.js Example

```javascript
const WebSocket = require('ws');
const fs = require('fs');

// Read cookie
const cookie = fs.readFileSync('/path/to/.cookie', 'utf8').trim();
const auth = 'Basic ' + Buffer.from(cookie).toString('base64');

// Connect
const ws = new WebSocket('ws://localhost:18999', {
    headers: { 'Authorization': auth }
});

ws.on('open', () => {
    console.log('Connected!');

    // Get block count
    ws.send(JSON.stringify({
        jsonrpc: '2.0',
        id: 1,
        method: 'getblockcount'
    }));
});

ws.on('message', (data) => {
    const response = JSON.parse(data);
    console.log('Response:', response);
});
```

---

## cURL Example (HTTP RPC)

```bash
# Read cookie
COOKIE=$(cat ~/.dinero/.cookie)

# Call RPC via HTTP
curl -u "$COOKIE" http://localhost:18998 \
    -H "Content-Type: application/json" \
    -d '{
        "jsonrpc": "2.0",
        "id": 1,
        "method": "getblockcount"
    }'
```

---

## Event Filtering

Subscribe only to high-value transactions:

```python
await ws.send(json.dumps({
    "jsonrpc": "2.0",
    "id": 4,
    "method": "ws_subscribe",
    "params": {
        "filter": {
            "event_types": ["transaction_received"],
            "min_amount": 100000000  # 1.0 DIN in una
        }
    }
}))
```

Watch specific address:

```python
await ws.send(json.dumps({
    "jsonrpc": "2.0",
    "id": 5,
    "method": "ws_subscribe",
    "params": {
        "filter": {
            "event_types": ["wallet_incoming_tx"],
            "addresses": ["din1q..."]
        }
    }
}))
```

---

## Error Handling

```python
try:
    response = json.loads(await ws.recv())

    if 'error' in response:
        print(f"RPC Error {response['error']['code']}: {response['error']['message']}")
    elif 'result' in response:
        print(f"Success: {response['result']}")

except websockets.exceptions.ConnectionClosed:
    print("Connection closed - reconnecting...")
    # Implement reconnection logic

except json.JSONDecodeError:
    print("Invalid JSON response")
```

---

## Common Error Codes

| Code | Meaning |
|------|---------|
| `-32700` | Parse error (invalid JSON) |
| `-32600` | Invalid request |
| `-32601` | Method not found |
| `-32602` | Invalid params |
| `-32603` | Internal error |
| `-32000` | Application-specific error |

---

## Best Practices

### 1. Reconnection Logic

```python
async def connect_with_retry():
    max_retries = 5
    retry_delay = 1  # seconds

    for attempt in range(max_retries):
        try:
            return await websockets.connect(url, extra_headers=headers)
        except Exception as e:
            if attempt < max_retries - 1:
                await asyncio.sleep(retry_delay * (attempt + 1))
            else:
                raise
```

### 2. Request ID Tracking

```python
class RPCClient:
    def __init__(self):
        self.request_id = 0
        self.pending_requests = {}

    async def call(self, method, params=None):
        self.request_id += 1
        request_id = self.request_id

        # Send request
        await self.ws.send(json.dumps({
            'jsonrpc': '2.0',
            'id': request_id,
            'method': method,
            'params': params or {}
        }))

        # Wait for response
        future = asyncio.Future()
        self.pending_requests[request_id] = future
        return await future

    async def handle_response(self, message):
        data = json.loads(message)
        request_id = data.get('id')

        if request_id in self.pending_requests:
            self.pending_requests[request_id].set_result(data['result'])
            del self.pending_requests[request_id]
```

### 3. Rate Limiting

```python
import time

class RateLimiter:
    def __init__(self, max_per_second=10):
        self.max_per_second = max_per_second
        self.requests = []

    async def wait(self):
        now = time.time()
        # Remove requests older than 1 second
        self.requests = [t for t in self.requests if now - t < 1]

        if len(self.requests) >= self.max_per_second:
            # Wait until oldest request is 1 second old
            sleep_time = 1 - (now - self.requests[0])
            if sleep_time > 0:
                await asyncio.sleep(sleep_time)

        self.requests.append(time.time())
```

---

## Testing with wscat

Install wscat:
```bash
npm install -g wscat
```

Connect:
```bash
wscat -c ws://localhost:18999 \
    -H "Authorization: Basic $(echo -n '__cookie__:abc123' | base64)"
```

Send command:
```json
{"jsonrpc":"2.0","id":1,"method":"getblockcount"}
```

---

## Next Steps

1. **Read Full Documentation:** `docs/WEBSOCKET_RPC_SYSTEM.md`
2. **Explore Events:** `docs/REAL_TIME_ECOSYSTEM_VISION.md`
3. **Run Test Client:** `python3 test_event_bus.py`
4. **Build Your App:** See examples in SDK documentation

---

## Support

- **GitHub Issues:** https://github.com/dinero-coin/dinero/issues
- **Documentation:** https://docs.dinero-coin.com
- **Discord:** https://discord.gg/dinero

---

**Happy Building! 🚀**
