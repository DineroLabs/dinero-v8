# Deploy Dinero to Linux Server

**Date**: October 4, 2025  
**Mode**: 🧪 **TESTNET**

---

## 📋 PRE-DEPLOYMENT CHECKLIST

Before deploying:
- [x] Genesis mined
- [x] Code committed to git
- [x] Mac build tested
- [ ] Linux server access confirmed
- [ ] Build dependencies on Linux

---

## 🚀 DEPLOYMENT STEPS

### **Step 1: Update Linux Server Details**

Edit `deploy/deploy_testnet.sh` and update:

```bash
SERVER="your-linux-server.com"  # Your Linux server IP/hostname
SERVER_USER="dinero"            # Your SSH user
SERVER_DIR="/home/dinero/DineroCoin"
```

### **Step 2: Push Code to Git**

```bash
# On Mac
cd /Users/haydarevich/Documents/DineroCoin

# Push to git (if not auto-pushed)
git push origin main
```

### **Step 3: Setup Linux Server**

SSH into your Linux server and setup:

```bash
# SSH to server
ssh your-user@your-linux-server.com

# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libboost-all-dev \
    libsqlite3-dev \
    pkg-config

# Clone repo (if not already)
cd ~
git clone https://github.com/your-username/DineroCoin.git
cd DineroCoin

# Or pull latest if already cloned
git pull origin main

# Build
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Verify build
ls -lh bin/dinerod
```

### **Step 4: Run Testnet on Linux**

```bash
# Create directories
mkdir -p data/testnet
mkdir -p logs

# Start testnet daemon
./bin/dinerod \
  --testnet \
  --datadir=./data/testnet \
  --rpcport=19998 \
  --port=19999 \
  > logs/testnet.log 2>&1 &

# Check it's running
ps aux | grep dinerod

# Monitor logs
tail -f logs/testnet.log

# Test RPC
curl -X POST http://localhost:19998 \
  -H "Content-Type: application/json" \
  -d '{"method":"getblockchaininfo"}'
```

### **Step 5: Test P2P Sync Between Mac and Linux**

**On Mac:**
```bash
# Start testnet
./START_TESTNET.sh

# Connect to Linux peer
./bin/dinero-cli --testnet addnode linux-server-ip:19999 add

# Check peers
./bin/dinero-cli --testnet getpeerinfo
```

**On Linux:**
```bash
# Connect to Mac peer
./bin/dinero-cli --testnet addnode mac-ip:19999 add

# Check peers
./bin/dinero-cli --testnet getpeerinfo

# Check sync
./bin/dinero-cli --testnet getblockchaininfo
```

---

## 🧪 TESTNET TESTING (Both Machines)

Now test everything on both machines:

### **Test 1: Mining**
```bash
# On Mac: Start mining
./build/bin/dinero-miner --testnet --threads 4

# On Linux: Verify block propagates
tail -f logs/testnet.log
# Should see: "New block received from peer"
```

### **Test 2: Wallet & Transactions**
```bash
# On Mac: Create wallet, send tx
./build/bin/dinero-cli --testnet createhdwallet testnet-wallet
./build/bin/dinero-cli --testnet getnewaddress
./build/bin/dinero-cli --testnet sendtoaddress <addr> 10

# On Linux: Verify tx propagates
./bin/dinero-cli --testnet getmempoolinfo
./bin/dinero-cli --testnet listtransactions
```

### **Test 3: Stress Test**
```bash
# Generate many blocks
# Send many transactions
# Monitor memory usage
# Check for crashes
```

---

## 📊 MONITORING

### **Check Status**
```bash
# Blockchain info
curl -X POST http://localhost:19998 -d '{"method":"getblockchaininfo"}'

# Peer info
curl -X POST http://localhost:19998 -d '{"method":"getpeerinfo"}'

# Mining info
curl -X POST http://localhost:19998 -d '{"method":"getmininginfo"}'

# Economics
curl -X POST http://localhost:19998 -d '{"method":"geteconomics"}'
```

### **Monitor Logs**
```bash
# Tail logs
tail -f logs/testnet.log

# Check for errors
grep -i error logs/testnet.log

# Check for warnings
grep -i warning logs/testnet.log
```

### **System Resources**
```bash
# CPU usage
top -p $(pgrep dinerod)

# Memory usage
ps aux | grep dinerod

# Disk usage
du -sh data/testnet/
```

---

## ⚠️ TROUBLESHOOTING

### **Build Fails on Linux**
```bash
# Missing dependencies?
sudo apt-get install -y build-essential cmake git

# CMake version too old?
cmake --version  # Need >= 3.15

# If cmake too old, install newer:
wget https://github.com/Kitware/CMake/releases/download/v3.25.0/cmake-3.25.0-Linux-x86_64.sh
sudo sh cmake-3.25.0-Linux-x86_64.sh --prefix=/usr/local --skip-license
```

### **Daemon Won't Start**
```bash
# Check logs
cat logs/testnet.log

# Check port available
netstat -tuln | grep 19998

# Kill existing process
pkill -f dinerod
```

### **P2P Sync Fails**
```bash
# Check firewall
sudo ufw status
sudo ufw allow 19999

# Check connectivity
telnet linux-server-ip 19999
```

### **Different Genesis Hash**
```bash
# CRITICAL: Both must have same genesis!
# If different, one machine has wrong code

# Check genesis on both:
curl -X POST http://localhost:19998 -d '{"method":"getblockhash","params":[0]}'

# Should be: 10992d751621c536a49998d1d007a97f270a1db3eddb3ef60f2c9946398d927e

# If different, git pull and rebuild
```

---

## ✅ SUCCESS CRITERIA

Testnet deployment successful when:

- [ ] Both Mac and Linux daemons running
- [ ] Genesis hash matches on both
- [ ] P2P sync working
- [ ] Blocks propagate
- [ ] Transactions propagate
- [ ] Mining works on both
- [ ] No crashes for 24+ hours
- [ ] Memory stable

---

## 🎯 NEXT STEPS

After successful testnet:

1. **Test for 1-2 weeks**
   - Mine blocks
   - Send transactions
   - Stress test
   - Monitor stability

2. **Fix any bugs found**
   - Update code
   - Git push
   - Redeploy
   - Test again

3. **When confident → MAINNET**
   - Follow TESTNET_TO_MAINNET.md
   - Launch mainnet on both machines
   - Monitor closely

---

**Status**: 🧪 **TESTNET READY**  
**Next**: Deploy to Linux, test for 1-2 weeks, then launch mainnet! 🚀

