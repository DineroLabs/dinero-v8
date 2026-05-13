# Dell & Mac Server Access Guide

## Local Dell Linux
- **IP:** 192.168.1.114
- **User:** tower
- **Password:** tower

```bash
ssh tower@192.168.1.114
```

---

## DineroVA Server
- **IP:** 173.249.195.59
- **User:** root
- **Password:** H4lxnG0geuPL9qGTzwlm
- **SSH Key (Mac):** ~/.ssh/dinerova_key

### From Mac:
```bash
ssh -i ~/.ssh/dinerova_key root@173.249.195.59
```

### From Dell Linux:
```bash
ssh -i ~/.ssh/dinerova_key root@173.249.195.59
```
(Key already installed on Dell)

---

## DineroLA Server
- **IP:** 172.93.160.131
- **User:** root
- **SSH Key (Mac):** ~/.ssh/dinerocoin_la_key
- **SSH Key (Dell):** ~/.ssh/dinerocoin_la_key

### From Mac:
```bash
ssh -i ~/.ssh/dinerocoin_la_key root@172.93.160.131
```

### From Dell Linux:
```bash
ssh -i ~/.ssh/dinerocoin_la_key root@172.93.160.131
```

---

## DineroCA Server
- **IP:** 96.9.226.98
- **User:** root
- **SSH Key (Mac):** .server-key (in DineroCoin repo)

### From Mac:
```bash
ssh -i /Users/haydarevich/Documents/DineroCoin/.server-key root@96.9.226.98
```

---

## Mac (Local)
- **Public IP:** 162.200.227.214
- **Local IP:** 192.168.1.130
- **User:** haydarevich

---

## Quick Reference

| Server | IP | User | Access |
|--------|-----|------|--------|
| Dell Linux | 192.168.1.114 | tower | password: tower |
| DineroVA | 173.249.195.59 | root | key: ~/.ssh/dinerova_key |
| DineroLA | 172.93.160.131 | root | key: ~/.ssh/dinerocoin_la_key |
| DineroCA | 96.9.226.98 | root | key: .server-key |

---
## UPDATE: Jan 27, 2026 - Network Setup Progress

### ISSUE DISCOVERED: Binary Version Mismatch
- Mac binary was built Jan 26 23:16
- Latest commit was Jan 26 23:19 (3 minutes AFTER binary built)
- Dell binary built from latest commit
- Result: Different consensus rules = "bad-pow" block rejection

### FIX APPLIED:
- Rebuilt Mac dinerod from latest commit (a4ce519e5)
- Mac now running with 219 blocks
- Both Mac and Dell on same git commit

### CURRENT STATUS:
Node      | Status              | Blocks | Port
----------|---------------------|--------|------
Mac       | Running             | 219    | 20999
Dell      | JSON Bug (crashed)  | -      | 20999
DineroVA  | Building            | -      | 20999

### KNOWN BUG: Dell JSON Crash
Error: Json::LogicError: in Json::Value::find(begin, end): requires objectValue or nullValue
- Happens when RPC called during sync
- Need to investigate chainstate/RPC code

### TO RESTART DELL MAINNET DAEMON:
```bash
# Kill any existing daemon
pkill -9 -f dinerod

# Clear data for fresh sync
rm -rf ~/dinero-mainnet-data/*

# Start daemon
cd ~/Dinero-Coin/build
./dinerod -datadir=$HOME/dinero-mainnet-data -port=20999 -rpcport=20998 -server=1 -listen=1 -addnode=192.168.1.130:20999 -daemon

# Check if running
pgrep -af dinerod

# Check block height (wait 30 sec for startup)
curl -s --user $(cat ~/dinero-mainnet-data/.cookie) \
  -H "Content-Type: application/json" \
  -d '{"method":"getblockcount","params":[],"id":1}' \
  http://127.0.0.1:20998/
```

### NETWORK TOPOLOGY:
```
Internet Users
      |
      v
seed2.dinero-coin.com (173.249.195.59)
      |
      v
  DineroVA (public seed node)
      ^
      | (Dell connects OUT)
      v
Mac <--LAN--> Dell
192.168.1.130    192.168.1.114
```

### SYNC BLOCKCHAIN TO DINEROVA:
Once DineroVA build is done:
```bash
# From Mac:
rsync -avz --progress ~/dinero_data/ root@173.249.195.59:/root/dinero_data/

# Or from Dell:
rsync -avz --progress ~/dinero-mainnet-data/ root@173.249.195.59:/root/dinero_data/
```

### START DINEROVA DAEMON:
```bash
ssh -i ~/.ssh/dinerova_key root@173.249.195.59
cd /root/Dinero-Coin/build
./dinerod -datadir=/root/dinero_data -port=20999 -rpcport=20998 -server=1 -listen=1 -daemon
```

### GIT COMMITS (must match on all nodes):
```
Mac:  a4ce519e5 fix: BIP86 Taproot wallet improvements
Dell: a4ce519e5 fix: BIP86 Taproot wallet improvements
```

### FILES LOCATION:
- Mac binary: /Users/haydarevich/Desktop/DinerMacFolder/dinerod
- Mac config: /Users/haydarevich/Documents/DineroCoin/dinero-mainnet.conf
- Mac data: ~/.dinero/
- Dell binary: ~/Dinero-Coin/build/dinerod
- Dell data: ~/dinero-mainnet-data/
- DineroVA binary: /root/Dinero-Coin/build/dinerod
- DineroVA data: /root/dinero_data/

### SSH ACCESS:
- Dell: ssh tower@192.168.1.114 (password: tower)
- DineroVA: ssh -i ~/.ssh/dinerova_key root@173.249.195.59

=== P2P HEADER SYNC FIX @ Tue Jan 28 00:00 EST 2026 ===

**ROOT CAUSE FOUND:**
Nodes connect but don't sync because headers only requested when:
```cpp
if (peer_height > our_height + 1) {  // WRONG!
    syncWithPeer(peer_id);
}
```

This fails when:
- Both nodes at same height (peer_height == our_height)
- Peer has headers but fewer blocks (forks)
- Zero-state bootstrap

**FIX APPLIED (commit b6b9e53c0):**
```cpp
// Always send getheaders on new peer connection
// Height is not a reliable indicator of chain superiority
// Bitcoin Core rule: always request headers from new peers
syncWithPeer(peer_id);  // UNCONDITIONAL
```

**CHANGES:**
- File: src/daemon/network_message_handlers.cpp line 142
- Removed height check before syncWithPeer()
- Now sends getheaders to EVERY new peer on handshake complete

**ALL NODES NEED THIS FIX:**
```bash
# Dell:
cd ~/Dinero-Coin && git pull
cmake --build build --target dinerod -j4
pkill -9 -f dinerod
./build/dinerod -datadir=$HOME/dinero-mainnet-data -port=20999 -rpcport=20998 -server=1 -listen=1 -addnode=192.168.1.130:20999 -daemon

# DineroVA:
cd /root/Dinero-Coin && git pull
cmake --build build --target dinerod -j4
pkill -9 dinerod
./dinerod -datadir=/root/.dinero -port=20999 -rpcport=20998 -server=1 -listen=1 -daemon
```

**STATUS AFTER FIX:**
- Mac: Rebuilt and running (219 blocks)
- Dell: Pulled and rebuilt
- DineroVA: Needs git pull and rebuild

**EXPECTED BEHAVIOR:**
- On peer connect → getheaders sent immediately
- Headers flow → blocks follow
- No more "connected but not syncing"

=== MINER POW VALIDATION FIX @ Tue Jan 28 2026 ===

**DELL'S ANALYSIS CONFIRMED:**
The miner's `DineroPoW::MeetsTarget` was comparing reversed hash with big-endian target.

**ROOT CAUSE:**
- `double_sha256()` returns hash in reversed/little-endian display format
- `BitsToTargetHex()` returns target in big-endian format
- Comparing these directly is mathematically incorrect

**BUG (miner.cpp line 569):**
```cpp
bool DineroPoW::MeetsTarget(const std::string& hash, uint32_t target_bits) {
    std::string target_hex = BitsToTargetHex(target_bits);
    return hash <= target_hex;  // WRONG: comparing little-endian hash with big-endian target
}
```

**FIX APPLIED (Mac):**
```cpp
bool DineroPoW::MeetsTarget(const std::string& hash, uint32_t target_bits) {
    std::string target_hex = BitsToTargetHex(target_bits);

    // CRITICAL FIX: reverse hash from little-endian to big-endian
    std::string hash_reversed;
    for (int i = 63; i >= 0; i -= 2) {
        hash_reversed += hash.substr(i - 1, 2);
    }

    return hash_reversed <= target_hex;  // CORRECT: both now big-endian
}
```

**FILE:** src/mining/miner.cpp lines 565-576

**STATUS:**
- Mac: Fix committed (d4a76a59c), pushed, rebuilt, CHAIN RESET COMPLETE (height=1)
- Dell: Needs git pull, rebuild, and CHAIN RESET
- DineroVA: Needs git pull, rebuild, and CHAIN RESET

=== CHAIN RESET INSTRUCTIONS @ Tue Jan 28 00:20 EST 2026 ===

**DECISION:** Reset all chains. The 219 blocks were mined with buggy PoW validation.
Clean start with fixed miner code.

**MAC DONE:**
- Commit d4a76a59c pushed to origin/main
- Daemon rebuilt and running
- Chain data cleared, now at height=1

**DELL - RUN THESE COMMANDS:**
```bash
# 1. Pull the miner fix
cd ~/Dinero-Coin && git pull

# 2. Rebuild
cmake --build build --target dinerod -j4

# 3. Stop daemon and clear data
pkill -9 dinerod
rm -rf ~/dinero-mainnet-data/blockchain ~/dinero-mainnet-data/headers ~/dinero-mainnet-data/wallet*

# 4. Restart fresh
cd ~/Dinero-Coin/build
./dinerod -datadir=$HOME/dinero-mainnet-data -port=20999 -rpcport=20998 -server=1 -listen=1 -addnode=192.168.1.130:20999 -daemon

# 5. Verify height=1
sleep 10 && curl -s --user $(cat ~/dinero-mainnet-data/.cookie) -H "Content-Type: application/json" -d '{"method":"getblockcount","params":[],"id":1}' http://127.0.0.1:20998/
```

**DINEROVA - RUN THESE COMMANDS:**
```bash
# 1. Pull the miner fix
cd /root/Dinero-Coin && git pull

# 2. Rebuild
cmake --build build --target dinerod -j4

# 3. Stop daemon and clear data
pkill -9 dinerod
rm -rf /root/.dinero/blockchain /root/.dinero/headers /root/.dinero/wallet*

# 4. Restart fresh
cd /root/Dinero-Coin/build
./dinerod -datadir=/root/.dinero -port=20999 -rpcport=20998 -server=1 -listen=1 -daemon

# 5. Verify height=1
sleep 10 && curl -s --user $(cat /root/.dinero/.cookie) -H "Content-Type: application/json" -d '{"method":"getblockcount","params":[],"id":1}' http://127.0.0.1:20998/
```

**AFTER ALL NODES RESET:**
- All at height=1 (genesis only)
- Mac starts mining with FIXED PoW validation
- Blocks will be properly validated this time
- Dell and DineroVA sync from Mac

**GIT COMMIT TO PULL:**
```
d4a76a59c fix: Miner PoW validation - reverse hash before target comparison
```
