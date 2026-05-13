# Exchange Deployment Guide

## Overview

This guide explains how to deploy production-grade Dinero binaries on cryptocurrency exchanges. Dinero follows Bitcoin Core's architectural philosophy: **zero external dependencies, static linking, portable binaries**.

**Target Audience**: Exchange operators, infrastructure engineers, DevOps teams

## Why Dinero is Exchange-Ready

### Bitcoin Core Compatibility

| Feature | Bitcoin Core | Dinero |
|---------|-------------|---------|
| Static linking | ✅ Yes | ✅ Yes |
| Zero runtime dependencies | ✅ Yes | ✅ Yes |
| Reproducible builds | ✅ Yes | ✅ Yes |
| Release mode | ✅ Yes | ✅ Yes |
| Portable binaries | ✅ Yes | ✅ Yes |
| CI verification | ✅ Yes (`check-symbols.py`) | ✅ Yes (`verify_release_binary.sh`) |

### Dependency Philosophy

**Development Builds** (internal use only):
- Uses gRPC, protobuf, abseil for fast iteration
- ~80 Homebrew dependencies on macOS
- NOT suitable for exchange deployment

**Release Builds** (exchange-ready):
- Zero Homebrew dependencies
- Zero external runtime libraries
- Only system libraries (libc++, libSystem)
- Fully static where possible
- No package manager requirements

## Pre-Deployment Verification

### 1. Verify Binary Purity

Before deploying any Dinero binary, run the verification script:

```bash
./scripts/verify_release_binary.sh build/dinerod
```

**Expected Output (PASS)**:
```
═══════════════════════════════════════════════════════════════════════
Release Binary Verification: dinerod
═══════════════════════════════════════════════════════════════════════

Platform: macOS
Binary: build/dinerod

Total dynamic dependencies: 2

Test 1: Checking for Homebrew dependencies...
✅ PASSED: No Homebrew dependencies

Test 2: Checking for gRPC/protobuf/abseil...
✅ PASSED: No gRPC/protobuf/abseil dependencies

Test 3: Checking allowed system libraries...
✅ PASSED: Only system libraries linked

═══════════════════════════════════════════════════════════════════════
✅ ALL TESTS PASSED
═══════════════════════════════════════════════════════════════════════

Binary dinerod is release-grade:
  ✅ No Homebrew dependencies
  ✅ No gRPC/protobuf/abseil
  ✅ Only system libraries
  ✅ Exchange-ready

Dependencies (2 total):
/usr/lib/libSystem.B.dylib
/usr/lib/libc++.1.dylib
```

**If verification fails**, the binary is NOT production-ready. Rebuild with `DINERO_RELEASE=ON`.

### 2. Check Binary Dependencies

**macOS**:
```bash
otool -L build/dinerod
```

**Linux**:
```bash
ldd build/dinerod
```

**Expected**:
- macOS: Only `/usr/lib/libSystem.B.dylib` and `/usr/lib/libc++.1.dylib`
- Linux: Only `libc.so.6`, `libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `libm.so.6`

**Red Flags** (DO NOT DEPLOY):
- Any Homebrew paths (`/opt/homebrew/*`)
- Any gRPC/protobuf/abseil libraries
- Any non-system libraries

## Building Release Binaries

### Method 1: Manual Build (Recommended)

```bash
# Clone repository
git clone https://github.com/dinerocoin/dinerocoin.git
cd dinerocoin

# Initialize submodules (vendored dependencies)
git submodule update --init --recursive

# Configure release build
mkdir build
cd build
cmake -DDINERO_RELEASE=ON \
      -DCMAKE_BUILD_TYPE=Release \
      ..

# Build binaries
make clean
make dinerod -j$(nproc)

# Verify binary
../scripts/verify_release_binary.sh dinerod
```

### Method 2: Hermetic Build Script (Advanced)

```bash
./scripts/hermetic-build.sh dinerod
```

This script ensures:
- Clean build environment
- Reproducible builds
- Automatic verification
- Deterministic timestamps

### Method 3: CI/CD Pipeline (Production)

Use GitHub Actions workflow for reproducible builds:

```yaml
- name: Build release binary
  run: |
    cmake -DDINERO_RELEASE=ON -DCMAKE_BUILD_TYPE=Release ..
    make clean
    make dinerod -j$(nproc)

- name: Verify binary
  run: ./scripts/verify_release_binary.sh build/dinerod

- name: Upload artifact
  uses: actions/upload-artifact@v3
  with:
    name: dinerod-release
    path: build/dinerod
```

## Deployment Architecture

### Single-Server Deployment

```
┌─────────────────────────────────────┐
│  Exchange Infrastructure Server     │
│                                     │
│  ┌─────────────────────────────┐   │
│  │     dinerod (L1 node)       │   │
│  │  - Blockchain validation    │   │
│  │  - UTXO set management      │   │
│  │  - Mempool tracking         │   │
│  │  - RPC API (port 20996)     │   │
│  │  - Socket wallet server     │   │
│  └───────────┬─────────────────┘   │
│              │ Socket IPC          │
│  ┌───────────▼─────────────────┐   │
│  │  lightningd (L2 daemon)     │   │
│  │  - Lightning channels       │   │
│  │  - HTLC routing             │   │
│  │  - Invoice generation       │   │
│  │  - Payment processing       │   │
│  └─────────────────────────────┘   │
│                                     │
└─────────────────────────────────────┘
         │
         │ HTTPS
         ▼
  [Exchange API Backend]
```

### Multi-Server Deployment (High Availability)

```
          [Load Balancer]
                 │
      ┌──────────┼──────────┐
      │          │          │
   Server 1   Server 2   Server 3
   (dinerod)  (dinerod)  (dinerod)
      │          │          │
      └──────────┴──────────┘
             │
      [Shared Database]
   (wallet state, channels)
```

## Configuration

### dinerod Configuration

Create `~/.dinero/dinero.conf`:

```ini
# Network
testnet=0
regtest=0

# RPC Server
rpcport=20996
rpcallowip=127.0.0.1
rpcuser=dinero_exchange
rpcpassword=<strong-random-password>

# P2P Network
port=21001
maxconnections=125

# Wallet
disablewallet=0

# Logging
debug=0
logtimestamps=1

# Socket Wallet Server (Lightning IPC)
# Binds to 127.0.0.1:50051 by default
# No configuration needed
```

### lightningd Configuration

Create `~/.lightning/config`:

```ini
# Network
network=bitcoin

# dinerod Connection (Socket Mode)
wallet-server=127.0.0.1:50051

# Lightning P2P
bind-addr=0.0.0.0:9735

# RPC
rpc-file=/tmp/lightning-rpc

# Logging
log-level=info
```

## Running Binaries

### Starting dinerod

```bash
# Background mode
dinerod -daemon

# Foreground mode (recommended for monitoring)
dinerod

# Check status
dinero-cli getblockchaininfo
```

### Starting lightningd

```bash
# Background mode
lightningd --daemon

# Foreground mode
lightningd

# Check status
lightning-cli getinfo
```

## Monitoring

### Health Checks

**dinerod Health**:
```bash
# Check if daemon is running
dinero-cli getblockchaininfo

# Check sync status
dinero-cli getblockcount

# Check mempool
dinero-cli getmempoolinfo

# Check wallet
dinero-cli getbalance
```

**lightningd Health**:
```bash
# Check if daemon is running
lightning-cli getinfo

# Check channels
lightning-cli listchannels

# Check peers
lightning-cli listpeers
```

**Socket Wallet Server Health**:
```bash
# Check if socket server is listening
netstat -an | grep 50051

# Expected output (server listening):
tcp4       0      0  127.0.0.1.50051        *.*                    LISTEN
```

### Metrics

Monitor these key metrics:

1. **dinerod**:
   - Block height (sync status)
   - Mempool size
   - Peer count
   - UTXO set size
   - Disk usage
   - CPU/memory usage

2. **lightningd**:
   - Channel count
   - Channel capacity
   - Payment success rate
   - Routing fees earned
   - Peer count

3. **Socket Wallet Server**:
   - Active connections
   - Request rate
   - Error rate
   - Response latency

### Logging

**dinerod Logs**:
- Location: `~/.dinero/debug.log`
- Rotation: Automatic (by size)
- Level: Configurable (debug, info, warning, error)

**lightningd Logs**:
- Location: `~/.lightning/lightning.log`
- Rotation: Manual
- Level: Configurable

**Socket Wallet Server Logs**:
- Integrated with dinerod logs
- Prefix: `[SocketWalletServer]`
- Messages: Connection status, request/response logging

## Security

### Network Security

1. **Firewall Configuration**:
   ```bash
   # Allow P2P (mainnet)
   ufw allow 21001/tcp

   # Allow Lightning P2P
   ufw allow 9735/tcp

   # Block RPC from internet (localhost only)
   ufw deny 20996/tcp

   # Block socket wallet server (localhost only)
   # No rule needed - binds to 127.0.0.1 only
   ```

2. **RPC Authentication**:
   - Use strong passwords (`rpcpassword`)
   - Enable RPC auth (`rpcauth` for hashed passwords)
   - Restrict IPs (`rpcallowip=127.0.0.1`)

3. **Socket Wallet Server**:
   - Binds to `127.0.0.1` only (no external access)
   - No authentication (trusted localhost)
   - No encryption (localhost assumed secure)

### Data Security

1. **Wallet Encryption**:
   ```bash
   dinero-cli encryptwallet "<strong-passphrase>"
   ```

2. **Backup Strategy**:
   - Backup `~/.dinero/wallet.dat` daily
   - Backup `~/.lightning/hsm_secret` (Lightning node key)
   - Store backups encrypted and offline

3. **Key Management**:
   - Use HD wallets (BIP-32/44/49/84)
   - Derive Lightning keys from HD wallet
   - Never expose private keys

## Upgrade Procedure

### Pre-Upgrade Checklist

1. ✅ Read release notes
2. ✅ Test upgrade on testnet
3. ✅ Backup wallet and data
4. ✅ Verify new binary with `verify_release_binary.sh`
5. ✅ Schedule maintenance window

### Upgrade Steps

```bash
# 1. Stop daemons
lightning-cli stop
dinero-cli stop

# 2. Backup current binaries
cp build/dinerod build/dinerod.backup
cp build/lightningd build/lightningd.backup

# 3. Build new binaries
git pull
git checkout <new-version-tag>
cd build
cmake -DDINERO_RELEASE=ON -DCMAKE_BUILD_TYPE=Release ..
make clean
make dinerod lightningd -j$(nproc)

# 4. Verify new binaries
../scripts/verify_release_binary.sh dinerod
../scripts/verify_release_binary.sh lightningd

# 5. Start new binaries
./dinerod -daemon
./lightningd --daemon

# 6. Verify operation
dinero-cli getblockchaininfo
lightning-cli getinfo

# 7. Monitor for issues
tail -f ~/.dinero/debug.log
```

### Rollback Procedure

```bash
# If upgrade fails, rollback to backup binaries
cp build/dinerod.backup build/dinerod
cp build/lightningd.backup build/lightningd

# Restart with old binaries
./dinerod -daemon
./lightningd --daemon
```

## Performance Tuning

### dinerod Optimization

```ini
# Increase database cache (default: 450 MB)
dbcache=2048

# Increase max mempool size (default: 300 MB)
maxmempool=1000

# Increase max connections
maxconnections=250

# Enable transaction index (for exchange queries)
txindex=1
```

### lightningd Optimization

```ini
# Increase channel capacity limits
large-channels

# Enable MPP (multi-part payments)
experimental-offers

# Increase fee rate estimates
fee-base=1000
fee-per-una=1
```

### System Tuning

**Linux**:
```bash
# Increase file descriptor limit
ulimit -n 65536

# Increase max open files
echo "fs.file-max = 2097152" >> /etc/sysctl.conf
sysctl -p

# Optimize TCP for Bitcoin P2P
echo "net.ipv4.tcp_fin_timeout = 30" >> /etc/sysctl.conf
echo "net.ipv4.tcp_keepalive_time = 1200" >> /etc/sysctl.conf
```

## Troubleshooting

### Common Issues

**Issue**: Socket wallet server not starting
```
Error: Failed to bind to 127.0.0.1:50051: Address already in use
```
**Solution**: Port 50051 is occupied. Check if gRPC server is running or change port.

**Issue**: Lightning daemon can't connect to wallet
```
Error: Failed to connect to wallet server: Connection refused
```
**Solution**: Ensure dinerod is running and socket server is listening on 50051.

**Issue**: Binary has Homebrew dependencies
```
❌ FAILED: Found Homebrew dependencies
```
**Solution**: Rebuild with `DINERO_RELEASE=ON` and verify build mode.

### Debug Commands

```bash
# Check socket connections
netstat -an | grep 50051

# Check process status
ps aux | grep dinerod
ps aux | grep lightningd

# Check logs for errors
grep ERROR ~/.dinero/debug.log
grep ERROR ~/.lightning/lightning.log

# Test RPC connectivity
dinero-cli getblockchaininfo
lightning-cli getinfo
```

## Support

### Resources

- **Documentation**: https://github.com/dinerocoin/dinerocoin/tree/main/docs
- **Release Builds**: docs/RELEASE_BUILDS.md
- **Wire Protocol**: docs/WIRE_PROTOCOL_SPEC.md
- **CI Enforcement**: docs/CI_ENFORCEMENT.md

### Community

- **GitHub Issues**: https://github.com/dinerocoin/dinerocoin/issues
- **Discord**: https://discord.gg/dinerocoin
- **Telegram**: https://t.me/dinerocoin

### Professional Support

For exchange-specific integration support, contact: support@dinero-coin.com

---

**Status**: ✅ Production-ready - Exchange deployment validated

**Last Updated**: January 2026
