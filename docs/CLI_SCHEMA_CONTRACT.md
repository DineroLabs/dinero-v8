# DineroCoin CLI JSON Schema Contract

## Overview

The DineroCoin CLI uses a versioned JSON envelope format (`din.cli.v1`) to ensure backward compatibility and enable safe automation. All JSON output follows this contract.

## Schema Version: `din.cli.v1`

### Base Envelope Structure

```json
{
  "schema": "din.cli.v1",
  "command": "wallet_balance",
  "network": "main",
  "rpc_url": "https://node1.example.com:20998/wallet/exchange",
  "wallet": "exchange",
  "ok": true,
  "data": { /* command-specific data */ },
  "page": { /* pagination metadata (optional) */ }
}
```

### Required Fields

| Field | Type | Description |
|-------|------|-------------|
| `schema` | string | Always `"din.cli.v1"` |
| `command` | string | Command executed (e.g., `"wallet_balance"`, `"nodeinfo"`) |
| `network` | string | Network type: `"main"`, `"testnet"`, `"regtest"` |
| `rpc_url` | string | RPC endpoint used |
| `wallet` | string\|null | Wallet name or `null` for node-level commands |
| `ok` | boolean | `true` for success, `false` for errors |
| `data` | object\|array | Command result data |

### Optional Fields

| Field | Type | Description |
|-------|------|-------------|
| `page` | object | Pagination metadata (when using `--limit`, `--offset`) |
| `error` | object | Error details when `ok: false` |

## Pagination Envelope

When using paging flags (`--limit`, `--offset`, `--cursor`), the `page` object contains:

```json
{
  "page": {
    "limit": 100,
    "returned": 100,
    "offset": 0,
    "has_more": true,
    "next_offset": 100,
    "cursor": null,
    "filters": {
      "confirmed_only": true,
      "min_amount": 0.01
    }
  }
}
```

### Pagination Fields

| Field | Type | Description |
|-------|------|-------------|
| `limit` | number | Requested page size |
| `returned` | number | Actual items returned |
| `offset` | number | Current offset |
| `has_more` | boolean | More data available |
| `next_offset` | number | Offset for next page |
| `cursor` | string\|null | Opaque pagination token |
| `filters` | object | Active filters applied |

## Error Envelope

When `ok: false`, the envelope includes error details:

```json
{
  "schema": "din.cli.v1",
  "command": "wallet_balance",
  "network": "main",
  "rpc_url": "https://node1.example.com:20998/wallet/missing",
  "wallet": "missing",
  "ok": false,
  "data": null,
  "error": {
    "code": 69,
    "message": "Wallet 'missing' not found",
    "type": "wallet_not_found"
  }
}
```

## Command Examples

### Node Information
```json
{
  "schema": "din.cli.v1",
  "command": "nodeinfo",
  "network": "main",
  "rpc_url": "https://node1.example.com:20998",
  "wallet": null,
  "ok": true,
  "data": {
    "version": "1.0.0",
    "blocks": 850000,
    "connections": 8,
    "difficulty": 12345678.90
  }
}
```

### Wallet Balance
```json
{
  "schema": "din.cli.v1",
  "command": "wallet_balance",
  "network": "main",
  "rpc_url": "https://node1.example.com:20998/wallet/exchange",
  "wallet": "exchange",
  "ok": true,
  "data": {
    "confirmed": 1234.56789,
    "unconfirmed": 0.0,
    "total": 1234.56789
  }
}
```

### Paginated Transaction History
```json
{
  "schema": "din.cli.v1",
  "command": "wallet_history",
  "network": "main",
  "rpc_url": "https://node1.example.com:20998/wallet/exchange",
  "wallet": "exchange",
  "ok": true,
  "data": [
    {
      "txid": "abc123...",
      "amount": 10.0,
      "confirmations": 6,
      "time": "2024-01-15T10:30:00Z"
    }
  ],
  "page": {
    "limit": 100,
    "returned": 1,
    "offset": 0,
    "has_more": false,
    "next_offset": null,
    "cursor": null,
    "filters": {
      "min_conf": 1
    }
  }
}
```

### UTXO List with Filters
```json
{
  "schema": "din.cli.v1",
  "command": "wallet_utxos",
  "network": "main",
  "rpc_url": "https://node1.example.com:20998/wallet/exchange",
  "wallet": "exchange",
  "ok": true,
  "data": [
    {
      "txid": "def456...",
      "vout": 0,
      "amount": 5.0,
      "confirmations": 10,
      "address": "DIN1abc..."
    }
  ],
  "page": {
    "limit": 500,
    "returned": 1,
    "offset": 0,
    "has_more": false,
    "next_offset": null,
    "cursor": null,
    "filters": {
      "confirmed_only": true,
      "min_amount": 0.01
    }
  }
}
```

## Automation Guidelines

### Parsing Strategy
```bash
# Extract success status
jq -r '.ok' response.json

# Get data payload
jq -r '.data' response.json

# Check for more pages
jq -r '.page.has_more // false' response.json

# Get next offset
jq -r '.page.next_offset // null' response.json
```

### Pagination Loop
```bash
#!/bin/bash
offset=0
while true; do
  response=$(dinero-cli --format json wallet history --limit 100 --offset $offset)
  
  # Process data
  echo "$response" | jq -r '.data[]'
  
  # Check if more pages
  has_more=$(echo "$response" | jq -r '.page.has_more // false')
  if [ "$has_more" != "true" ]; then
    break
  fi
  
  # Get next offset
  offset=$(echo "$response" | jq -r '.page.next_offset')
done
```

### Error Handling
```bash
#!/bin/bash
response=$(dinero-cli --format json wallet balance 2>/dev/null)
ok=$(echo "$response" | jq -r '.ok')

if [ "$ok" = "true" ]; then
  balance=$(echo "$response" | jq -r '.data.total')
  echo "Balance: $balance DIN"
else
  error_msg=$(echo "$response" | jq -r '.error.message')
  echo "Error: $error_msg" >&2
  exit 1
fi
```

## Versioning Policy

### Current Version: `din.cli.v1`
- **Stable**: No breaking changes to existing fields
- **Additive**: New optional fields may be added
- **Backward compatible**: Existing automation continues to work

### Breaking Changes
- Require new schema version (e.g., `din.cli.v2`)
- Announced with deprecation warnings
- Supported migration path provided

### Schema Evolution
```json
{
  "schema": "din.cli.v1",
  "deprecated_fields": ["old_field"],
  "migration_guide": "https://docs.dinero-coin.com/cli/v2-migration"
}
```

## Validation

### Schema Validation
```bash
# Verify schema version
jq -e '.schema == "din.cli.v1"' response.json

# Validate required fields
jq -e 'has("schema") and has("command") and has("ok") and has("data")' response.json

# Check pagination structure
jq -e '.page | has("limit") and has("returned") and has("has_more")' response.json
```

### Testing Contract Compliance
```bash
# Run CLI with JSON output
dinero-cli --format json nodeinfo > test.json

# Validate schema
jsonschema -i test.json cli-schema-v1.json

# Test pagination
dinero-cli --format json wallet history --limit 2 | jq '.page'
```
