# 🚀 DineroCoin v0.6.0 - Minimal Release

## Core Features

### 🏦 Daemon-Centric Architecture
- **Complete wallet & mining RPC integration** with daemon as single source of truth
- **Mining address ownership validation** - ensures mining rewards are spendable
- **Security hardening** with cookie permissions and loopback binding validation
- **Persistent configuration** with startup validation and automatic cleanup

### 🌐 WebSocket RPC & Authentication  
- **Full-duplex WebSocket RPC** alongside HTTP RPC for real-time applications
- **Cookie-based authentication** with strict 0600 permissions enforcement
- **JSON-RPC 2.0 compliance** with comprehensive error handling
- **Message limits and backpressure** protection

### 🔧 Ephemeral Port Support
- **Dynamic port allocation** via `-rpcport=0 -wsport=0 -port=0` for CI/CD environments
- **CLI auto-discovery** finds daemon endpoints via nodeinfo.json automatically
- **Test-friendly** design enables parallel testing and seamless CI integration

## 🛡️ **Security Enhancements**

- **Cookie Permissions**: Automatic 0600 permission enforcement
- **Loopback Validation**: Warnings for non-localhost binding
- **Mining Address Validation**: Startup validation with HRP checking
- **WebSocket Auth**: Strict cookie-based authentication with 401 rejections

## 🧪 **Testing & Quality**

- **JSON Schema Validation**: CI guards for `nodeinfo.json` format
- **WebSocket Auth Tests**: Comprehensive invalid/expired cookie handling
- **Integration Tests**: End-to-end wallet and mining workflows
- **Client Examples**: Python and JavaScript WebSocket clients

## 📚 **Documentation**

## Minimal Release Artifacts

This v0.6.0 release focuses on **core daemon and CLI functionality** for maximum stability:

- ✅ **dinerod** - Full-featured daemon with wallet & mining RPC
- ✅ **dinero-cli** - Command-line client with auto-discovery
- ✅ **Minimal dependencies** - No GUI/Qt requirements
- ✅ **Production-ready** - Security hardened and thoroughly tested

## Quick Start

### 1. Download & Extract
```bash
# Download for macOS
wget https://github.com/Trucker2827/Dinero-Coin/releases/download/v0.6.0/dinerocoin-macOS-v0.6.0.tar.gz
tar -xzf dinerocoin-macOS-v0.6.0.tar.gz
```

### 2. Start Daemon (Regtest)
```bash
# Start daemon with ephemeral ports
./dinerod -regtest -rpcport=0 -wsport=0 -port=0

# Check auto-generated nodeinfo.json for actual ports
cat ~/.dinero/regtest/nodeinfo.json
```

### 3. Use CLI Auto-Discovery
```bash
# CLI automatically discovers daemon endpoints
./dinero-cli -regtest getbestblockhash

# Create wallet and generate address
./dinero-cli -regtest wallet.create my_wallet
./dinero-cli -regtest wallet.getnewaddress

# Set mining address and generate blocks
./dinero-cli -regtest mining.setaddress rdin1...
./dinero-cli -regtest mining.generatetoaddress 10 rdin1...
```

## 🧪 **Smoke Test**

Run the included smoke test to verify your installation:

```bash
./scripts/smoke_test_v0.6.0.sh
```

## 📦 **Downloads**

| Platform | File | SHA256 |
|----------|------|--------|
| macOS | `DineroCoin-v0.6.0-macOS.dmg` | `[checksum]` |
| Linux | `DineroCoin-v0.6.0-Linux.AppImage` | `[checksum]` |
| Windows | `DineroCoin-v0.6.0-Windows.exe` | `[checksum]` |

## 🔄 **Migration from v0.5.x**

- ✅ **No Breaking Changes**: All existing RPC methods continue to work
- ✅ **Enhanced Security**: Mining addresses now require wallet ownership
- ✅ **New Features**: Additional wallet and mining RPC methods available

## 🔮 **What's Next (v0.7.0)**

- Start/Stop mining RPC with thread control
- WSS/TLS support via reverse proxy
- Descriptor wallet import/export
- Watch-only mining addresses
- Homebrew tap for easy installation

---

**Full Changelog**: https://github.com/dinero-org/dinero/compare/v0.5.0...v0.6.0

**## Documentation**

- [CLI Deployment Guide](docs/CLI_DEPLOYMENT_GUIDE.md)
- [Production Readiness](docs/production/PRODUCTION_READINESS.md)
- [Backup Runbook](docs/production/BACKUP_RUNBOOK.md)
- [Filesystem Requirements](docs/production/FILESYSTEM_REQUIREMENTS.md) - **Critical for production durability**

**Examples**: [WebSocket Clients](examples/) | [Smoke Test](scripts/smoke_test_v0.6.0.sh)
