# ❓ DineroCoin Frequently Asked Questions (FAQ)

Common questions and troubleshooting for DineroCoin users.

---

## 📑 Table of Contents

### Getting Started
- [What is DineroCoin?](#what-is-dinerocoin)
- [How do I get started?](#how-do-i-get-started)
- [What are the system requirements?](#what-are-the-system-requirements)

### Wallet & Security
- [How do I backup my wallet?](#how-do-i-backup-my-wallet)
- [I forgot my wallet password, what do I do?](#i-forgot-my-wallet-password)
- [What is a BIP39 mnemonic?](#what-is-a-bip39-mnemonic)
- [Can I use the same wallet on multiple devices?](#can-i-use-the-same-wallet-on-multiple-devices)
- [Is my wallet encrypted?](#is-my-wallet-encrypted)

### Mining
- [How do I start mining?](#how-do-i-start-mining)
- [Why is mining not working?](#why-is-mining-not-working)
- [How many threads should I use?](#how-many-threads-should-i-use)
- [When will I receive my mining rewards?](#when-will-i-receive-my-mining-rewards)
- [What is the block reward?](#what-is-the-block-reward)

### Transactions
- [How do I send coins?](#how-do-i-send-coins)
- [How long do transactions take?](#how-long-do-transactions-take)
- [What are transaction fees?](#what-are-transaction-fees)
- [Why is my transaction unconfirmed?](#why-is-my-transaction-unconfirmed)

### Technical Issues
- [Daemon won't start](#daemon-wont-start)
- [RPC connection errors](#rpc-connection-errors)
- [Port already in use](#port-already-in-use)
- [Peer connection issues](#peer-connection-issues)

### Advanced Topics
- [What is generatetoaddress and why is it disabled?](#what-is-generatetoaddress)
- [How does ASERT difficulty adjustment work?](#how-does-asert-work)
- [What is the economic model?](#what-is-the-economic-model)

---

## Getting Started

### What is DineroCoin?

DineroCoin (DIN) is a cryptocurrency featuring:
- **CPU-friendly mining** - Mine with regular computers (no ASICs needed in Phase 1)
- **HD Wallets** - BIP39 mnemonic backup (12-word seed phrases)
- **SegWit Support** - Native Bech32 addresses (din1q...)
- **ASERT Difficulty** - Smooth difficulty adjustment every block
- **Real PoW** - SHA-256d proof-of-work (like Bitcoin)
- **99M Supply Cap** - Fixed maximum supply

### How do I get started?

Follow the [QUICK_START.md](QUICK_START.md) guide:
1. Download/build the software
2. Start the daemon
3. Create a wallet
4. Backup your 12-word mnemonic
5. Get an address
6. Start mining

**Total time**: ~5 minutes

### What are the system requirements?

**Minimum:**
- CPU: 2+ cores
- RAM: 4GB
- Disk: 10GB free space
- OS: macOS 10.15+, Linux (Ubuntu 20.04+), or Windows (via WSL2)

**Recommended:**
- CPU: 4+ cores
- RAM: 8GB
- Disk: 20GB SSD
- Network: 10+ Mbps stable connection

---

## Wallet & Security

### How do I backup my wallet?

**CRITICAL**: Use the BIP39 mnemonic (12-word seed phrase)

```bash
# Unlock wallet
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"walletpassphrase","params":["YourPassword", 60],"id":1}'

# Get mnemonic
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"backupwallet","params":[],"id":1}' | jq -r '.result.mnemonic'
```

**Write down these 12 words on paper** and store them safely:
- ✅ Fireproof safe
- ✅ Safety deposit box
- ✅ Multiple physical copies in secure locations

**NEVER**:
- ❌ Store on computer/phone
- ❌ Take screenshots
- ❌ Email to yourself
- ❌ Store in cloud (Dropbox, Google Drive, etc.)

### I forgot my wallet password

**If you have your 12-word mnemonic:**
✅ You can restore your wallet:
```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"restorewallet","params":["your 12 words here"],"id":1}'
```

**If you DON'T have your mnemonic:**
❌ **Your coins are permanently lost**. There is no password reset.

This is why backing up your mnemonic is **CRITICAL**.

### What is a BIP39 mnemonic?

BIP39 is a Bitcoin standard for wallet backups using human-readable words.

**Your 12 words = Your entire wallet**:
- All your addresses
- All your private keys
- All your future transactions

**Example mnemonic:**
```
abandon ability able about above absent absorb abstract absurd abuse access accident
```

**These 12 words can restore your wallet on ANY device** running DineroCoin.

### Can I use the same wallet on multiple devices?

✅ **Yes!** Using your BIP39 mnemonic:

**Device 1 (Desktop):**
1. Create wallet → Get 12-word mnemonic
2. Write down the words

**Device 2 (Laptop/Server):**
1. Install DineroCoin
2. Restore wallet with same 12 words
3. **Same addresses, same balance!**

Both devices will have identical wallets.

### Is my wallet encrypted?

**By default: NO** - You must encrypt it:

```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"encryptwallet","params":["StrongPassword123!"],"id":1}'
```

**After encryption:**
- ✅ Private keys encrypted with AES-256-GCM
- ✅ Password hashed with Argon2id
- ✅ Need password to send coins
- ⚠️ **Still need mnemonic backup** (encryption ≠ backup)

---

## Mining

### How do I start mining?

```bash
# Get your address first
ADDRESS=\$(curl -s -u "\$(cat ~/.dinero/.cookie)" \
  -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getnewaddress","params":[],"id":1}' | jq -r '.result')

# Start mining
./build/dinero-miner \
  --rpc http://127.0.0.1:20998 \
  --address \$ADDRESS \
  --threads 4
```

**You'll see:**
```
Hashrate: 125.3 kH/s
[Block 297] ✅ Found valid block!
```

### Why is mining not working?

**Common issues:**

1. **Daemon not running:**
   ```bash
   # Check if daemon is running
   curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
     -d '{"method":"getblockcount","params":[],"id":1}'
   ```

2. **Wrong RPC address:**
   - Mainnet: `http://127.0.0.1:20998`
   - Regtest: Custom port (e.g., `http://127.0.0.1:19998`)

3. **Wallet locked:**
   ```bash
   curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
     -d '{"method":"walletpassphrase","params":["YourPassword", 3600],"id":1}'
   ```

4. **Invalid address:**
   - Mainnet addresses start with `din1q...`
   - Regtest addresses start with `rdin1q...`

### How many threads should I use?

**Laptop (battery):**
```bash
--threads 1  # Conserve power
```

**Desktop (quad-core):**
```bash
--threads 4  # Use all cores
```

**Server (8+ cores):**
```bash
--threads 8  # High performance
```

**Rule of thumb:** Use (CPU cores - 1) to keep system responsive.

### When will I receive my mining rewards?

**Mining reward maturity:**
- Block found → Reward added to wallet
- **Wait 100 confirmations** (~5 hours @ 3min/block)
- After 100 confirmations → Spendable

**Check immature balance:**
```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getbalance","params":[],"id":1}' | jq '.result.immature'
```

### What is the block reward?

**Phase 1 (CPU-Friendly)** - Blocks 2-200,000:
- **100 DIN per block**
- Low difficulty (CPU mining viable)

**Phase 2 (Bitcoin-Level)** - Blocks 200,001+:
- **50 DIN per block** (initial)
- **Halves every 210,000 blocks**
- Higher difficulty (more secure)

**Check current phase:**
```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getphase","params":[],"id":1}' | jq
```

---

## Transactions

### How do I send coins?

```bash
# Unlock wallet
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"walletpassphrase","params":["YourPassword", 60],"id":1}'

# Send 10.5 DIN
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"sendtoaddress","params":["din1q...", 10.5],"id":1}' | jq
```

### How long do transactions take?

**Typical timeline:**
- **0 confirmations** - Immediate (seen in mempool)
- **1 confirmation** - ~3 minutes (included in block)
- **6 confirmations** - ~18 minutes (considered safe)
- **100 confirmations** - ~5 hours (fully mature, required for mining rewards)

### What are transaction fees?

**Default fee:** ~0.00001 DIN (~1,000 una)

**Set custom fee:**
```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"settxfee","params":[0.00002],"id":1}'
```

**Fee guidelines:**
- Higher fee → Faster confirmation
- Minimum: 546 una (dust threshold)
- Recommended: 1,000-10,000 una

### Why is my transaction unconfirmed?

**Possible reasons:**

1. **Fee too low** - Increase fee and resubmit
2. **Network congestion** - Wait for next block
3. **Double-spend attempt** - Transaction will be rejected
4. **Insufficient confirmations** - Wait longer

**Check transaction:**
```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"gettransaction","params":["<txid>"],"id":1}' | jq
```

---

## Technical Issues

### Daemon won't start

**Error: "Port already in use"**
```bash
# Find process using port 20998
lsof -i :20998

# Kill old daemon
pkill dinerod

# Wait 2 seconds, then restart
sleep 2
./build/dinerod
```

**Error: "Database corruption"**
```bash
# Backup wallet first!
cp -r ~/.dinero/wallet ~/dinero_wallet_backup

# Remove corrupted database
rm -rf ~/.dinero/chaindb

# Restart daemon (will resync)
./build/dinerod
```

**Error: "Permission denied"**
```bash
# Make binaries executable
chmod +x ./build/dinerod
chmod +x ./build/dinero-miner
```

### RPC connection errors

**Error: "Connection refused"**
```bash
# Check daemon is running
ps aux | grep dinerod

# Check cookie file exists
cat ~/.dinero/.cookie

# Test RPC manually
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getblockcount","params":[],"id":1}'
```

**Error: "401 Unauthorized"**
- Check cookie file is correct
- Make sure using correct port (20998 for mainnet)

### Port already in use

```bash
# List processes on ports
lsof -i :20998  # RPC
lsof -i :20999  # P2P

# Kill specific process
kill <PID>

# Or kill all dinerod processes
pkill dinerod
```

### Peer connection issues

**No peers connected:**
```bash
# Check peer info
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getpeerinfo","params":[],"id":1}' | jq

# Manually add peer
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"addnode","params":["172.93.160.131:20999", "add"],"id":1}'
```

**Firewall blocking connections:**
```bash
# macOS
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add /path/to/dinerod

# Linux (ufw)
sudo ufw allow 20999/tcp
```

---

## Advanced Topics

### What is generatetoaddress?

`generatetoaddress` is a **testing RPC** for quickly mining blocks in regtest mode.

**Status in DineroCoin:**
- ⚠️ **Disabled** due to persistent bugs
- ✅ **Use `dinero-miner` instead** (works perfectly)

**Why use dinero-miner?**
- ✅ Production-ready (proven with 1,300+ DIN on mainnet)
- ✅ Multi-threaded
- ✅ Real hashrate reporting
- ✅ Works on both mainnet and regtest

**Example:**
```bash
./build/dinero-miner --rpc http://127.0.0.1:20998 \
  --address rdin1q... --threads 1
```

### How does ASERT work?

**ASERT (aSERT3-2d)** is DineroCoin's difficulty adjustment algorithm.

**Key features:**
- **Adjusts every block** (not every 2016 like Bitcoin)
- **Target block time:** 180 seconds (3 minutes)
- **Responsive:** Reacts quickly to hashrate changes
- **Smooth:** No difficulty jumps

**Phase-dependent parameters:**
- **Phase 1 (CPU):** Gentle caps, 48-hour half-life
- **Phase 2 (Bitcoin-level):** Stricter caps, tighter control

**Check difficulty:**
```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getdifficulty","params":[],"id":1}' | jq
```

### What is the economic model?

**Total Supply:** 99,000,000 DIN

**Distribution:**

1. **Genesis Block** (Block 0):
   - 100,000 DIN (burned, unspendable)
   
2. **Premine** (Block 1):
   - 2,000,000 DIN (developer fund)

3. **Phase 1: CPU-Friendly** (Blocks 2-200,000):
   - 100 DIN per block
   - ~20,000,000 DIN total
   - Duration: ~13.7 months

4. **Phase 2: Halving** (Blocks 200,001+):
   - 50 DIN → 25 DIN → 12.5 DIN → ...
   - Halves every 210,000 blocks (~14.3 months)
   - ~77,000,000 DIN total

**Check current supply:**
```bash
curl -u "\$(cat ~/.dinero/.cookie)" -X POST http://127.0.0.1:20998/ \
  -d '{"method":"getsupply","params":[],"id":1}' | jq
```

---

## Still Need Help?

**Resources:**
- 📖 [QUICK_START.md](QUICK_START.md) - Getting started guide
- 📡 [RPC_API.md](RPC_API.md) - Complete RPC reference
- 🛡️ [PRODUCTION_STATUS.md](PRODUCTION_STATUS.md) - Security & status

**Community:**
- GitHub Issues: https://github.com/dinerocoin/dinero/issues
- Discord: https://discord.gg/dinerocoin
- Forum: https://forum.dinero-coin.com

---

**Last Updated:** October 20, 2025
**Version:** v0.1.0
