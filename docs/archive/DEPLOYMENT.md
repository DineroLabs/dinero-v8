# 🚀 DineroCoin Server Deployment Guide

Instructions for deploying the updated daemon to production servers.

**Date:** October 20, 2025  
**Version:** v0.1.0  
**Package:** `/tmp/dinero-v0.1.0-20251019.tar.gz` (3.6 MB)

---

## 📦 What's Included

The deployment package contains:
- ✅ `dinerod` - Updated mainnet daemon
- ✅ `dinero-miner` - CPU miner
- ✅ `QUICK_START.md` - User guide
- ✅ `FAQ.md` - Common questions
- ✅ `RPC_API.md` - Complete API reference
- ✅ `PRODUCTION_STATUS.md` - Feature documentation

---

## 🌐 Your Servers

**LA Server (Primary)**
- IP: `172.93.160.131`
- Port: `20999` (P2P), `20998` (RPC)
- Current status: 2 peer connections

**VA Server (Virginia)**  
- Port: `20999` (P2P), `20998` (RPC)

---

## 🔧 Deployment Steps

### Step 1: Copy Package to Servers

**From your Mac:**
```bash
# LA Server
scp /tmp/dinero-v0.1.0-20251019.tar.gz \
  root@172.93.160.131:/root/

# VA Server (use your VA server IP)
scp /tmp/dinero-v0.1.0-20251019.tar.gz \
  root@<VA_SERVER_IP>:/root/
```

---

### Step 2: Deploy to LA Server

```bash
# SSH to LA server
ssh root@172.93.160.131

# Stop current daemon gracefully
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"stop","params":[],"id":1}' || pkill dinerod

# Wait for daemon to stop
sleep 5

# Backup current installation
cp ~/dinerod ~/dinerod.backup.$(date +%Y%m%d)
cp ~/dinero-miner ~/dinero-miner.backup.$(date +%Y%m%d)

# Extract new version
cd /root
tar -xzf dinero-v0.1.0-20251019.tar.gz

# Install new binaries
cp dinero-deploy/dinerod ~/dinerod
cp dinero-deploy/dinero-miner ~/dinero-miner
chmod +x ~/dinerod ~/dinero-miner

# Start new daemon
~/dinerod &

# Wait for startup
sleep 8

# Verify running
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"getblockcount","params":[],"id":1}' | jq

# Check peers
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"getpeerinfo","params":[],"id":1}' | jq
```

---

### Step 3: Deploy to VA Server

```bash
# SSH to VA server
ssh root@<VA_SERVER_IP>

# Follow same steps as LA server:
# 1. Stop daemon
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"stop","params":[],"id":1}' || pkill dinerod

sleep 5

# 2. Backup current
cp ~/dinerod ~/dinerod.backup.$(date +%Y%m%d)
cp ~/dinero-miner ~/dinero-miner.backup.$(date +%Y%m%d)

# 3. Extract and install
cd /root
tar -xzf dinero-v0.1.0-20251019.tar.gz
cp dinero-deploy/dinerod ~/dinerod
cp dinero-deploy/dinero-miner ~/dinero-miner
chmod +x ~/dinerod ~/dinero-miner

# 4. Start daemon
~/dinerod &

sleep 8

# 5. Verify
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"getblockcount","params":[],"id":1}' | jq
```

---

## ✅ Post-Deployment Verification

### Check Both Servers

Run on **both LA and VA servers**:

```bash
# 1. Check blockchain height
HEIGHT=$(curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"getblockcount","params":[],"id":1}' | jq -r '.result')
echo "Height: $HEIGHT"

# 2. Check peer connections
PEERS=$(curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"getpeerinfo","params":[],"id":1}' | jq '.result | length')
echo "Peers: $PEERS"

# 3. Check daemon version
~/dinerod --version

# 4. Check if syncing
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"getblockchaininfo","params":[],"id":1}' | jq '.result.verificationprogress'
```

**Expected output:**
```
Height: 296+
Peers: 1-2
Dinero Daemon v0.1.0 (1659101f)
Verification progress: 1.0 (fully synced)
```

---

## 🔍 Verify Connectivity

**From your Mac**, test both servers:

```bash
# Test LA server
curl -X POST http://172.93.160.131:20998 \
  -u "__cookie__:<cookie-from-server>" \
  -H "Content-Type: application/json" \
  -d '{"method":"getblockcount","params":[],"id":1}'

# Test VA server
curl -X POST http://<VA_IP>:20998 \
  -u "__cookie__:<cookie-from-server>" \
  -H "Content-Type: application/json" \
  -d '{"method":"getblockcount","params":[],"id":1}'
```

---

## 🛡️ Security Checklist

After deployment:

- [ ] Both daemons running
- [ ] Peer connections established
- [ ] Blockchain heights match
- [ ] Firewall allows ports 20998-20999
- [ ] Cookie auth working
- [ ] No error messages in logs

**Check logs:**
```bash
# LA Server
tail -100 ~/nohup.out

# VA Server
tail -100 ~/nohup.out
```

---

## 🔄 Rollback Plan

If anything goes wrong:

```bash
# Stop new daemon
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"stop","params":[],"id":1}' || pkill dinerod

sleep 5

# Restore backup
cp ~/dinerod.backup.$(date +%Y%m%d) ~/dinerod
cp ~/dinero-miner.backup.$(date +%Y%m%d) ~/dinero-miner

# Restart old version
~/dinerod &
```

---

## 📊 Monitoring

**Monitor servers for 24 hours:**

```bash
# Every hour, check:
watch -n 3600 'curl -s -X POST http://127.0.0.1:20998 \
  -u "$(cat ~/.dinero/.cookie)" \
  -d "{\"method\":\"getblockchaininfo\",\"params\":[],\"id\":1}" | jq'
```

**What to watch for:**
- ✅ Height increasing
- ✅ Peers staying connected
- ✅ No crash/restart
- ✅ Memory usage stable

---

## 🚨 Troubleshooting

### Daemon Won't Start

```bash
# Check port in use
lsof -i :20998

# Kill old process
pkill -9 dinerod

# Check permissions
chmod +x ~/dinerod

# Try manual start
~/dinerod
```

### No Peer Connections

```bash
# Manually connect to other server
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"addnode","params":["<OTHER_SERVER_IP>:20999", "add"],"id":1}'
```

### Blockchain Not Syncing

```bash
# Check if stuck
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"getblockchaininfo","params":[],"id":1}' | jq

# Restart if stuck
curl -s -X POST http://127.0.0.1:20998 -u "$(cat ~/.dinero/.cookie)" \
  -d '{"method":"stop","params":[],"id":1}'
sleep 5
~/dinerod &
```

---

## 📈 Success Criteria

**Deployment is successful when:**

✅ **LA Server:**
- Daemon running
- Height: 296+
- Peers: 1+
- No errors in logs

✅ **VA Server:**
- Daemon running  
- Height: 296+
- Peers: 1+
- No errors in logs

✅ **Network:**
- Servers connected to each other
- New blocks being mined
- Transactions propagating

---

## 🎉 What's New in This Release

This deployment includes:

1. **Updated Documentation** ✅
   - QUICK_START.md - Complete user guide
   - RPC_API.md - All 58 RPC methods documented
   - FAQ.md - Common questions answered
   - PRODUCTION_STATUS.md - Feature verification

2. **Disabled generatetoaddress** ⚠️
   - Returns helpful error directing to dinero-miner
   - Prevents user confusion from buggy RPC

3. **Cleanup** 🧹
   - Removed 68 backup files
   - Cleaner repository

4. **Same Core Functionality** ✅
   - No consensus changes
   - No protocol changes
   - Fully compatible with existing chain
   - Safe to deploy without blockchain reset

---

## 📞 Support

If you encounter issues:

1. Check logs: `tail -100 ~/nohup.out`
2. Check this guide's troubleshooting section
3. Rollback if needed (see Rollback Plan)

---

**Deployment Package:** `/tmp/dinero-v0.1.0-20251019.tar.gz`  
**Size:** 3.6 MB  
**Prepared:** October 20, 2025  
**Ready to Deploy:** ✅ YES
