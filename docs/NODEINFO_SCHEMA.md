# Nodeinfo.json Schema Documentation

The `nodeinfo.json` file is the authoritative source for discovering DineroCoin daemon endpoints and configuration. It's written after successful service initialization and contains effective (actual bound) ports, making it essential for test harnesses and automated tooling.

## Schema

```json
{
  "rpc": {
    "host": "127.0.0.1",
    "port": 63377,
    "url": "http://127.0.0.1:63377"
  },
  "ws": {
    "host": "127.0.0.1", 
    "port": 63378,
    "url": "ws://127.0.0.1:63378/ws"
  },
  "p2p": {
    "port": 63379
  },
  "cookie": "/path/to/datadir/.cookie",
  "datadir": "/path/to/datadir",
  "network": "regtest",
  "pid": 12345
}
```

## Field Descriptions

### `rpc` (object, always present)
HTTP JSON-RPC server configuration.
- `host` (string): Bind address, typically "127.0.0.1"
- `port` (number): Effective bound port (may differ from configured port if ephemeral)
- `url` (string): Complete HTTP URL for RPC calls

### `ws` (object, present if WebSocket enabled)
WebSocket JSON-RPC server configuration.
- `host` (string): Bind address, typically "127.0.0.1"
- `port` (number): Effective bound port (0 if not properly reporting yet)
- `url` (string): Complete WebSocket URL for RPC calls

### `p2p` (object or null)
P2P networking configuration. `null` if P2P disabled.
- `port` (number): P2P listening port

### `cookie` (string)
Absolute path to the authentication cookie file for RPC access.

### `datadir` (string)
Absolute path to the daemon's data directory.

### `network` (string)
Active network: `"mainnet"`, `"testnet"`, or `"regtest"`

### `pid` (number)
Process ID of the daemon (useful for multi-instance debugging).

## Usage Examples

### Test Harness Discovery
```python
import json
import base64
import requests

# Load nodeinfo
with open("nodeinfo.json") as f:
    info = json.load(f)

# Get authentication
with open(info["cookie"], "rb") as f:
    cookie = f.read().strip()
auth = "Basic " + base64.b64encode(cookie).decode()

# Make RPC call
response = requests.post(
    info["rpc"]["url"],
    json={"jsonrpc": "2.0", "method": "getbestblockhash", "id": 1},
    headers={"Authorization": auth}
)
```

### CLI Auto-Discovery
```bash
# CLI can auto-discover endpoints from nodeinfo.json
dinero-cli --datadir=/path/to/data getbestblockhash

# Or explicitly print discovered configuration
dinero-cli --datadir=/path/to/data --print-nodeinfo
```

## Ephemeral Ports

When daemon is started with ephemeral ports (`-rpcport=0 -wsport=0 -port=0`):
- `rpc.port` contains the OS-assigned port (e.g., 63377)
- `ws.port` should contain the OS-assigned port (currently reports 0)
- `p2p.port` contains the OS-assigned port if P2P enabled
- URLs reflect the actual bound addresses for immediate connectivity

## File Location

The nodeinfo.json location is controlled by:
1. `-nodeinfo=/path/to/nodeinfo.json` flag (explicit path)
2. Default: `<datadir>/nodeinfo.json`

## Timing Guarantees

- Written after all services successfully bind to their ports
- Contains effective (actual) port numbers, not configured values
- Safe to read immediately after daemon startup completes
- Rewritten if daemon restarts or ports change

## Integration Notes

- Essential for CI/CD test environments using ephemeral ports
- Enables "plug-and-play" tooling that discovers endpoints automatically
- Provides single source of truth for daemon connectivity
- Cookie path enables immediate authenticated access
