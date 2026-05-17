# DineroCoin v0.6.0 Release Notes

**Release Date:** September 7, 2025  
**Codename:** "Ephemeral Ports + GUI Mining Flow"

## 🎯 Major Features

### Ephemeral Port Support
- **Dynamic Port Allocation**: Use `-rpcport=0`, `-wsport=0`, `-port=0` for automatic port assignment
- **Test-Friendly**: Perfect for CI/CD environments and parallel testing
- **Auto-Discovery**: Actual ports written to `nodeinfo.json` for client discovery

### GUI Receive & Mine Panel
- **Zero-CLI Flow**: Complete wallet and mining operations via modern Qt6 interface
- **Address Generation**: Generate new HD wallet addresses with one click
- **Address Validation**: Real-time validation with ownership indicators
- **Mining Setup**: Set wallet-owned mining addresses with security validation
- **Block Generation**: Mine test blocks in regtest mode directly from GUI

### Wallet & Mining RPC Integration
- **Wallet Methods**: `wallet.getnewaddress`, `wallet.validateaddress`
- **Mining Methods**: `mining.setaddress`, `mining.getaddress`, `mining.generatetoaddress`
- **Security-First**: Mining addresses must be wallet-owned or mining is disabled
- **Persistent Storage**: Mining addresses saved to `datadir/mining_address.txt`

## 🔧 Technical Improvements

### Daemon-Centric Architecture
- **Single Source of Truth**: Daemon manages all keys, addresses, balances, and mining policy
- **Thread-Safe**: Mutex-protected mining address management
- **Startup Validation**: Mining addresses validated on daemon restart

### Enhanced CLI
- **Full Parity**: CLI commands for all wallet and mining operations
- **User-Friendly**: Emoji indicators and clear success/error messages
- **WebSocket Support**: Auto-fallback from WebSocket to HTTP transport

### NodeInfo Schema v1
```json
{
  "rpc": {"url": "http://127.0.0.1:20998"},
  "ws": {"url": "ws://127.0.0.1:21000/ws"},
  "cookie": "/path/to/.cookie",
  "datadir": "/path/to/datadir",
  "network": "regtest",
  "mining_address": "rdin1qxy2kg..."
}
```

## 🛡️ Security Enhancements

### Mining Address Security
- **Wallet Ownership Validation**: Only wallet-owned addresses accepted for mining
- **Network Compatibility**: Address validation enforces correct network prefixes
- **Startup Validation**: Invalid saved addresses removed on daemon startup

### RPC Security
- **Comprehensive Validation**: All RPC methods validate parameters and types
- **Error Handling**: Detailed error messages with actionable feedback
- **Regtest Guards**: `mining.generatetoaddress` restricted to regtest mode

## 📚 Documentation & Examples

### Complete Integration Guide
- **Copy-Pastable Examples**: Ready-to-use code snippets for Python, JavaScript, CLI
- **Security Best Practices**: Comprehensive security considerations
- **Troubleshooting Guide**: Common issues and debug commands

### WebSocket Client Examples
- **Python**: `examples/websocket_client.py`
- **JavaScript**: `examples/websocket_client.js`
- **Full Workflow**: Wallet creation → Address generation → Mining setup → Block generation

## 🧪 Testing & Quality

### Comprehensive Test Suite
- **Coinbase Correctness**: End-to-end mining payout validation
- **WebSocket Authentication**: Invalid/expired cookie handling
- **Schema Validation**: CI guards for `nodeinfo.json` format
- **Integration Tests**: Complete wallet and mining workflows

## 🚀 Quick Start

### 1. Start Daemon (Regtest)
```bash
dinerod -regtest -daemon
```

### 2. Create Wallet & Generate Address
```bash
dinero-cli wallet.create my_wallet password123
dinero-cli wallet.load my_wallet password123
ADDRESS=$(dinero-cli wallet.getnewaddress)
```

### 3. Setup Mining
```bash
dinero-cli mining.setaddress $ADDRESS
dinero-cli mining.generatetoaddress 10 $ADDRESS
```

### 4. Use GUI
Launch the modern Qt6 GUI and use the "Receive & Mine" tab for zero-CLI workflow.

## 🔄 Migration Guide

### From v0.5.x
- **No Breaking Changes**: All existing RPC methods continue to work
- **New Methods**: Additional wallet and mining RPC methods available
- **Enhanced Security**: Mining addresses now require wallet ownership

### Configuration
- **Ephemeral Ports**: Add `-rpcport=0 -wsport=0 -port=0` for dynamic allocation
- **Mining Setup**: Use `mining.setaddress` to configure wallet-owned mining addresses

## 📋 API Reference

### New RPC Methods

#### `wallet.getnewaddress`
Generate new HD wallet address
```json
{"jsonrpc": "2.0", "method": "wallet.getnewaddress", "params": []}
```

#### `wallet.validateaddress`
Validate address and check ownership
```json
{"jsonrpc": "2.0", "method": "wallet.validateaddress", "params": ["rdin1q..."]}
```

#### `mining.setaddress`
Set wallet-owned mining payout address
```json
{"jsonrpc": "2.0", "method": "mining.setaddress", "params": ["rdin1q..."]}
```

#### `mining.getaddress`
Get current mining address with ownership status
```json
{"jsonrpc": "2.0", "method": "mining.getaddress", "params": []}
```

#### `mining.generatetoaddress` (Regtest Only)
Generate blocks to specified address
```json
{"jsonrpc": "2.0", "method": "mining.generatetoaddress", "params": [5, "rdin1q..."]}
```

## 🐛 Bug Fixes
- Fixed port collision detection for ephemeral ports
- Improved WebSocket connection stability
- Enhanced error messages for invalid addresses
- Fixed mining address persistence across restarts

## 🔮 What's Next (v0.7.0)
- Start/Stop mining RPC with thread control
- WSS/TLS support via reverse proxy
- Descriptor wallet import/export
- Watch-only mining addresses
- Homebrew tap for easy installation

## 💾 Download

### Binaries
- **macOS**: `DineroCoin-v0.6.0-macOS.dmg`
- **Linux**: `DineroCoin-v0.6.0-Linux.AppImage`
- **Windows**: `DineroCoin-v0.6.0-Windows.exe`

### Source
```bash
git clone https://github.com/dinero-org/dinero.git
git checkout v0.6.0
```

## 🙏 Acknowledgments

Special thanks to the community for testing, feedback, and contributions that made this release possible.

---

**Full Changelog**: https://github.com/dinero-org/dinero/compare/v0.5.0...v0.6.0
