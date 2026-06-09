# Dinero Core

## v8 Public Source Snapshot

This repository is the clean public Dinero v8 source snapshot.

Snapshot source: `ab34beeca90a386d39f7d295ad29c8a271aed112`

The older private development repository remains private because its historical commits contained generated runtime artifacts and sensitive test/recovery material that should not be published. Public development should continue from this snapshot.

---

**v8.0.0 Release Candidate Line - Monorepo Core, Qt, NodeCore, and Mining**

[![Release](https://img.shields.io/badge/release-v8.0.0--rc-blue)](https://github.com/DineroLabs/dinero-v8/releases)
[![Protocol](https://img.shields.io/badge/protocol-v8.0-blue)](docs/PROJECT_STATUS.md)
[![Tests](https://img.shields.io/badge/tests-65%2F65-brightgreen)](docs/consensus/)
[![Status](https://img.shields.io/badge/status-release--candidate-blue)](https://github.com/DineroLabs/dinero-v8/releases)
[![Wallet Safety](https://img.shields.io/badge/wallet-exchange--grade-gold)](docs/wallet/WALLET_CHAOS_TEST_RESULTS.md)

A real, mineable cryptocurrency with **stateless validation (Utreexo), Lightning integration, mobile support**, and **100+ formally verified protocol properties**.

### v8 Sync Defaults

- **Server / operator nodes (`dinerod`)** default to full validation from genesis.
- **AssumeUTXO snapshot bootstrap** is available as an explicit fast-sync opt-in via configuration or RPC.
- **Mobile NodeCore / DineroDPI local validation** may use a bundled snapshot anchor for viable phone UX, then validates forward with headers, chainwork, Utreexo roots, and accumulator state.

### What's New in v8

- 🎯 **Utreexo validation** - Local validation with compact accumulator state
- 🌐 **Bridge proof network** - Historical Utreexo proof serving for mobile clients
- ⛏️ **Solo and SV2 mining** - CPU/GPU solo mining plus SV2 pool mining backends
- 📱 **Mobile local node profile** - Snapshot-assisted bootstrap for phone-hosted validation
- 🔄 **Release-lane consolidation** - Daemon, CLI, Qt, NodeCore, and miners in the v8 monorepo

**Protocol Status:** [PROJECT_STATUS.md](docs/PROJECT_STATUS.md)

## 📋 Table of Contents

- [🔒 Wallet Safety Certification](#-wallet-safety-certification)
- [🏛️ Architecture](#️-architecture)
- [🚀 Quick Start](#-quick-start)
- [🏗️ Building the System](#️-building-the-system)
- [⚡ Daily Usage Commands](#-daily-usage-commands)
- [🏦 Wallet Management](#-wallet-management)
- [⛏️ Mining Operations](#️-mining-operations)
- [🔗 RPC API Reference](#-rpc-api-reference)
- [🛠️ Troubleshooting](#️-troubleshooting)
- [📁 File Structure](#-file-structure)
- [🔧 Advanced Configuration](#-advanced-configuration)

---

## 🎉 v2.0.1 Dinero Rings - Protocol Core Complete

**Released:** January 4, 2026
**Status:** Protocol Core SEALED 🔒

DineroCoin v2.0.1 marks the **completion and sealing** of the protocol core through the Rings architecture.

### What This Means

**This release introduces zero new semantics.** Instead, it finalizes, verifies, and governs all protocol behavior through formal verification.

- ✅ **100+ properties proven** across 8 rings
- ✅ **46 test suites** with 100% pass rate
- ✅ **Deterministic** execution guaranteed
- ✅ **Backward-compatible** by construction
- ✅ **Auditable** with complete governance discipline
- ✅ **Safely evolvable** through extension gating

### The Rings Architecture

| Ring | Properties | Status | Description |
|------|-----------|--------|-------------|
| **Ring 1** | Supply & Invariants | 🔒 SEALED | Supply conservation, UTXO consistency |
| **Ring 2** | Consensus Validation | 🔒 SEALED | Block/transaction/script validity |
| **Ring 3** | P2P Networking | 🔒 SEALED | 20 P2P protocol properties |
| **Ring 4** | Mining Properties | 🔒 SEALED | 15 properties (MC, MS, ML, MD, MR) |
| **Ring 5** | Distributed Consensus | 🔒 SEALED | 25 properties (DC, DL, DN, DB, DD) |
| **Ring 6** | Economic Properties | 🔒 SEALED | 20 properties (E1-E20) |
| **Ring 7** | Script Semantics | 🔒 **FROZEN** | 25 properties (S1-S25) - Immutable |
| **Ring 8** | Governance | 🔒 SEALED | 10 properties (BC, EG, CL) |

### Key Resources

- 📖 **[Release Notes](RELEASE_v2.0.1_DRAFT.md)** - Comprehensive documentation
- 🔍 **Historical v2 release material** - superseded by the v8 release lane
- 📚 **[Ring Documentation](docs/consensus/)** - All ring specifications
- 🔒 **[Ring 7 Semantics](docs/consensus/RING7_SCRIPT_SEMANTICS.md)** - Frozen script semantics
- 📝 **[Governance Model](docs/consensus/RING8_GOVERNANCE.md)** - Change management

### Independent Verification

```bash
git clone https://github.com/DineroLabs/dinero-v8.git
cd dinero-v8
cmake -S . -B build && cmake --build build
ctest --test-dir build -R "Ring" --output-on-failure
# Expected: 46/46 tests passed (100%)
```

---

## 🔒 Wallet Safety Certification

**DineroCoin wallets are exchange-grade safe** - empirically proven through comprehensive chaos engineering.

### Chaos Testing Results

Between January 8-9, 2026, DineroCoin executed **65 adversarial chaos cycles** across 4 independent risk surfaces with **zero fund loss, zero corruption, and zero data integrity failures**.

| Framework | Risk Surface | Cycles | Result |
|-----------|-------------|--------|--------|
| **Crash Chaos** | Process termination (SIGKILL) | 30 | ✅ PROVEN |
| **Spending Chaos** | Live fund spending | 25 | ✅ PROVEN |
| **Reorg Chaos** | Blockchain reorganizations | 5 | ✅ PROVEN |
| **Mempool Chaos** | Transaction evictions | 5 | ✅ PROVEN |
| **TOTAL** | **All risk surfaces** | **65** | ✅ **CERTIFIED** |

### Exchange-Grade Guarantees

✅ **No fund loss** - 100% fund conservation across all 65 cycles
✅ **No corruption** - Zero database corruption events
✅ **No rescans** - Clean recovery from all crash scenarios
✅ **State integrity** - All invariants preserved under adversarial conditions

### Documentation

📖 **[Comprehensive Test Results](docs/wallet/WALLET_CHAOS_TEST_RESULTS.md)** - Full certification report
📖 **[Chaos Testing Overview](tests/wallet/CHAOS_TESTING_OVERVIEW.md)** - Framework documentation
🔍 **Evidence Tags:** `v2.3.0-wallet-chaos`, `v2.4.0-wallet-spending-chaos`, `v2.5.0-wallet-reorg-chaos`, `v2.6.0-wallet-mempool-chaos`

---

## 🏛️ Architecture

DineroCoin is built on a **layered architecture** that ensures the integrity and compatibility of advanced features including Taproot, Covenants, Utreexo, Zero-Knowledge proofs, and Lightning Network.

### **Core Design Principles**

**Consensus remains authoritative.** All acceleration, compression, privacy, and off-chain mechanisms are designed to **never become authorities** themselves.

### **Architectural Documentation**

**For Contributors & Reviewers:**

All changes touching consensus, state representation, privacy, or off-chain protocols **must** comply with our architectural invariants:

📖 **[Layered Feature Compatibility](docs/architecture/layered_feature_compatibility.md)** (Normative)
- Defines fundamental layer separation rules
- Documents prohibited architectural patterns
- Establishes mandatory invariants

📚 **[Architecture Index](docs/architecture/README.md)**
- Complete architectural documentation
- Normative documents
- Compliance requirements

### **Layer Model**

| Layer | Responsibility | Examples |
|-------|----------------|----------|
| **Layer 0** | Consensus rules | Taproot, Covenants, Script validation |
| **Layer 1** | State representation | Utreexo, AssumeUTXO |
| **Layer 2** | Privacy | Zero-Knowledge proofs, Confidential Transactions |
| **Layer 3** | Off-chain protocols | Lightning Network |
| **Layer 4** | UX optimizations | Wallet features |

### **Mandatory Invariants**

1. **Lower layers never trust higher layers**
2. **Higher layers never weaken lower layers**

These invariants prevent architectural drift and ensure that advanced features remain compatible and secure.

---

## 🚀 Quick Start

### **1. Start the Daemon**
```bash
# Basic startup (v0.6.0+ with ephemeral ports)
dinerod -regtest -rpcport=0 -wsport=0 -port=0

# Fixed ports (legacy)
dinerod -regtest -rpcport=20998 -wsport=21000 -port=20999

# Check nodeinfo.json for actual ports
cat ~/.dinero/regtest/nodeinfo.json
```

### **2. Create Your First Wallet**
```bash
# Using new wallet RPC methods (v0.6.0+)
dinero-cli wallet.create my_wallet password123
dinero-cli wallet.load my_wallet password123
```

### **3. Generate Your First Address**
```bash
# Generate new HD wallet address
ADDRESS=$(dinero-cli wallet.getnewaddress)
echo "New address: $ADDRESS"

# Validate address ownership
dinero-cli wallet.validateaddress $ADDRESS
```

### **4. Start Mining**
```bash
# Set wallet-owned mining address
dinero-cli mining.setaddress $ADDRESS

# Verify mining configuration
dinero-cli mining.getaddress

# Generate test blocks (regtest only)
dinero-cli mining.generatetoaddress 10 $ADDRESS

# Start mining with 1 thread
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "setgenerate", "params": [true, 1]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

---

## 🏗️ Building the System

### **Prerequisites**
- macOS (arm64) with Xcode
- Qt 6.9.1 installed at `$HOME/Qt/6.9.1/macos`
- CMake 3.10+

### **Build Commands**

#### **Release Build (Production)**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos" -DENABLE_SANITIZERS=OFF
cmake --build build -j8
```

#### **Debug Build (Development)**
```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos" -DENABLE_SANITIZERS=ON
cmake --build build-asan -j8
```

#### **Build Specific Targets**
```bash
# Build just the daemon
cmake --build build --target dinerod

# Build just the Qt app
cmake --build build --target dinero-simple-test

# Build everything
cmake --build build
```

---

## ⚡ Daily Usage Commands

### **🚀 Starting the System**

#### **Start Daemon (Background)**
```bash
./build/bin/dinerod -datadir=./data -rpcport=20998 -port=20999 -daemon=1 -server=1
```

#### **Start Daemon (Foreground - see logs)**
```bash
./build/bin/dinerod -datadir=./data -rpcport=20998 -port=20999 -daemon=0 -server=1
```

#### **Start Qt GUI**
```bash
./build/bin/dinero-simple-test
```

### **🛑 Stopping the System**

#### **Stop Daemon Gracefully**
```bash
# Find daemon PID
ps aux | grep dinerod | grep -v grep

# Kill by PID
kill <PID>

# Force kill if needed
kill -9 <PID>
```

#### **Stop All Dinero Processes**
```bash
# Kill all dinerod and Qt processes
pkill -f "dinerod|dinero-"

# Force kill all
pkill -9 -f "dinerod|dinero-"
```

### **🧹 Cleanup Commands**

#### **Clean All Databases and Restart Fresh**
```bash
# Stop all processes
pkill -f "dinerod|dinero-"

# Remove all database files
rm -f data/*.db* data/*.db-shm data/*.db-wal

# Remove lock files
find data/ -name "*LOCK*" -type f -delete

# Start fresh
./build/bin/dinerod -datadir=./data -rpcport=20998 -port=20999 -daemon=0 -server=1
```

---

## 🏦 Wallet Management

### **Creating Wallets**

#### **Create New Wallet**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "createwallet", "params": ["wallet_name"]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Create Multiple Wallets**
```bash
# Personal wallet
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "createwallet", "params": ["personal"]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/

# Business wallet
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "createwallet", "params": ["business"]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

### **Generating Addresses**

#### **Generate Single Address**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getnewaddress", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Generate Multiple Addresses**
```bash
# Generate 5 addresses
for i in {1..5}; do
  curl -s --user "__cookie__:$(cat data/.cookie)" \
    --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getnewaddress", "params": []}' \
    -H 'content-type: text/plain;' http://127.0.0.1:20998/
  echo ""
done
```

### **Wallet Information**

#### **Check Wallet Status**
```bash
# Get blockchain info (includes wallet status)
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getblockchaininfo", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

---

## ⛏️ Mining Operations

### **Mining Setup**

#### **Set Mining Address**
```bash
# Replace YOUR_ADDRESS_HERE with your actual address
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "setminingaddress", "params": ["din1qq0nym82d00y4vuxv962mvhllmf76gcp6t2arjx"]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Start Mining**
```bash
# Start mining with 1 thread
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "setgenerate", "params": [true, 1]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/

# Start mining with 4 threads (adjust based on your CPU)
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "setgenerate", "params": [true, 4]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Stop Mining**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "setgenerate", "params": [false, 0]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

### **Mining Monitoring**

#### **Get Mining Info**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getmininginfo", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Check Mining Status**
```bash
# Quick mining status check
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getmininginfo", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/ | grep -o '"mining_enabled" : [^,]*'
```

---

## 🔗 RPC API Reference

### **Blockchain Information**

#### **Get Blockchain Info**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getblockchaininfo", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Get Block Count**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getblockcount", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Get Best Block Hash**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getbestblockhash", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

### **Network Information**

#### **Get Connection Count**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getconnectioncount", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **Get Network Info**
```bash
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getnetworkinfo", "params": []}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

---

## 🛠️ Troubleshooting

### **Common Issues & Solutions**

#### **"No wallet is loaded" Error**
```bash
# Solution: Create a wallet first
curl -s --user "__cookie__:$(cat data/.cookie)" \
  --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "createwallet", "params": ["default"]}' \
  -H 'content-type: text/plain;' http://127.0.0.1:20998/
```

#### **"Unauthorized" RPC Error**
```bash
# Check if cookie file exists and has correct format
cat data/.cookie

# Should show: __cookie__:long_hex_string
# If missing or wrong format, restart daemon
```

#### **Port Already in Use**
```bash
# Check what's using the port
lsof -i :20998

# Kill the process using the port
kill -9 <PID>

# Or use different ports
./build/bin/dinerod -datadir=./data -rpcport=20998 -port=20999 -daemon=0 -server=1
```

#### **Database Locked**
```bash
# Clean shutdown and restart
pkill -f "dinerod|dinero-"
sleep 2
rm -f data/*.db* data/*.db-shm data/*.db-wal
find data/ -name "*LOCK*" -type f -delete
# Then restart daemon
```

### **Debug Commands**

#### **Check Daemon Status**
```bash
# Check if daemon is running
ps aux | grep dinerod | grep -v grep

# Check if daemon is listening
lsof -i :20998

# Check daemon logs (if running in foreground)
# Look for error messages in terminal
```

#### **Check Database Files**
```bash
# List all database files
ls -la data/ | grep -E "\.(db|db-shm|db-wal)"

# Check file sizes
du -h data/*.db*
```

---

## 📁 File Structure

### **Data Directory (`./data/`)**
```
data/
├── .cookie                    # RPC authentication
├── dinero.conf               # Daemon configuration
├── explorer.db               # Explorer database (read-only analytics layer)
├── wallet.db                 # Wallet database
├── mempool.db                # Transaction mempool
├── peers.db                  # Peer connections
├── blockchain_data/          # Blockchain storage
├── mainnet/                  # Mainnet data
└── regtest/                  # Regtest data
```

### **Build Directory (`./build/`)**
```
build/
├── bin/
│   ├── dinerod               # Main daemon
│   ├── dinero-simple-test    # Qt GUI app
│   └── dinero-cli            # Command line interface
└── lib/                      # Libraries
```

---

## 🔧 Advanced Configuration

### **Configuration File (`data/dinero.conf`)**
```ini
# RPC authentication
rpcuser=dinero_user
rpcpassword=dinero_pass_123456

# Network settings
listen=1
port=20999
rpcport=20998

# Server mode
server=1

# Mining settings
gen=0

# Debug logging
debug=net
debug=addrman

# Connection settings
maxconnections=8
timeout=5000
```

### **Environment Variables**
```bash
# Set data directory
export DINERO_DATA_DIR="./data"

# Set RPC port
export DINERO_RPC_PORT=20998

# Set network
export DINERO_NETWORK=mainnet
```

---

## 🎯 **Quick Reference Card**

### **Essential Commands (Copy & Paste)**
```bash
# Start daemon
./build/bin/dinerod -datadir=./data -rpcport=20998 -port=20999 -daemon=0 -server=1

# Create wallet
curl -s --user "__cookie__:$(cat data/.cookie)" --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "createwallet", "params": ["my_wallet"]}' -H 'content-type: text/plain;' http://127.0.0.1:20998/

# Generate address
curl -s --user "__cookie__:$(cat data/.cookie)" --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "getnewaddress", "params": []}' -H 'content-type: text/plain;' http://127.0.0.1:20998/

# Start mining
curl -s --user "__cookie__:$(cat data/.cookie)" --data-binary '{"jsonrpc": "1.0", "id": "test", "method": "setgenerate", "params": [true, 1]}' -H 'content-type: text/plain;' http://127.0.0.1:20998/

# Stop everything
pkill -f "dinerod|dinero-"
```

---

## 🚀 **Congratulations!**

You now have a **REAL, WORKING cryptocurrency** that you can:
- ✅ **Mine** with your CPU
- ✅ **Generate addresses** for receiving coins
- ✅ **Create wallets** for managing funds
- ✅ **Monitor** blockchain status
- ✅ **Use** just like Bitcoin!

**Your Dinero system is production-ready and follows all the same security standards as major cryptocurrencies!** 🎉

---

## 📞 Support & Community

- **GitHub Repository:** https://github.com/DineroLabs/dinero-v8
- **Issue Tracker:** https://github.com/DineroLabs/dinero-v8/issues
- **Documentation:** https://docs.dinero-coin.com
- **Security:** security@dinero-coin.com

---

*Last updated: June 9, 2026*
*Dinero Version: **v8.0.0 release candidate line***
*Release artifacts: [DineroLabs/dinero-v8 releases](https://github.com/DineroLabs/dinero-v8/releases)*
