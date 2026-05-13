# DineroCoin Wallet & Mining Integration Guide

This guide provides comprehensive documentation for the DineroCoin wallet and mining RPC integration with copy-pastable examples for quick start.

## Table of Contents
- [Overview](#overview)
- [RPC Methods](#rpc-methods)
- [CLI Commands](#cli-commands)
- [GUI Integration](#gui-integration)
- [Quick Start Examples](#quick-start-examples)
- [Error Handling](#error-handling)
- [Security Considerations](#security-considerations)

## Overview

DineroCoin implements a daemon-centric architecture where:
- **Daemon** is the single source of truth for keys, addresses, balances, and mining policy
- **Mining addresses must be wallet-owned** or mining is disabled
- **Persistent storage** with validation on daemon restart
- **Zero-CLI flow** via modern Qt6 GUI with Receive & Mine panel

## RPC Methods

### Wallet Methods

#### `wallet.getnewaddress`
Generate a new HD wallet address.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "wallet.getnewaddress",
  "params": []
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"
}
```

#### `wallet.validateaddress`
Validate address and check wallet ownership.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "wallet.validateaddress",
  "params": ["rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"]
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "isvalid": true,
    "ismine": true,
    "account": "default",
    "address": "rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"
  }
}
```

### Mining Methods

#### `mining.setaddress`
Set mining payout address (must be wallet-owned).

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "mining.setaddress",
  "params": ["rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"]
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": "Mining address set successfully"
}
```

#### `mining.getaddress`
Get current mining payout address.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "mining.getaddress",
  "params": []
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "address": "rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh",
    "ismine": true,
    "source": "configured"
  }
}
```

#### `mining.generatetoaddress` (Regtest Only)
Generate blocks to specified address.

**Request:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "mining.generatetoaddress",
  "params": [5, "rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh"]
}
```

**Response:**
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": [
    "0000abc123...",
    "0000def456...",
    "0000ghi789...",
    "0000jkl012...",
    "0000mno345..."
  ]
}
```

## CLI Commands

### Wallet Commands

```bash
# Generate new address
dinero-cli getnewaddress

# Validate address
dinero-cli validateaddress rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh
```

### Mining Commands

```bash
# Set mining address
dinero-cli setminingaddress rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh

# Get mining address
dinero-cli getminingaddress

# Generate blocks (regtest only)
dinero-cli generatetoaddress 5 rdin1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh
```

## GUI Integration

The modern Qt6 GUI includes a **Receive & Mine** panel with:

- **Address Generation**: Generate new wallet addresses
- **Address Validation**: Inline validation with ownership indication
- **Mining Setup**: Set mining payout address with ownership verification
- **Block Generation**: Mine blocks in regtest mode
- **Status Display**: Current mining address and coinbase maturity

## Quick Start Examples

### 1. Basic Wallet Setup

```bash
# Start daemon in regtest mode
dinerod -regtest -daemon

# Create and load wallet
dinero-cli wallet.create my_wallet password123
dinero-cli wallet.load my_wallet password123

# Generate receiving address
ADDRESS=$(dinero-cli wallet.getnewaddress)
echo "New address: $ADDRESS"

# Validate address ownership
dinero-cli wallet.validateaddress $ADDRESS
```

### 2. Mining Configuration

```bash
# Set mining address (must be wallet-owned)
dinero-cli mining.setaddress $ADDRESS

# Verify mining configuration
dinero-cli mining.getaddress

# Generate test blocks (regtest only)
dinero-cli mining.generatetoaddress 10 $ADDRESS

# Check balance
dinero-cli wallet.getbalance
```

### 3. Python RPC Client Example

```python
import requests
import json
import base64

# RPC configuration
rpc_url = "http://127.0.0.1:20998"
cookie_path = "~/.dinero/regtest/.cookie"

# Read auth cookie
with open(cookie_path) as f:
    cookie = f.read().strip()
    
auth_header = "Basic " + base64.b64encode(cookie.encode()).decode()

def rpc_call(method, params=None):
    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": method,
        "params": params or []
    }
    
    headers = {
        "Authorization": auth_header,
        "Content-Type": "application/json"
    }
    
    response = requests.post(rpc_url, json=payload, headers=headers)
    return response.json()["result"]

# Generate address and set mining
address = rpc_call("wallet.getnewaddress")
print(f"Generated address: {address}")

rpc_call("mining.setaddress", [address])
print("Mining address set")

# Generate blocks
blocks = rpc_call("mining.generatetoaddress", [5, address])
print(f"Generated {len(blocks)} blocks")
```

### 4. WebSocket Client Example

```javascript
const WebSocket = require('ws');

// Connect to WebSocket RPC
const ws = new WebSocket('ws://127.0.0.1:21000/ws', {
  headers: {
    'Authorization': 'Basic ' + Buffer.from(cookie).toString('base64')
  }
});

ws.on('open', () => {
  // Generate new address
  ws.send(JSON.stringify({
    jsonrpc: '2.0',
    id: 1,
    method: 'wallet.getnewaddress',
    params: []
  }));
});

ws.on('message', (data) => {
  const response = JSON.parse(data);
  console.log('Address:', response.result);
});
```

## Error Handling

### Common Error Cases

#### Non-Wallet-Owned Mining Address
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -1,
    "message": "Address not owned by active wallet. Mining rewards will not be spendable."
  }
}
```

#### Invalid Address Format
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -1,
    "message": "Invalid mining address: Invalid Bech32 encoding"
  }
}
```

#### No Active Wallet
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "error": {
    "code": -1,
    "message": "No active wallet loaded"
  }
}
```

### Error Handling Best Practices

1. **Always validate addresses** before setting as mining address
2. **Check wallet ownership** using `wallet.validateaddress`
3. **Handle network-specific addresses** (mainnet: `din1`, regtest: `rdin1`)
4. **Implement retry logic** for transient network errors
5. **Log errors appropriately** for debugging

## Security Considerations

### Mining Address Security
- **Wallet ownership validation** prevents mining to unspendable addresses
- **Address persistence** with validation on daemon startup
- **Mutex protection** for thread-safe mining address updates
- **File-based persistence** in `datadir/mining_address.txt`

### RPC Security
- **Cookie-based authentication** required for all RPC calls
- **WebSocket authentication** enforced on connection upgrade
- **No fallback connections** without proper authentication
- **Message size limits** and backpressure protection

### Best Practices
1. **Always use wallet-owned addresses** for mining payouts
2. **Validate addresses** before setting mining configuration
3. **Monitor mining address ownership** after wallet operations
4. **Use secure RPC connections** in production environments
5. **Regularly backup wallet files** and mining configuration

## Integration Testing

Run the comprehensive coinbase correctness test:

```bash
cd /path/to/DineroCoin
python3 tests/test_coinbase_correctness.py
```

This test verifies:
- Wallet creation and address generation
- Mining address ownership validation
- Block generation and coinbase correctness
- Address persistence across daemon restarts
- NodeInfo mining address integration

## Troubleshooting

### Common Issues

1. **Mining address not persisting**: Check datadir permissions and disk space
2. **Address ownership false**: Ensure wallet is loaded and address was generated by wallet
3. **RPC connection failed**: Verify daemon is running and ports are accessible
4. **Invalid address format**: Check network prefix (din1 vs rdin1)

### Debug Commands

```bash
# Check daemon status
dinero-cli getinfo

# Verify wallet status
dinero-cli wallet.info

# Check mining configuration
dinero-cli mining.getaddress

# List wallet addresses
dinero-cli wallet.listaddresses

# Check nodeinfo.json
cat ~/.dinero/regtest/nodeinfo.json
```

## Support

For additional support:
- Check the [DineroCoin Documentation](../README.md)
- Review [Architecture Documentation](ARCHITECTURE.md)
- Submit issues on GitHub
- Join the community Discord

---

*This documentation covers DineroCoin v0.6.0+ wallet and mining integration.*
