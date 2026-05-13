# Address API Examples

This document provides practical examples for the DineroCoin Address API, including valid usage patterns and common error scenarios.

## Valid Address Examples

### Regtest Network (HRP: "rdin")

```bash
# Generate a new address
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"getnewaddress","params":[],"id":1}' \
  http://127.0.0.1:20999/

# Response
{
  "id": 1,
  "jsonrpc": "2.0",
  "result": "rdin1q68f926c852932c64715f5fef05cdafba74eaf3f1",
  "rpc_schema": "din.rpc.v1",
  "schema_rev": 1
}
```

### Validate the Generated Address

```bash
# Validate the address
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["rdin1q68f926c852932c64715f5fef05cdafba74eaf3f1"],"id":1}' \
  http://127.0.0.1:20999/

# Response
{
  "id": 1,
  "jsonrpc": "2.0",
  "result": {
    "address": "rdin1q68f926c852932c64715f5fef05cdafba74eaf3f1",
    "isvalid": true,
    "iswitness": true,
    "isscript": false,
    "witness_version": 0,
    "witness_program": "68f926c852932c64715f5fef05cdafba74eaf3f1",
    "ismine": true,
    "account": "default"
  },
  "rpc_schema": "din.rpc.v1",
  "schema_rev": 1
}
```

## Common Error Scenarios

### 1. Wrong HRP (Network Mismatch)

```bash
# Mainnet address on regtest (should fail)
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["din1q68f926c852932c64715f5fef05cdafba74eaf3f1"],"id":1}' \
  http://127.0.0.1:20999/

# Response
{
  "id": 1,
  "jsonrpc": "2.0",
  "result": {
    "address": "din1q68f926c852932c64715f5fef05cdafba74eaf3f1",
    "isvalid": false,
    "ismine": false
  },
  "rpc_schema": "din.rpc.v1",
  "schema_rev": 1
}
```

**Error Reason**: `hrp_mismatch` - HRP "din" doesn't match expected "rdin" for regtest

### 2. Mixed-Case Address

```bash
# Mixed-case address (should fail)
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["rDnR1q68f926c852932c64715f5fef05cdafba74eaf3f1"],"id":1}' \
  http://127.0.0.1:20999/

# Response
{
  "id": 1,
  "jsonrpc": "2.0",
  "result": {
    "address": "rDnR1q68f926c852932c64715f5fef05cdafba74eaf3f1",
    "isvalid": false,
    "ismine": false
  },
  "rpc_schema": "din.rpc.v1",
  "schema_rev": 1
}
```

**Error Reason**: `mixed_case` - Bech32 addresses must be lowercase

### 3. Invalid Checksum

```bash
# Address with invalid checksum (should fail)
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["rdin1q68f926c852932c64715f5fef05cdafba74eaf3f2"],"id":1}' \
  http://127.0.0.1:20999/

# Response
{
  "id": 1,
  "jsonrpc": "2.0",
  "result": {
    "address": "rdin1q68f926c852932c64715f5fef05cdafba74eaf3f2",
    "isvalid": false,
    "ismine": false
  },
  "rpc_schema": "din.rpc.v1",
  "schema_rev": 1
}
```

**Error Reason**: `checksum_invalid` - Bech32 checksum validation failed

### 4. Bech32m (witver >= 1)

```bash
# Bech32m address (should fail - not yet supported)
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["rdin1p68f926c852932c64715f5fef05cdafba74eaf3f1"],"id":1}' \
  http://127.0.0.1:20999/

# Response
{
  "id": 1,
  "jsonrpc": "2.0",
  "result": {
    "address": "rdin1p68f926c852932c64715f5fef05cdafba74eaf3f1",
    "isvalid": false,
    "witness_version": 1,
    "ismine": false
  },
  "rpc_schema": "din.rpc.v1",
  "schema_rev": 1
}
```

**Error Reason**: `bech32m_not_supported` - Bech32m (witver >= 1) not yet implemented

### 5. Malformed Address

```bash
# Completely invalid address (should fail)
curl -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"wallet.validateaddress","params":["invalid"],"id":1}' \
  http://127.0.0.1:20999/

# Response
{
  "id": 1,
  "jsonrpc": "2.0",
  "result": {
    "address": null,
    "isvalid": null,
    "ismine": null
  },
  "rpc_schema": "din.rpc.v1",
  "schema_rev": 1
}
```

**Error Reason**: `malformed` - Address doesn't match Bech32 format

## Address Types

### P2WPKH (Pay-to-Witness-Public-Key-Hash)
- **Witness Version**: 0
- **Program Length**: 20 bytes
- **Script Type**: `isscript: false`
- **Example**: `rdin1q68f926c852932c64715f5fef05cdafba74eaf3f1`

### P2WSH (Pay-to-Witness-Script-Hash)
- **Witness Version**: 0
- **Program Length**: 32 bytes
- **Script Type**: `isscript: true`
- **Example**: `rdin1q68f926c852932c64715f5fef05cdafba74eaf3f168f926c852932c64715f5fef05cdafba74eaf3f1`

## Network-Specific HRPs

| Network | HRP | Example |
|---------|-----|---------|
| Regtest | `rdin` | `rdin1q...` |
| Testnet | `tdin` | `tdin1q...` |
| Mainnet | `din` | `din1q...` |

## ScriptPubKey Construction

For valid P2WPKH addresses, the scriptPubKey is constructed as:
```
OP_0 (0x00) + PUSH20 (0x14) + 20-byte witness program
```

Example:
- Address: `rdin1q68f926c852932c64715f5fef05cdafba74eaf3f1`
- Witness Program: `68f926c852932c64715f5fef05cdafba74eaf3f1`
- ScriptPubKey: `001468f926c852932c64715f5fef05cdafba74eaf3f1`

## Best Practices

### 1. Always Validate Addresses
```bash
# Before using an address, validate it
VALID=$(curl -s -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"wallet.validateaddress\",\"params\":[\"$ADDRESS\"],\"id\":1}" \
  http://127.0.0.1:20999/ | jq -r '.result.isvalid')

if [ "$VALID" = "true" ]; then
  echo "Address is valid"
else
  echo "Address is invalid"
fi
```

### 2. Check Network Compatibility
```bash
# Ensure address matches current network
HRP=$(curl -s -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"getbuildinfo","params":[],"id":1}' \
  http://127.0.0.1:20999/ | jq -r '.result.network')

case "$HRP" in
  "regtest") EXPECTED_HRP="rdin" ;;
  "testnet") EXPECTED_HRP="tdin" ;;
  "mainnet") EXPECTED_HRP="din" ;;
esac

if [[ "$ADDRESS" == "$EXPECTED_HRP"* ]]; then
  echo "Address matches current network"
else
  echo "Address network mismatch"
fi
```

### 3. Handle Errors Gracefully
```bash
# Robust address validation with error handling
validate_address() {
  local addr="$1"
  local result
  
  result=$(curl -s -X POST -H "Content-Type: application/json" \
    -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
    -d "{\"jsonrpc\":\"2.0\",\"method\":\"wallet.validateaddress\",\"params\":[\"$addr\"],\"id\":1}" \
    http://127.0.0.1:20999/)
  
  if [ $? -ne 0 ]; then
    echo "RPC call failed"
    return 1
  fi
  
  local isvalid=$(echo "$result" | jq -r '.result.isvalid')
  local witness_program=$(echo "$result" | jq -r '.result.witness_program')
  
  if [ "$isvalid" = "true" ]; then
    echo "Valid address: $addr"
    echo "Witness program: $witness_program"
    return 0
  else
    echo "Invalid address: $addr"
    return 1
  fi
}
```

## Troubleshooting

### Common Issues

1. **"Address is invalid" but looks correct**
   - Check HRP matches current network
   - Ensure lowercase only
   - Verify checksum

2. **"No witness program"**
   - Address may be malformed
   - Check Bech32 encoding
   - Verify program length (20 or 32 bytes)

3. **"Network mismatch"**
   - Regtest expects "rdin" prefix
   - Testnet expects "tdin" prefix
   - Mainnet expects "din" prefix

### Debug Commands

```bash
# Check current network
curl -s -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"getbuildinfo","params":[],"id":1}' \
  http://127.0.0.1:20999/ | jq '.result.network'

# Generate a known-good address
curl -s -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d '{"jsonrpc":"2.0","method":"getnewaddress","params":[],"id":1}' \
  http://127.0.0.1:20999/ | jq -r '.result'

# Validate the generated address
curl -s -X POST -H "Content-Type: application/json" \
  -H "Authorization: Basic $(printf '__cookie__:%s' "$COOKIE" | base64)" \
  -d "{\"jsonrpc\":\"2.0\",\"method\":\"wallet.validateaddress\",\"params\":[\"$GENERATED_ADDRESS\"],\"id\":1}" \
  http://127.0.0.1:20999/ | jq '.result'
```
