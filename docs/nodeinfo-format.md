# Dinero CLI nodeinfo.json Format

The `nodeinfo.json` file provides connection details for the Dinero CLI to communicate with the daemon. This file is automatically discovered or can be specified explicitly.

## Network Economics (v1.0.0)
- **Block Time**: 520 seconds (8.7 minutes)
- **Retarget**: 60 blocks (~8.7 hours)
- **CPU-Friendly Phase**: 7M→25M DIN at 99 DIN/block (~3 years)
- **Total Supply**: 180.5M DIN (7M premine + 18M CPU + 155.5M halving)

## File Location

The CLI searches for `nodeinfo.json` in the following order:

1. **Explicit path**: `--nodeinfo /path/to/nodeinfo.json`
2. **OS default**: 
   - macOS/Linux: `~/.dinero/<network>/nodeinfo.json`
   - Linux: `~/.dinero/<network>/nodeinfo.json`
3. **Datadir**: `--datadir /path/to/data/<network>/nodeinfo.json`

## Canonical Format

```json
{
  "schema": "din.nodeinfo.v1",
  "version": "din.nodeinfo.v1", 
  "network": "regtest",
  "rpc": {
    "url": "http://127.0.0.1:20999",
    "cookie_path": "/absolute/path/to/regtest/.cookie",
    "timeout_seconds": 30
  }
}
```

## Cookie Field Variants (Tolerant Parsing)

The CLI accepts multiple cookie formats for compatibility:

### Preferred: `cookie_path`
```json
{
  "rpc": {
    "url": "http://127.0.0.1:20999",
    "cookie_path": "/path/to/.cookie"
  }
}
```

### Alternative: `cookie_file`
```json
{
  "rpc": {
    "url": "http://127.0.0.1:20999", 
    "cookie_file": "/path/to/.cookie"
  }
}
```

### Legacy: `cookie.path`
```json
{
  "rpc": {
    "url": "http://127.0.0.1:20999",
    "cookie": {
      "path": "/path/to/.cookie"
    }
  }
}
```

### Insecure: Literal cookie content
```json
{
  "rpc": {
    "url": "http://127.0.0.1:20999",
    "cookie": "__cookie__:deadbeef..."
  }
}
```
⚠️ **Security Warning**: Literal cookie content requires `--accept-insecure-cookie` flag and is NOT recommended for production use.

## Override Behavior

Command-line flags always take precedence over nodeinfo.json:

- `--rpc-url` overrides `nodeinfo.rpc.url`
- `--cookie-file` overrides all cookie fields
- If both overrides are provided, nodeinfo.json is completely bypassed

## Examples

### Development (regtest)
```json
{
  "schema": "din.nodeinfo.v1",
  "version": "din.nodeinfo.v1",
  "network": "regtest",
  "rpc": {
    "url": "http://127.0.0.1:20999",
    "cookie_path": "/Users/alice/Documents/DineroCoin/data/regtest/.cookie",
    "timeout_seconds": 30
  }
}
```

### Production (mainnet)
```json
{
  "schema": "din.nodeinfo.v1", 
  "version": "din.nodeinfo.v1",
  "network": "mainnet",
  "rpc": {
    "url": "http://127.0.0.1:8332",
    "cookie_path": "/var/lib/dinero/mainnet/.cookie",
    "timeout_seconds": 60
  }
}
```

### Remote daemon
```json
{
  "schema": "din.nodeinfo.v1",
  "version": "din.nodeinfo.v1", 
  "network": "mainnet",
  "rpc": {
    "url": "https://my-dinero-node.example.com:8332",
    "cookie_path": "/home/user/.dinero/remote-cookie",
    "timeout_seconds": 120
  }
}
```

## Error Handling

### Missing cookie
```
❌ nodeinfo.json missing cookie path
```
**Solution**: Add `cookie_path`, `cookie_file`, or use `--cookie-file`

### Missing URL  
```
❌ nodeinfo.json missing rpc.url
```
**Solution**: Add `rpc.url` or use `--rpc-url`

### File not found
```
❌ nodeinfo.json not found at: /path/to/nodeinfo.json
```
**Solution**: Create the file or use `--rpc-url` and `--cookie-file` overrides

## Best Practices

1. **Use absolute paths** for cookie files to avoid path resolution issues
2. **Set appropriate permissions** on cookie files (600/rw-------)
3. **Use `cookie_path`** as the preferred cookie field name
4. **Keep timeout reasonable** (30-120 seconds depending on network conditions)
5. **Never commit** nodeinfo.json with literal cookies to version control
