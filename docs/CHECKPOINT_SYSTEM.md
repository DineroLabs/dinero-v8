# Checkpoint System - Anti-Fork Protection
**November 7, 2025**

## 🔒 **What Are Checkpoints?**

Checkpoints are **hardcoded block hashes** in the daemon binary that serve two critical purposes:

1. **Anti-Reorg Protection**: Prevent blockchain reorganizations past checkpoint blocks
2. **Fake Chain Prevention**: Reject any chain that doesn't match checkpoint hashes

---

## 🎯 **Why Checkpoints Matter**

### **Without Checkpoints**
```
Attacker creates fake chain:
  Block 0: 173fe6da... (real genesis) ✅
  Block 1: 0000002bd3fa677b... (real premine) ✅
  Block 2: [fake_hash] (pays to attacker)
  Block 3-1000: [more fake blocks with high PoW]

If attacker has more chainwork → Nodes follow fake chain! ❌
```

### **With Checkpoints**
```
Checkpoints in code:
  {0, "173fe6da..."}     ✅ Genesis
  {1, "0000002bd3fa677b..."}     ✅ Premine
  {100, "00001234..."}   ✅ Block 100

Attacker creates fake chain:
  Block 0: 173fe6da... ✅ Matches checkpoint
  Block 1: 0000002bd3fa677b... ✅ Matches checkpoint
  Block 100: [fake]    ❌ Doesn't match checkpoint

Result: Nodes reject attacker's chain at block 100! ✅
```

---

## 📊 **Current Checkpoint Status**

### **Active Checkpoints**
```cpp
{0, "173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33"},  // Genesis
{1, "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a"},  // Premine
```

### **Pending Checkpoints** (Current mainnet: 296+ blocks)
```
Block 100:   ⏳ Can be added now (exists on mainnet)
Block 500:   ⏳ Not mined yet (wait until height ≥ 500)
Block 1000:  ⏳ Not mined yet
Block 5000:  ⏳ Not mined yet
Block 10000: ⏳ Not mined yet
Block 20000: ⏳ Not mined yet
```

---

## 🛠️ **How to Add Checkpoints**

### **Step 1: Get Block Hashes**

**Option A: Use the helper script (Recommended)**
```bash
cd /Users/haydarevich/Documents/DineroCoin

# Make sure daemon is running
./build/bin/dinerod -datadir=$HOME/.dinero &

# Run the checkpoint script
./tools/get_checkpoint_hashes.sh

# Output example:
# {100, "000012345abcdef..."},  // Block 100 checkpoint
# // {500, "[hash]"},  // ⏳ Not mined yet (current: 296)
```

**Option B: Manual RPC calls**
```bash
# Get hash for block 100 (if it exists)
./build/bin/dinero-cli getblockhash 100

# Output: 000012345abcdef...
```

### **Step 2: Update chainparams_impl.cpp**

Edit `src/consensus/chainparams_impl.cpp` (around line 127):

```cpp
.vCheckpoints = {
    // IMMUTABLE CHECKPOINTS (Never change these!)
    {0, "173fe6da2ccc8a671380c8845a2c15e70cc8b84132fa2ab61108425c85412a33"},
    {1, "0000002bd3fa677b43cc4efcb82078db53131f191ad72afb9d463ddd33c7f79a"},
    
    // NEW CHECKPOINT (Add the hash you fetched)
    {100, "000012345abcdef..."},  // ← Uncomment and add real hash
}
```

### **Step 3: Rebuild Binaries**

```bash
cd /Users/haydarevich/Documents/DineroCoin/build

# Clean rebuild
rm -rf * && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j8

# Verify new binary
./bin/dinerod --version
# Should show new build timestamp
```

### **Step 4: Deploy to All Nodes**

**Mac Deployment:**
```bash
# Update DineroMacPublic package
cp bin/dinerod /Users/haydarevich/Desktop/DineroMacPublic/bin/
cp bin/dinero-cli /Users/haydarevich/Desktop/DineroMacPublic/bin/

# Update checksums
cd /Users/haydarevich/Desktop/DineroMacPublic/bin
shasum -a 256 * > ../CHECKSUM.txt
```

**Linux Deployment:**
```bash
# Deploy to servers
./deploy_genesis_fix_to_linux.sh

# Or manually:
ssh root@172.93.160.131 "cd /root/DineroCoin && git pull && cd build && make -j8"
ssh root@173.249.195.59 "cd /root/DineroCoin && git pull && cd build && make -j8"
```

### **Step 5: Restart All Nodes**

```bash
# Mac
pkill dinerod && ./bin/dinerod -datadir=$HOME/.dinero &

# Linux (California)
ssh root@172.93.160.131 "pkill dinerod && nohup /root/DineroCoin/build/bin/dinerod -datadir=/root/.dinero &"

# Linux (Virginia)
ssh root@173.249.195.59 "pkill dinerod && nohup /root/DineroCoin/build/bin/dinerod -datadir=/root/.dinero &"
```

---

## ⏰ **When to Add Checkpoints**

### **Recommended Schedule**

| Block Height | When to Add | Priority |
|--------------|-------------|----------|
| **100** | Now (exists) | 🔥 High |
| **500** | After 500 blocks mined | Medium |
| **1000** | After 1000 blocks mined | Medium |
| **5000** | After 5000 blocks mined | Medium |
| **10000** | Major milestone | High |
| **20000** | Major milestone | High |
| **50000** | Long-term growth | Medium |
| **100000** | Long-term growth | Medium |
| **210000** | First halving | 🔥 Critical |

### **When to Update**

✅ **Add checkpoints when:**
- Every ~1000 blocks for active chains
- Before major version releases
- After network upgrades
- When distributing binaries to new users

⚠️ **DON'T add checkpoints:**
- For very recent blocks (wait for 100+ confirmations)
- During active reorgs (let network stabilize)
- Without verifying hash on trusted node

---

## 🧪 **Testing Checkpoints**

### **Test 1: Verify Checkpoint Validation**

```bash
# Start daemon with checkpoint at block 100
./dinerod -datadir=./test_data

# Try to sync from a node with different block 100 hash
# Expected: Daemon should reject blocks and disconnect from peer
```

### **Test 2: Verify Reorg Protection**

```bash
# Current chain:
#   Block 100: 00001234... (checkpoint)
#   Block 101: 00005678...

# Attacker sends alternative chain:
#   Block 100: 00009999... (different hash)
#   Block 101-200: [fake blocks]

# Expected: Daemon rejects at block 100 (doesn't match checkpoint)
```

---

## 🔍 **Verifying Checkpoint Integrity**

### **Check Current Checkpoints**

```bash
# Search codebase for current checkpoints
grep -A5 "vCheckpoints" src/consensus/chainparams_impl.cpp

# Expected output:
# {0, "173fe6da..."},  // Genesis
# {1, "0000002bd3fa677b..."},  // Premine
# {100, "..."},        // Block 100 (if added)
```

### **Verify Hash Matches Network**

```bash
# On a trusted node (e.g., your California server):
./dinero-cli getblockhash 100

# Compare with checkpoint in code (should match exactly)
grep "100," src/consensus/chainparams_impl.cpp
```

---

## 📚 **Example: Adding Block 100 Checkpoint**

### **Complete Workflow**

```bash
# 1. Get block 100 hash from running node
cd /Users/haydarevich/Documents/DineroCoin
./build/bin/dinero-cli -datadir=$HOME/.dinero getblockhash 100

# Output example:
# 00001234567890abcdef1234567890abcdef1234567890abcdef1234567890

# 2. Edit chainparams_impl.cpp
vim src/consensus/chainparams_impl.cpp

# Change:
#   // {100, "[hash]"},
# To:
#   {100, "00001234567890abcdef1234567890abcdef1234567890abcdef1234567890"},

# 3. Rebuild
cd build && rm -rf * && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j8

# 4. Test locally
./bin/dinerod -datadir=./test_checkpoint &
sleep 5
./bin/dinero-cli -datadir=./test_checkpoint getblockchaininfo

# 5. Deploy to production
cp bin/dinerod /Users/haydarevich/Desktop/DineroMacPublic/bin/
./deploy_genesis_fix_to_linux.sh

# 6. Announce to community
echo "🔒 Checkpoint added at block 100 - all users should update!"
```

---

## 🚨 **Security Best Practices**

### **DO:**
✅ Get checkpoint hashes from **trusted nodes** (your own servers)
✅ Wait for **100+ confirmations** before adding checkpoint
✅ **Double-check** hash matches across multiple nodes
✅ **Announce** checkpoint updates to community
✅ **Document** which blocks have checkpoints

### **DON'T:**
❌ Add checkpoints for **unconfirmed blocks** (< 100 confirmations)
❌ Get hashes from **untrusted sources** (could be fake)
❌ Add checkpoints during **active reorgs** (wait for stability)
❌ **Change existing checkpoints** (genesis/premine are immutable!)
❌ Add checkpoints **too frequently** (every 100-1000 blocks is enough)

---

## 📊 **Impact of Checkpoints**

### **Security**
- 🔒 **Prevents 51% attacks** past checkpoint blocks
- 🔒 **Stops deep reorgs** (can't reorganize past checkpoints)
- 🔒 **Rejects fake chains** (must match all checkpoint hashes)

### **Network**
- ⚡ **Faster sync** (nodes don't validate PoW before checkpoints)
- 🌐 **Stronger consensus** (all nodes agree on checkpoint blocks)
- 🛡️ **Attack resistance** (attacker can't fork past checkpoints)

### **Trade-offs**
- ⚠️ **Centralization risk** (requires trusted node for checkpoint hashes)
- ⚠️ **Less flexibility** (can't reorg past checkpoints, even if legitimate)
- ⚠️ **Update burden** (must rebuild/redeploy binaries)

---

## 🎯 **Recommended Next Steps**

1. **Add Block 100 Checkpoint** (current: 296 blocks, so 100 exists)
   ```bash
   ./tools/get_checkpoint_hashes.sh
   # Copy hash for block 100
   # Edit chainparams_impl.cpp
   # Rebuild and deploy
   ```

2. **Plan Future Checkpoints**
   - Block 500: Add when mainnet reaches ~600 blocks
   - Block 1000: Add when mainnet reaches ~1100 blocks
   - Update binaries before major releases

3. **Monitor Network Health**
   ```bash
   # Check for chain splits
   ./dinero-cli getpeerinfo | grep "startingheight"
   
   # All peers should have same height (no splits)
   ```

4. **Document Checkpoint History**
   - Keep record of when checkpoints were added
   - Note block height and hash for each checkpoint
   - Track which binary versions include which checkpoints

---

## 🏆 **Status**

- ✅ **Genesis checkpoint** (block 0): Immutable
- ✅ **Premine checkpoint** (block 1): Immutable
- ⏳ **Block 100 checkpoint**: Ready to add (mainnet at 296+ blocks)
- ⏳ **Future checkpoints**: Add as mainnet grows

**Next action**: Run `./tools/get_checkpoint_hashes.sh` to fetch block 100 hash and add it!

---

**See also:**
- `src/consensus/chainparams_impl.cpp` - Checkpoint definitions
- `tools/get_checkpoint_hashes.sh` - Helper script to fetch hashes
- Bitcoin Core checkpoint system (our implementation is similar)

