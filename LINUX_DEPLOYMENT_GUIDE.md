# Linux Server Deployment Guide - Genesis Fix
**November 7, 2025**

## 🎯 What This Deploys

This deploys all your latest Mac code (including critical fixes) to Linux servers **without GitHub**:

✅ **Genesis hash fix** (173fe6da... hardcoded)  
✅ **Premine integration** (2,627,900 DIN)  
✅ **std::bad_alloc fix** (hardcoded mainnet values)  
✅ **Network mismatch fix** (mainnet everywhere)  
✅ **ASERT DAA from block 2** (no Bitcoin DAA)  
✅ **Coin type 1447** (correct Dinero SLIP-0044)  
✅ **Experimental features disabled** (production-ready)

---

## 🚀 How to Deploy

### Step 1: Run the deployment script

```bash
cd /Users/haydarevich/Documents/DineroCoin
./deploy_genesis_fix_to_linux.sh
```

### Step 2: Choose which servers to deploy to

```
Which servers do you want to deploy to?
  1) California only (172.93.160.131)
  2) Virginia only (173.249.195.59)
  3) Both servers
  
Enter choice (1-3):
```

**Recommendation**: Start with one server to test, then deploy to both.

---

## 📋 What the Script Does (Automatically)

For each selected server:

1. **✅ Checks server connectivity** - Verifies SSH access
2. **✅ Checks disk space** - Shows available space
3. **✅ Stops old daemon** - Kills running dinerod process
4. **✅ Syncs code via rsync** - Copies all your Mac code to server (no GitHub!)
5. **✅ Builds on Linux** - Compiles dinerod natively on server
6. **✅ Starts daemon** - Launches with mainnet config
7. **✅ Verifies deployment** - Checks for errors in logs

**Time**: ~5-10 minutes per server (depending on build speed)

---

## 🔍 What Gets Synced

Your **entire local repository** from Mac → Linux:

```
/Users/haydarevich/Documents/DineroCoin/ → /root/DineroCoin/
```

**Excluded** (not synced):
- `build/` directories (rebuilt on server)
- `.git/` (not needed for compilation)
- `data/` and `.dinero/` (server's own blockchain data)
- `*.o`, `*.a` (object files)

**Included** (synced):
- All source code (`src/`, `include/`)
- CMakeLists.txt
- All your hardcoded genesis/premine fixes
- Dependencies (RocksDB, etc.)

---

## 🛠️ Manual Commands (If Needed)

### Check if daemon is running on server

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 "pgrep dinerod"
```

### View live logs on server

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 \
  "tail -f /root/.dinero/mainnet/dinerod.log"
```

### Test RPC on server

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 << 'REMOTE'
  curl --user $(cat /root/.dinero/mainnet/.cookie) \
    --data-binary '{"jsonrpc":"1.0","method":"getblockchaininfo","params":[]}' \
    http://localhost:20998
REMOTE
```

### Stop daemon manually

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 "pkill -9 dinerod"
```

### Restart daemon manually

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 << 'REMOTE'
  cd /root/DineroCoin
  nohup ./build/dinerod \
    -datadir=/root/.dinero \
    -rpcport=20998 \
    -port=20999 \
    -rpcbind=0.0.0.0 \
    -rpcallowip=0.0.0.0/0 \
    > /root/.dinero/mainnet/dinerod.log 2>&1 &
REMOTE
```

---

## ✅ Success Indicators

After deployment, the script will check for:

1. **✅ Process running** - `pgrep dinerod` returns PID
2. **✅ No std::bad_alloc** - Memory allocation fixed
3. **✅ No network mismatch** - Running mainnet
4. **✅ No genesis hash mismatch** - Correct genesis (173fe6da...)
5. **✅ Genesis initialized** - "Genesis stored" in log
6. **✅ Premine initialized** - "Premine stored" in log

---

## 🔥 What If Something Goes Wrong?

### Problem: Can't connect to server

**Solution**: Check SSH key and server IP:

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 "echo OK"
```

### Problem: Build fails on server

**Solution**: SSH in and check dependencies:

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131
cd /root/DineroCoin/build
cat CMakeError.log
```

Common fixes:
- `apt update && apt install -y build-essential cmake libssl-dev`

### Problem: Daemon crashes on startup

**Solution**: Check the log for errors:

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 \
  "tail -50 /root/.dinero/mainnet/dinerod.log"
```

Look for:
- Genesis hash mismatch → Fixed in this deployment
- std::bad_alloc → Fixed in this deployment
- Missing library → Install dependency

### Problem: Need to wipe blockchain and restart

**Solution**:

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 << 'REMOTE'
  pkill -9 dinerod
  rm -rf /root/.dinero/mainnet/*
  cd /root/DineroCoin
  ./build/dinerod -datadir=/root/.dinero -rpcport=20998 -port=20999
REMOTE
```

---

## 📊 Monitoring After Deployment

### Check block height

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 << 'REMOTE'
  curl -s --user $(cat /root/.dinero/mainnet/.cookie) \
    --data-binary '{"jsonrpc":"1.0","method":"getblockcount","params":[]}' \
    http://localhost:20998 | jq .
REMOTE
```

### Check peer connections

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 << 'REMOTE'
  curl -s --user $(cat /root/.dinero/mainnet/.cookie) \
    --data-binary '{"jsonrpc":"1.0","method":"getpeerinfo","params":[]}' \
    http://localhost:20998 | jq .
REMOTE
```

### Check genesis block

```bash
ssh -i ~/.ssh/dinero_deployment_2025 root@172.93.160.131 << 'REMOTE'
  curl -s --user $(cat /root/.dinero/mainnet/.cookie) \
    --data-binary '{"jsonrpc":"1.0","method":"getblockhash","params":[0]}' \
    http://localhost:20998 | jq -r .result
REMOTE
```

**Expected**: `173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33`

---

## 🎉 Summary

**No GitHub Required!** This script:
- Uses `rsync` over SSH to copy your Mac code directly to servers
- Builds natively on Linux (avoids cross-compilation issues)
- Deploys all your latest fixes in one command
- Verifies the deployment succeeded

**Ready to deploy?**

```bash
./deploy_genesis_fix_to_linux.sh
```
