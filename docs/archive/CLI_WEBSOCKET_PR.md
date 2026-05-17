# CLI WebSocket Transport Integration PR

## Summary
Adds WebSocket transport support to `dinero-cli` with automatic fallback to HTTP, aligning CLI with the new WebSocket RPC server on port 21000.

## Changes

### CLI Transport Flags
- `--transport {auto,http,ws}` - Transport mode (default: auto)
- `--http-url URL` - HTTP RPC endpoint (default: http://127.0.0.1:20998/)
- `--ws-url URL` - WebSocket RPC endpoint (default: ws://127.0.0.1:21000/ws)
- `--cookie PATH` - Cookie file path (default: auto-detect from datadir)

### Port Configuration
| Service | Port | Description |
|---------|------|-------------|
| HTTP RPC | 20998 | JSON-RPC over HTTP |
| P2P Network | 20999 | Node-to-node communication |
| WebSocket RPC | 21000 | JSON-RPC over WebSocket (path: /ws) |

### Authentication
- Unified cookie-based Basic authentication for both HTTP and WebSocket
- Reads entire `.cookie` line and sends `Authorization: Basic <base64(cookie-line)>`

### WebSocket Support
- One-shot JSON-RPC over WebSocket (send request, await single reply)
- No double-encoding issues
- Proper error handling and connection management

### Auto-Transport Logic
1. `--transport auto` (default): Try WebSocket first, fallback to HTTP on error
2. `--transport ws`: Force WebSocket only
3. `--transport http`: Force HTTP only

## Files Modified

### Core Implementation
- `src/cli/main.cpp` - Added transport flags and WebSocket logic
- `src/cli/ws_client.h` - WebSocket client interface
- `src/cli/ws_client.cpp` - Beast WebSocket client implementation
- `CMakeLists.txt` - Added WebSocket client build support

### Documentation
- `WEBSOCKET_RPC_GUIDE.md` - Added CLI examples and transport options
- CLI help text updated with new flags and port reference

## Usage Examples

```bash
# Auto-detect transport (WebSocket preferred, HTTP fallback)
./dinero-cli getbestblockhash

# Force WebSocket transport
./dinero-cli --transport ws wallet.getnewaddress

# Force HTTP transport
./dinero-cli --transport http mining.info

# Custom endpoints
./dinero-cli --ws-url ws://127.0.0.1:21000/ws getblockcount
./dinero-cli --http-url http://127.0.0.1:20998/ getbestblockhash
```

## Testing

### Manual Testing
```bash
# Start daemon with WebSocket RPC
./dinerod --regtest --datadir=./test-data

# Test HTTP transport
./dinero-cli --transport http getbestblockhash

# Test WebSocket transport  
./dinero-cli --transport ws getbestblockhash

# Test auto-detection
./dinero-cli getbestblockhash
```

### CI Integration (Planned)
```bash
# Start daemon in CI
./dinerod --regtest --datadir=./ci-data &

# Test HTTP transport
./dinero-cli --http-url http://127.0.0.1:20998/ getbestblockhash | grep -E '^[0-9a-f]{64}$'

# Test WebSocket transport
./dinero-cli --transport ws --ws-url ws://127.0.0.1:21000/ws getbestblockhash | grep -E '^[0-9a-f]{64}$'

# Test unauthorized WebSocket (should fail)
./dinero-cli --transport ws --cookie /nonexistent getbestblockhash && exit 1 || echo "Auth test passed"
```

## Version Bump
- Minor version bump to v2.1.0
- Release notes: "CLI: Adds WebSocket transport support with auto-detection and fallback to HTTP"

## Backward Compatibility
- All existing CLI usage patterns continue to work unchanged
- HTTP remains the reliable fallback transport
- Legacy `--rpc` flag still supported for HTTP endpoint

## Benefits
- Aligns CLI with WebSocket RPC server infrastructure
- Prepares foundation for GUI WebSocket integration
- Provides better performance for interactive CLI usage
- Maintains full backward compatibility
