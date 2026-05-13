# Dinero Cryptocurrency - Quick Start Guide

## ✅ What's Working Now

- **Daemon**: Clean genesis block with 1M DIN premine
- **P2P Networking**: Mac node peers with Ubuntu server (96.9.226.98:20999)
- **GUI**: Widgets-based interface (stable, no QML crashes)
- **Mining**: External CPU miner with SIMD optimization
- **RPC**: Cookie authentication working
- **Consensus**: Phase 1 (CPU-friendly) and Phase 2 (halving) ready

---

## 🚀 Mac Setup (Development/Mining Node)

### 1. Start the Node

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Option A: Use the start script
./start-node.sh

# Option B: Manual start with nohup
nohup ./build-clean/dinerod \
  -datadir=./data-main \
  -addnode=96.9.226.98:20999 \
  -listen=1 -rpcbind=127.0.0.1 \
  >> ./data-main/console.log 2>&1 &
```

### 2. Launch the GUI

```bash
./build-gui/dinero-qt -datadir=./data-main &
```

The GUI will:
- Load cookie from `./data-main/.cookie`
- Connect to `http://127.0.0.1:20998/`
- Show blockchain status, wallet, and mining info

### 3. Start Mining

```bash
# Option A: Use the mining script
./start-mining.sh

# Option B: Manual mining
./build-clean/dinero-miner \
  --rpc http://127.0.0.1:20998/ \
  --datadir ./data-main \
  --address din1q6xhym4p08vhtj7er2r99lhzg58t00fd5uxcpl6 \
  --threads 8
```

---

## 🖥️ Ubuntu Server (96.9.226.98)

### Current Status
- **RPC**: `127.0.0.1:20998` (localhost only, secure)
- **P2P**: `*:20999` (publicly listening)
- **Firewall**: Port 20999 open for peer connections
- **Genesis**: Same hash as Mac (`f3f22c75...`)

### Server Commands

```bash
# Check server status
COOKIE=$(cat ~/.dinero/.cookie)
curl -s --user "$COOKIE" http://127.0.0.1:20998/ \
  -d '{"jsonrpc":"2.0","id":1,"method":"getblockchaininfo","params":[]}'

# Check peers
curl -s --user "$COOKIE" http://127.0.0.1:20998/ \
  -d '{"jsonrpc":"2.0","id":1,"method":"getpeerinfo","params":[]}' | jq
```

---

## 🔍 Verification Commands

### Check Node is Running
```bash
# Mac
ps aux | grep "dinerod.*data-main"
lsof -nP -iTCP:20998 -sTCP:LISTEN

# Server
ps aux | grep dinerod
lsof -nP -iTCP:20998 -sTCP:LISTEN
```

### RPC Health Check
```bash
COOKIE=$(tr -d '\r\n' < ./data-main/.cookie)

# Block count
curl -s --user "$COOKIE" http://127.0.0.1:20998/ \
  -d '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' 

# Supply
curl -s --user "$COOKIE" http://127.0.0.1:20998/ \
  -d '{"jsonrpc":"2.0","id":1,"method":"getsupply","params":[]}' | jq

# Peer info
curl -s --user "$COOKIE" http://127.0.0.1:20998/ \
  -d '{"jsonrpc":"2.0","id":1,"method":"getpeerinfo","params":[]}' | jq
```

### Mining Stats
```bash
# Get mining address balance
curl -s --user "$COOKIE" http://127.0.0.1:20998/ \
  -d '{"jsonrpc":"2.0","id":1,"method":"getbalance","params":[]}' 

# List wallet addresses
curl -s --user "$COOKIE" http://127.0.0.1:20998/ \
  -d '{"jsonrpc":"2.0","id":1,"method":"listaddresses","params":[]}' | jq
```

---

## 🛠️ Troubleshooting

### GUI Shows "Host requires authentication"
**Cause**: GUI is loading wrong cookie file  
**Fix**: Always launch with `-datadir=./data-main`:
```bash
./build-gui/dinero-qt -datadir=./data-main
```

### Daemon Keeps Shutting Down
**Cause**: Using `timeout`, pipes, or scripts with exit traps  
**Fix**: Use `nohup` without wrappers:
```bash
nohup ./build-clean/dinerod -datadir=./data-main ... >> ./data-main/console.log 2>&1 &
```

### Port 20998 Already in Use
**Cause**: Multiple dinerod instances running  
**Fix**: Kill all and start clean:
```bash
pkill -f dinerod
sleep 2
./start-node.sh
```

### Nodes Not Peering
**Check firewall** on Ubuntu server:
```bash
# Server
sudo ufw allow 20999/tcp
sudo ufw status

# Mac
curl -v telnet://96.9.226.98:20999
```

---

## 📊 Economics Summary

### Genesis Block
- **Hash**: `f3f22c7592812a24930ff2063a7cbae1e3342e197904ba7ef14a4aeae633112c`
- **Burned**: 100,000 DIN (provably unspendable)
- **Premine**: 1,000,000 DIN → `din1q0gqj8ush5026e5q97jkw2mhxg3sngjjlxcpjzn`
- **Maturity**: Spendable after block 100

### Phase 1: CPU-Friendly Mining
- **Blocks**: 0 - 180,000
- **Reward**: 100 DIN per block
- **Difficulty**: 0x2100ffff (easy)
- **Total**: 18,000,000 DIN

### Phase 2: Bitcoin-Style Halving
- **Starting Reward**: 50 DIN
- **Halving Interval**: Every 800,000 blocks (~4 years)
- **Total**: 80,000,000 DIN
- **Max Supply**: 99,000,000 DIN

---

## 🔐 Security Notes

1. **RPC is localhost-only** (`127.0.0.1:20998`) - never exposed publicly
2. **Cookie authentication** - no hardcoded passwords
3. **P2P port 20999** - only port open to internet (on server)
4. **SSH tunnel** for remote RPC (if needed):
   ```bash
   ssh -N -L 19098:127.0.0.1:20998 root@96.9.226.98
   # Then use http://127.0.0.1:19098/ locally
   ```

---

## 📝 Next Steps

### For Production Use:
1. ✅ **Mac mines locally** (current setup)
2. ✅ **Server acts as P2P seed** (always-on bootstrap)
3. ⏳ **Mine 100+ blocks** to mature the premine
4. ⏳ **Test spending** from premined address
5. ⏳ **Windows build** (same codebase, different platform)

### Windows Build (Next):
```powershell
# Install Qt 6.9.1 for Windows
# Install Visual Studio 2022
# Install CMake

cd C:\DineroCoin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.9.1\msvc2022_64"
cmake --build build --config Release

# Package with windeployqt
cd build\bin
windeployqt dinero-qt.exe
```

---

## 🎉 Success Metrics

- ✅ Clean genesis block mined
- ✅ Mac node synced with server
- ✅ GUI launches without crashes
- ✅ Cookie authentication working
- ✅ CPU miner ready (SIMD optimized)
- ⏳ Mining blocks and earning rewards
- ⏳ Windows build completed

**You're ready to mine Dinero! 🚀⛏️💎**

