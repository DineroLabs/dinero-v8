# WebSocket RPC Integration - Production Ready

## Summary
Implements production-grade Beast WebSocket RPC server with strict authentication, message limits, and clean JSON responses. Resolves port conflicts and eliminates double-encoding issues.

## 🎯 Key Changes

### WebSocket RPC Server
- **NEW**: Beast-based WebSocket JSON-RPC on port 21000
- **BREAKING**: Port moved from 18332 → 21000 (avoids Bitcoin testnet conflict)
- **FIXED**: Double-encoding eliminated - clean JSON responses
- **SECURITY**: Cookie-based Basic auth required, 1MB message limit

### Port Management
- **HTTP RPC**: 20998 (unchanged)
- **P2P Network**: 20999 (unchanged) 
- **WebSocket RPC**: 21000 (new default)
- **ADDED**: Port collision guard prevents conflicts

### Legacy Cleanup
- **REMOVED**: Legacy event broadcasting WebSocket server (port conflict source)
- **FIXED**: CMake linkage with proper Boost::system dependency

## 🔧 Technical Implementation

### Files Changed
- `src/daemon/ws/ws_server.cpp` - Beast WebSocket server with limits
- `src/daemon/ws/ws_session.cpp` - RPC handling without double-encoding
- `src/daemon/rpc_server.cpp` - Fixed return type for clean JSON
- `src/daemon/main.cpp` - Port collision guard, new defaults
- `CMakeLists.txt` - Proper Boost::system linkage

### Configuration
| Parameter | Default | Description |
|-----------|---------|-------------|
| `ws.rpc.port` | 21000 | WebSocket RPC port |
| `ws.rpc.listen` | 127.0.0.1 | Bind address |
| `ws.max_message` | 1MB | Message size limit |
| `ws.require_auth` | true | Cookie authentication |

## 🧪 Testing

### Smoke Test (Automated)
```bash
# Start daemon
./build/bin/dinerod --datadir=e2e --rpc.port=20998 --ws.rpc.port=21000 &
sleep 2

# HTTP RPC test
AUTH64=$(base64 < e2e/.cookie | tr -d '\n')
curl -s -H "Authorization: Basic $AUTH64" \
  --data '{"jsonrpc":"2.0","id":1,"method":"getbestblockhash","params":[]}' \
  http://127.0.0.1:20998/ | jq -r .result | grep -E '^[0-9a-f]{64}$'

# WebSocket RPC test  
python3 tools/ws_ping.py --url ws://127.0.0.1:21000/ws --cookie e2e/.cookie

# Cleanup
pkill -f dinerod
```

### Manual Verification
- ✅ WebSocket responds with clean JSON (no double quotes)
- ✅ Cookie authentication enforced (401 on missing auth)
- ✅ Port collision detection works
- ✅ Message size limits enforced
- ✅ All RPC methods available via WebSocket

## 📚 Documentation
- `WEBSOCKET_RPC_GUIDE.md` - Complete client integration guide
- `CHANGELOG.md` - Breaking changes and migration notes
- `CODEOWNERS` - Protection for WebSocket infrastructure

## 🚀 Client Examples

### Python
```python
async with websockets.connect("ws://127.0.0.1:21000/ws", extra_headers=auth) as ws:
    await ws.send(json.dumps({"jsonrpc":"2.0","method":"getbestblockhash","id":1}))
    response = json.loads(await ws.recv())
```

### Qt
```cpp
QWebSocket ws;
QNetworkRequest req(QUrl("ws://127.0.0.1:21000/ws"));
req.setRawHeader("Authorization", "Basic " + cookie.toBase64());
ws.open(req);
```

## 🔒 Security Features
- Strict cookie-based authentication
- 1MB message size limit with policy close
- Timeout protection and backpressure
- Origin validation ready (optional)

## ⚠️ Breaking Changes
- **WebSocket port**: 18332 → 21000
- **Legacy events**: Disabled (use HTTP polling if needed)

## Migration Guide
Update client connections:
```diff
- ws://127.0.0.1:18332/
+ ws://127.0.0.1:21000/ws
```

## Ready for Production ✅
- Clean build with proper dependencies
- Comprehensive documentation
- Port conflict prevention
- Security hardening complete
