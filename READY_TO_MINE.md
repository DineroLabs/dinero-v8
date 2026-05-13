# 🎯 READY TO MINE - LAUNCH GUIDE

**Date:** October 2, 2025  
**Status:** ✅ ALL SYSTEMS GO!  
**Action:** Ready to mine first blocks

---

## 🚀 **QUICK START - MINE NOW!**

### **Step 1: Open SSH Tunnel (Terminal 1)**
```bash
# Forward server RPC to your Mac
ssh -i /tmp/server_key -N -L 19098:127.0.0.1:20998 root@96.9.226.98

# Leave this running in background
```

### **Step 2: Generate Mining Address (Terminal 2)**
```bash
cd /Users/haydarevich/Documents/DineroCoin

# Get a fresh address for mining rewards
ADDR=$(curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getnewaddress","params":[]}' \
  http://127.0.0.1:19098/ | jq -r .result)

echo "Mining to address: $ADDR"
```

### **Step 3: Start Mining!**
```bash
# Get CPU core count
THREADS=$(sysctl -n hw.ncpu)
echo "Using $THREADS threads"

# Start the miner
./build-clean/dinero-miner \
  --rpc http://127.0.0.1:19098/ \
  --address "$ADDR" \
  --threads $THREADS

# Watch it mine blocks! ⛏️
```

---

## 📊 **WHAT TO EXPECT**

### **Mining Speed:**
- **Difficulty:** 0x2100ffff (EASY - CPU friendly!)
- **Your Mac (M-series):** ~5-30 seconds per block
- **Phase 1 Reward:** 100 DIN per block
- **First 100 blocks:** Premine matures!

### **Block Rewards:**
```
Blocks 1-180,000: 100 DIN each
Blocks 180,001+:  50 DIN (then halving)
```

### **Premine Maturity:**
```
Current: 1M DIN locked (coinbase maturity)
After Block 100: 1M DIN spendable! 💰
```

---

## 🔍 **MONITORING (Terminal 3)**

### **Watch Blockchain Grow:**
```bash
# Loop to show progress
watch -n 5 'curl -s -H "Content-Type: application/json" \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockchaininfo\",\"params\":[]}" \
  http://127.0.0.1:19098/ | jq ".result | {blocks, moneysupply, phase}"'
```

### **Check Server Logs:**
```bash
ssh -i /tmp/server_key root@96.9.226.98 "journalctl -u dinerod -f"
```

### **Verify Balance:**
```bash
# After mining some blocks
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getbalance","params":[]}' \
  http://127.0.0.1:19098/ | jq
```

---

## 🎯 **MILESTONES**

### **Block 1:**
- ✅ First mined block
- ✅ 100 DIN reward
- ✅ Chain has begun!

### **Block 10:**
- ✅ 1,000 DIN mined
- ✅ Blockchain stable
- ✅ P2P tested

### **Block 100:**
- ✅ 10,000 DIN mined  
- ✅ **1M DIN PREMINE UNLOCKED!** 💎
- ✅ Can spend premine

### **Block 180,000:**
- ✅ Phase 1 complete (18M DIN)
- ✅ Difficulty increases
- ✅ Phase 2 begins (halving)

---

## ⚠️ **IMPORTANT NOTES**

### **1. Keep Server Key-less:**
- Server is for P2P/RPC only
- All mining rewards go to Mac wallet
- Never upload premine wallet to server

### **2. Backup After Mining:**
- Mac wallet has mining rewards
- Backup after significant mining
- Keep premine wallet separate

### **3. Premine Spending:**
- Can't spend until block 100
- Then fully spendable
- Plan accordingly

---

## 🔧 **TROUBLESHOOTING**

### **If Miner Can't Connect:**
```bash
# Check tunnel is running
ps aux | grep "ssh.*19098"

# Test RPC directly
curl -s http://127.0.0.1:19098/ | jq
```

### **If No Blocks Mined:**
```bash
# Check difficulty
curl -s -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"getdifficulty","params":[]}' \
  http://127.0.0.1:19098/

# Check miner logs for errors
# Should show "Block found!" when successful
```

### **If Server Crashes:**
```bash
ssh -i /tmp/server_key root@96.9.226.98 \
  "journalctl -u dinerod -n 100 --no-pager"
```

---

## 📝 **POST-MINING CHECKLIST**

After mining 100+ blocks:

- [ ] Verify premine is spendable
- [ ] Test sending a transaction
- [ ] Backup Mac wallet
- [ ] Check total supply matches expectations
- [ ] Verify UTXO set consistency
- [ ] **Announce mainnet launch!** 🎉

---

## 🎊 **YOU'RE READY!**

**All systems:**
- ✅ Blockchain: Working
- ✅ UTXO: Working  
- ✅ Mining: Ready
- ✅ Server: Running
- ✅ RPC: Working
- ✅ Premine: Initialized

**START MINING AND LAUNCH YOUR CRYPTOCURRENCY! 🚀**


