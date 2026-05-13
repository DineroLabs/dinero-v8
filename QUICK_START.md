# 🚀 DineroCoin Quick Start Guide

Get up and running with DineroCoin in under 5 minutes!

---

## 📋 Table of Contents

1. [System Requirements](#system-requirements)
2. [Installation](#installation)
3. [Starting the Daemon](#starting-the-daemon)
4. [Creating Your First Wallet](#creating-your-first-wallet)
5. [Backing Up Your Wallet](#backing-up-your-wallet)
6. [Getting Your Address](#getting-your-address)
7. [Mining DineroCoin](#mining-dinerocoin)
8. [Checking Your Balance](#checking-your-balance)
9. [Sending Coins](#sending-coins)
10. [Stopping the Daemon](#stopping-the-daemon)

---

## System Requirements

**Operating Systems:**
- macOS 10.15+ (Catalina or newer)
- Linux (Ubuntu 20.04+, Debian 11+, or equivalent)
- Windows (via WSL2)

**Hardware:**
- CPU: 2+ cores
- RAM: 4GB minimum, 8GB recommended
- Disk: 10GB free space
- Network: Stable internet connection

**Software:**
- curl (for RPC commands)
- jq (optional, for formatting JSON responses)

---

## Installation

### Option 1: Pre-built Binary (Recommended)

```bash
# Download DineroCoin
cd ~/Downloads
# Extract to your preferred location
tar -xzf dinero-v0.1.0.tar.gz
cd dinero

# Make binaries executable
chmod +x build/dinerod
chmod +x build/dinero-miner
chmod +x build/dinero-cli
```

### Option 2: Build from Source

```bash
# Install dependencies
# macOS:
brew install cmake openssl rocksdb sqlite3

# Ubuntu/Debian:
sudo apt-get update
sudo apt-get install build-essential cmake libssl-dev librocksdb-dev libsqlite3-dev

# Clone and build
git clone https://github.com/dinerocoin/dinero.git
cd dinero
mkdir build && cd build
cmake ..
make -j\$(nproc)
```

---

## Starting the Daemon

### Mainnet (Production Network)

```bash
./build/dinerod
```

The daemon will:
- Create data directory at `~/.dinero/`
- Generate authentication cookie at `~/.dinero/.cookie`
- Start RPC server on port `20998`
- Start P2P network on port `20999`
- Begin syncing with the network

**You'll see output like:**
```
🚀 Running in MAINNET mode
✅ Acquired instance lock
RPC server listening on 127.0.0.1:20998
P2P server listening on 0.0.0.0:20999
Connected to peer: 172.93.160.131:20999
```

### Regtest (Testing Network)

For testing and development:
```bash
./build/dinerod --regtest --datadir=./test_data
```

---

## Creating Your First Wallet

DineroCoin uses **HD (Hierarchical Deterministic) wallets** with BIP39 mnemonic backup.

### Step 1: Create HD Wallet

```bash
# Create new wallet
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"createhdwallet","params":[],"id":1}' | jq
```

**Response:**
```json
{
  "result": {
    "success": true,
    "wallet_id": "default",
    "message": "HD wallet created successfully"
  }
}
```

### Step 2: Encrypt Your Wallet (CRITICAL!)

```bash
# Set a strong password
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"encryptwallet","params":["YourStrongPassword123!"],"id":1}' | jq
```

**⚠️ IMPORTANT**: Choose a strong password and **never forget it**! Lost passwords = lost coins.

---

## Backing Up Your Wallet

### Get Your BIP39 Mnemonic (Most Important!)

```bash
# Unlock wallet first
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"walletpassphrase","params":["YourStrongPassword123!", 60],"id":1}'

# Get mnemonic backup
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"backupwallet","params":[],"id":1}' | jq
```

**Response:**
```json
{
  "result": {
    "mnemonic": "abandon ability able about above absent absorb abstract absurd abuse access accident",
    "warning": "Write down these 12 words and store them safely!"
  }
}
```

**🔥 CRITICAL**:
- **Write down these 12 words** on paper
- Store in a safe place (fireproof safe, safety deposit box)
- **Never** store digitally (no screenshots, no cloud storage)
- These words are your **only** backup if you lose your computer

---

## Getting Your Address

```bash
# Generate new receiving address
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"getnewaddress","params":[],"id":1}' | jq -r '.result'
```

**Example address:**
```
din1q5u3xjgjn8qdehahyecwdrusg2qprvng46sjkkf
```

**Address format:**
- Mainnet: Starts with `din1q...`
- Regtest: Starts with `rdin1q...`
- Bech32 native SegWit (same as Bitcoin)

---

## Mining DineroCoin

### Using the Built-in Miner

```bash
# Start mining with 4 threads
./build/dinero-miner \
  --rpc http://127.0.0.1:20998 \
  --address din1q5u3xjgjn8qdehahyecwdrusg2qprvng46sjkkf \
  --threads 4
```

**You'll see output like:**
```
Mining to address: din1q5u3xjgjn8qdehahyecwdrusg2qprvng46sjkkf
Threads: 4
Target difficulty: 0x21ffffff (CPU-friendly)
Hashrate: 125.3 kH/s
[Block 297] ✅ Found valid block! Hash: 0000001a2b3c...
```

### Mining Tips

**Adjust threads based on your CPU:**
```bash
# Low power (laptop):
--threads 1

# Medium (desktop):
--threads 4

# High power (server):
--threads 8
```

**Check mining progress:**
```bash
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"getmininginfo","params":[],"id":1}' | jq
```

---

## Checking Your Balance

```bash
# Get total balance
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"getbalance","params":[],"id":1}' | jq
```

**Response:**
```json
{
  "result": {
    "total": "1300.00000000",
    "confirmed": "1200.00000000",
    "unconfirmed": "100.00000000",
    "immature": "0.00000000"
  }
}
```

**Balance types:**
- **confirmed**: Spendable now (100+ confirmations)
- **unconfirmed**: Waiting for confirmations
- **immature**: Mining rewards (need 100 confirmations)
- **total**: Sum of all above

---

## Sending Coins

### Step 1: Unlock Wallet

```bash
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"walletpassphrase","params":["YourStrongPassword123!", 60],"id":1}'
```

### Step 2: Send Transaction

```bash
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{
    "method":"sendtoaddress",
    "params":[
      "din1q7d3ztduxteydgqhrshrksnflznuvc9dtpwx054",
      10.5
    ],
    "id":1
  }' | jq
```

**Parameters:**
- First: Recipient address
- Second: Amount in DIN

**Response:**
```json
{
  "result": {
    "txid": "a1b2c3d4e5f6...",
    "fee": "0.00001000"
  }
}
```

### Step 3: Verify Transaction

```bash
# Check transaction status
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"gettransaction","params":["a1b2c3d4e5f6..."],"id":1}' | jq
```

---

## Stopping the Daemon

### Graceful Shutdown

```bash
curl -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  -d '{"method":"stop","params":[],"id":1}'
```

The daemon will:
- ✅ Save all data to disk
- ✅ Close database connections
- ✅ Disconnect from peers gracefully
- ✅ Release lock file

**Wait for confirmation:**
```
Shutting down gracefully...
Database flushed
All connections closed
Goodbye!
```

---

## Quick Reference Card

### Essential Commands

```bash
# Start daemon
./build/dinerod

# Create wallet
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"createhdwallet","params":[],"id":1}'

# Get address
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getnewaddress","params":[],"id":1}' | jq -r '.result'

# Start mining
./build/dinero-miner --rpc http://127.0.0.1:20998 \
  --address <your-address> --threads 4

# Check balance
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getbalance","params":[],"id":1}' | jq

# Send coins
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"sendtoaddress","params":["<address>", 10.5],"id":1}'

# Stop daemon
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"stop","params":[],"id":1}'
```

---

## Next Steps

- 📖 Read [RPC_API.md](RPC_API.md) for complete API reference
- ❓ Check [FAQ.md](FAQ.md) for common questions
- 🛡️ Review [PRODUCTION_STATUS.md](PRODUCTION_STATUS.md) for security details
- 🌐 Join the community at [dinero-coin.com](https://dinero-coin.com)

---

## Need Help?

**Common Issues:**
- Daemon won't start → Check [FAQ.md](FAQ.md#daemon-wont-start)
- Forgot password → Check [FAQ.md](FAQ.md#forgot-password)
- Mining not working → Check [FAQ.md](FAQ.md#mining-issues)

**Get Support:**
- GitHub Issues: https://github.com/dinerocoin/dinero/issues
- Community Forum: https://forum.dinero-coin.com
- Discord: https://discord.gg/dinerocoin

---

**Happy mining! 🎉**

**Remember**:
- ✅ Back up your mnemonic (12 words)
- ✅ Use a strong password
- ✅ Never share your private keys
- ✅ Start with small transactions to test
