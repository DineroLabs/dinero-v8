# DineroCoin WebSocket RPC Guide

## Configuration

### Port Configuration
| Service | Default Port | Description |
|---------|--------------|-------------|
| HTTP RPC | 20998 | JSON-RPC over HTTP |
| P2P Network | 20999 | Node-to-node communication |
| WebSocket RPC | 21000 | JSON-RPC over WebSocket |

### Configuration Parameters
| Key | Default | Notes |
|-----|---------|-------|
| `rpc.port` | 20998 | HTTP JSON-RPC |
| `p2p.port` | 20999 | Node P2P |
| `ws.rpc.listen` | 127.0.0.1 | Bind address |
| `ws.rpc.port` | 21000 | WebSocket JSON-RPC |
| `ws.rpc.path` | /ws | RPC endpoint base |
| `ws.require_auth` | true | Basic (.cookie) |
| `ws.max_message` | 1048576 | 1 MB cap |
| `ws.max_queue` | 64 | Backpressure |

### Command Line Options
```bash
./dinerod --wsport=21000 --wsbind=127.0.0.1 --wspath=/ws
```

### Authentication
WebSocket RPC requires cookie-based Basic authentication:
```bash
# Read cookie from daemon data directory
COOKIE=$(cat ./data-fresh/.cookie)
AUTH_HEADER="Authorization: Basic $(echo -n "$COOKIE" | base64)"
```

## Client Examples

### Python WebSocket Client
```python
import asyncio
import websockets
import json
import base64

async def rpc_call():
    # Read authentication cookie
    with open('./data-fresh/.cookie', 'r') as f:
        cookie = f.read().strip()
    
    auth_string = base64.b64encode(cookie.encode()).decode()
    headers = {'Authorization': f'Basic {auth_string}'}
    
    uri = "ws://127.0.0.1:21000/"
    
    async with websockets.connect(uri, extra_headers=headers) as ws:
        request = {
            "jsonrpc": "2.0",
            "method": "getbestblockhash",
            "params": [],
            "id": 1
        }
        
        await ws.send(json.dumps(request))
        response = await ws.recv()
        return json.loads(response)
```

### Qt QWebSocket Client
```cpp
QWebSocket ws;
QNetworkRequest req(QUrl("ws://127.0.0.1:21000/ws"));
QByteArray basic = "Basic " + QFile("./data-fresh/.cookie").readAll().trimmed().toBase64();
req.setRawHeader("Authorization", basic);
ws.open(req);
```

### websocat Command Line
```bash
AUTH=$(base64 < ./data-fresh/.cookie | tr -d '\n')
websocat -H "Authorization: Basic $AUTH" ws://127.0.0.1:21000/ws
```

### curl WebSocket Test
```bash
AUTH64=$(base64 < ./data-fresh/.cookie | tr -d '\n')
curl -s -H "Authorization: Basic $AUTH64" \
     -H "Upgrade: websocket" \
     -H "Connection: Upgrade" \
     ws://127.0.0.1:21000/ws
```

### DineroCoin CLI with WebSocket Transport
The `dinero-cli` tool now supports WebSocket transport with automatic fallback:

```bash
# Auto-detect transport (tries WebSocket first, falls back to HTTP)
./dinero-cli getbestblockhash

# Force WebSocket transport
./dinero-cli --transport ws getbestblockhash

# Force HTTP transport
./dinero-cli --transport http getbestblockhash

# Custom WebSocket URL
./dinero-cli --ws-url ws://127.0.0.1:21000/ws getbestblockhash

# Custom cookie path
./dinero-cli --cookie ./custom-path/.cookie getbestblockhash
```

#### CLI Transport Options
| Flag | Default | Description |
|------|---------|-------------|
| `--transport` | `auto` | Transport mode: `http`, `ws`, or `auto` |
| `--http-url` | `http://127.0.0.1:20998/` | HTTP RPC endpoint |
| `--ws-url` | `ws://127.0.0.1:21000/ws` | WebSocket RPC endpoint |
| `--cookie` | Auto-detect | Cookie file path for authentication |

#### CLI Examples
```bash
# Wallet operations via WebSocket
./dinero-cli --transport ws wallet.create my_wallet
./dinero-cli --transport ws wallet.getnewaddress
./dinero-cli --transport ws wallet.listaddresses

# Mining operations via WebSocket
./dinero-cli --transport ws mining.info
./dinero-cli --transport ws mining.start 4
./dinero-cli --transport ws mining.setpayoutaddress rdin1abc...

# Blockchain queries via WebSocket
./dinero-cli --transport ws getblockchaininfo
./dinero-cli --transport ws getblockcount
./dinero-cli --transport ws getblock <hash>
```

## Production Deployment

### Reverse Proxy (Caddy)
```
your.domain.com {
  @rpc path /ws /wallet/*
  reverse_proxy @rpc 127.0.0.1:21000 {
    header_up Authorization {>Authorization}
    header_up Upgrade {>Upgrade}
    header_up Connection {>Connection}
  }
}
```

### Security Features
- ✅ **Strict Authentication**: 401 on missing/invalid Authorization
- ✅ **Message Limits**: 1MB max message size
- ✅ **Timeout Protection**: Configurable idle timeouts
- ✅ **Backpressure**: Connection closes on queue overflow

### Supported RPC Methods
All standard JSON-RPC 2.0 methods available via HTTP are also available via WebSocket:
- `getbestblockhash`
- `getblockcount`
- `getmininginfo`
- `wallet.*` methods (with proper wallet context)
- `mining.*` methods

## Testing
```bash
# Start daemon
./dinerod --datadir=./data-fresh --wsport=21000

# Test WebSocket RPC
python3 tools/ws_ping.py
```
