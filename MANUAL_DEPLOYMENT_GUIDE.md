# 🚨 Hard Fork Deployment Guide - Manual Instructions

**Date:** October 7, 2025  
**Status:** Ready for deployment when servers are online

---

## 📦 **Deployment Package Created:**

Location: `/tmp/dinero-deploy/`

Contains:
- `dinerod` - New daemon with genesis/premine (coin type 1447)
- `dinero-miner` - Updated miner

---

## 🚀 **Manual Deployment Steps:**

### **For Each Server (DineroCA, DineroVA, DineroLA):**

#### **1. Upload Binaries:**
```bash
# From your local machine:
scp -i ~/.ssh/[KEY] /tmp/dinero-deploy/* root@[SERVER_IP]:/root/dinero/
```

#### **2. SSH to Server:**
```bash
ssh -i ~/.ssh/[KEY] root@[SERVER_IP]
```

#### **3. Stop Old Daemon:**
```bash
cd /root/dinero
pkill dinerod
sleep 2
```

#### **4. Backup Old Blockchain (Optional):**
```bash
mv data data.old.backup
```

#### **5. Set Permissions:**
```bash
chmod +x dinerod dinero-miner
```

#### **6. Start New Daemon:**
```bash
nohup ./dinerod -datadir=./data > daemon.log 2>&1 &
```

#### **7. Verify Genesis:**
```bash
# Wait 10 seconds for startup
sleep 10

# Get cookie
COOKIE=$(cat data/.cookie | cut -d':' -f2)

# Check genesis block
curl -s --user "__cookie__:$COOKIE" \
     --data-binary '{"method":"getblock","params":[0]}' \
     http://127.0.0.1:20998/ | grep hash

# Should show: 00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e

# Check premine block
curl -s --user "__cookie__:$COOKIE" \
     --data-binary '{"method":"getblock","params":[1]}' \
     http://127.0.0.1:20998/ | grep hash

# Should show: 0000003b844fdbbe07f22e208007d55227c81795e0672e21db8c5070ecfd856b
```

---

## 🔍 **Verification Checklist:**

### **Genesis Block (Height 0):**
- [ ] Hash: `00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e`
- [ ] Nonce: `32701775`
- [ ] Difficulty: `0x1d3fffff` (490733567)
- [ ] Reward: 99 DIN (burned)

### **Premine Block (Height 1):**
- [ ] Hash: `0000003b844fdbbe07f22e208007d55227c81795e0672e21db8c5070ecfd856b`
- [ ] Nonce: `14670052`
- [ ] Difficulty: `0x1d3fffff` (490733567)
- [ ] Reward: 1M DIN
- [ ] Address: `din1qfmy8slqyt9zasexg7e849x9q08hr7da4d4hjmc`

### **Blockchain State:**
- [ ] Height: 1
- [ ] Phase: `cpu_friendly`
- [ ] Money Supply: 1000000 DIN
- [ ] Daemon running without errors

---

## 📋 **Server-Specific Commands:**

### **DineroCA (96.9.226.98):**
```bash
scp -i ~/.ssh/id_rsa /tmp/dinero-deploy/* root@96.9.226.98:/root/dinero/
ssh -i ~/.ssh/id_rsa root@96.9.226.98
# Follow steps 3-7 above
```

### **DineroVA (96.9.226.99):**
```bash
scp -i ~/.ssh/id_rsa /tmp/dinero-deploy/* root@96.9.226.99:/root/dinero/
ssh -i ~/.ssh/id_rsa root@96.9.226.99
# Follow steps 3-7 above
```

### **DineroLA (96.9.226.100):**
```bash
scp -i ~/.ssh/dinero-la-key /tmp/dinero-deploy/* root@96.9.226.100:/root/dinero/
ssh -i ~/.ssh/dinero-la-key root@96.9.226.100
# Follow steps 3-7 above
```

---

## ⚠️ **Important Notes:**

1. **This is a HARD FORK** - old blockchain data is incompatible
2. **All nodes must update** - old nodes will reject new blocks
3. **Backup old data** before wiping (optional but recommended)
4. **Verify genesis hash** after deployment to ensure correctness
5. **Check daemon.log** if any issues occur

---

## 🔐 **Premine Security:**

**Seed (KEEP SAFE!):**
```
iron expect scout august display north season extra dad material track payment
```

**Address:**
```
din1qfmy8slqyt9zasexg7e849x9q08hr7da4d4hjmc
```

**Derivation Path:**
```
m/84'/1447'/0'/0/0
```

---

## 📊 **What Changed:**

| Item | Old Value | New Value |
|------|-----------|-----------|
| Coin Type | 1 (testnet) | 1447 (Dinero) |
| Premine Address | din1qwaef... | din1qfmy8slq... |
| Genesis Hash | b464da19... | 00000008c7c1... |
| Premine Hash | 1857d4a7... | 0000003b844f... |
| Difficulty | 0x2100ffff | 0x1d3fffff |
| Phase 1 Duration | ~60 days | ~300 days |

---

## 🎯 **Success Criteria:**

✅ All servers show:
- Genesis hash: `00000008c7c1809e7d20d4d26f56d25fccf288ac6862ca5269009cfd6921437e`
- Premine hash: `0000003b844fdbbe07f22e208007d55227c81795e0672e21db8c5070ecfd856b`
- Height: 1
- Phase: `cpu_friendly`
- No errors in daemon.log

---

## 🆘 **Troubleshooting:**

### **Daemon won't start:**
```bash
cat daemon.log | tail -50
```

### **Wrong genesis hash:**
```bash
# Wipe data and restart
rm -rf data
nohup ./dinerod -datadir=./data > daemon.log 2>&1 &
```

### **RPC not responding:**
```bash
# Check if daemon is running
ps aux | grep dinerod

# Check RPC port
netstat -an | grep 20998
```

---

**Ready to deploy when servers are online!** 🚀
