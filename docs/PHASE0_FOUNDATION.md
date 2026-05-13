# Phase 0: Foundation Stabilization

This document outlines the Phase 0 foundation work needed before implementing the production CLI.

## Objectives

1. **Unified Datadir Convention**: Standardize on `DineroCoin` naming
2. **NodeInfo Contract**: Establish schema v1 with validation
3. **Auth Improvements**: Cookie permissions and error logging

## 1. Datadir Unification

### Current State
- Mixed naming: `.dinero`, `dinero`, `DineroCoin`
- Inconsistent across platforms

### Target State
- macOS: `~/Library/Application Support/DineroCoin/`
- Linux: `~/.config/DineroCoin/` or `~/.DineroCoin/`
- Windows: `%APPDATA%\DineroCoin\`

### Network Subdirectories
```
DineroCoin/
├── mainnet/
│   ├── nodeinfo.json
│   ├── .cookie
│   ├── blocks/
│   └── chainstate/
├── testnet/
│   └── ...
└── regtest/
    └── ...
```

## 2. NodeInfo Schema Contract

### Schema v1 (schemas/nodeinfo-v1.json)
- **Required fields**: rpc.url, ws.url, cookie, datadir, network
- **Optional fields**: mining_address, pid
- **Validation**: URL patterns, network enum, address format

### Generation Rules
1. Daemon MUST generate nodeinfo.json on startup
2. MUST use relative paths for cookie (e.g., ".cookie")
3. MUST validate schema before writing
4. MUST atomic write (temp + rename)

### CLI Discovery Rules
1. CLI MUST validate schema on read
2. MUST resolve relative paths against datadir
3. MUST fail fast on schema violations

## 3. Auth Improvements

### Cookie Security
- **Permissions**: MUST be 0600 (owner read/write only)
- **Content**: Random 32-byte base64 username:password
- **Location**: Always `.cookie` in network datadir

### Error Logging
- **HTTP 401**: "Authentication failed - check cookie file"
- **HTTP 403**: "Access denied - check permissions"
- **File missing**: "Cookie file not found: {path}"
- **Bad permissions**: "Cookie file has unsafe permissions: {perms} (expected 0600)"

### Validation Checklist
- [ ] Cookie file exists
- [ ] Cookie file has 0600 permissions
- [ ] Cookie content is valid base64
- [ ] Cookie decodes to username:password format
- [ ] RPC server accepts cookie auth

## Implementation Priority

1. **High**: Update daemon to use unified datadir paths
2. **High**: Implement nodeinfo.json schema validation
3. **Medium**: Add cookie permission checks
4. **Medium**: Improve auth error messages
5. **Low**: Add migration from old datadir locations

## Testing

```bash
# Test datadir discovery
./dinero-cli --print-nodeinfo

# Test schema validation
echo '{"invalid": "schema"}' > nodeinfo.json
./dinero-cli status  # Should fail with clear error

# Test cookie permissions
chmod 644 .cookie
./dinero-cli status  # Should warn about permissions
```

## Success Criteria

- [ ] All components use unified datadir naming
- [ ] NodeInfo schema v1 validated in CI
- [ ] Cookie permissions enforced with clear errors
- [ ] CLI auto-discovery works across all platforms
- [ ] Migration path from legacy datadir locations
