# Dinero Chain Reset — Deployment Procedure

**Status**: Pre-mainnet chain reset (consensus fixes require new genesis)
**Date**: 2026-02-22
**Reason**: Utreexo v2 commitment, domain-separated hashing, BIP340 taggedHash fix

---

## Prerequisites

- [ ] All Freeze Gates (A–D) pass
- [ ] `verify_genesis_bundle` passes on mined artifact bundle
- [ ] Binary built and tested locally (`dinerod` starts, accepts genesis + premine)
- [ ] Binary built on Dell Tower (Linux x86_64)

---

## 1. Stop All Nodes

```bash
# Dell Tower (miner)
sshpass -p 'tower' ssh tower@tower-Precision-7920-Tower.local \
  'pkill -f dinerod; pkill -f dinero-solo-miner; sleep 2; pgrep dinerod && echo "STILL RUNNING" || echo "stopped"'

# DineroLA
ssh -i ~/.ssh/dincalifornia.key root@172.93.160.131 \
  'systemctl stop dinerod; systemctl stop dinero-stratum; sleep 2; systemctl is-active dinerod || echo "stopped"'

# DineroVA
ssh -i ~/.ssh/dinerova_key root@173.249.195.59 \
  'systemctl stop dinerod; sleep 2; systemctl is-active dinerod || echo "stopped"'
```

---

## 2. Wipe Chain State (All Nodes)

**WARNING**: This deletes the entire old chain. Wallet data (keys/seeds) is NOT in the data dir.

```bash
DATA_DIR=~/Dinero-Coin/data-main

# Wipe chainstate, blocks, peers, and Utreexo forest
rm -rf "$DATA_DIR/blocks" "$DATA_DIR/chainstate" "$DATA_DIR/peers.dat" \
       "$DATA_DIR/utreexo" "$DATA_DIR/banlist.dat" "$DATA_DIR/fee_estimates.dat"

# Keep: dinero.conf, wallet files (if any), debug.log (for post-mortem)
ls "$DATA_DIR"  # Verify only config/logs remain
```

Repeat on all three servers.

---

## 3. Deploy New Binary

### Dell Tower (build from source)

```bash
# On Mac: push code
cd ~/src/dinero && git push github p2p-fix

# On Dell Tower: pull + build
sshpass -p 'tower' ssh tower@tower-Precision-7920-Tower.local << 'REMOTE'
  cd ~/src/dinero
  git pull
  cmake --build build -j$(nproc)
  ./build/dinerod --version
REMOTE
```

### DineroLA + DineroVA (deploy binary)

```bash
# Copy from Dell Tower to remote servers
# (Adjust path to wherever the binary lives on Dell Tower)
TOWER="tower@tower-Precision-7920-Tower.local"
BINARY_PATH="/home/tower/src/dinero/build/dinerod"

# DineroLA
scp -i ~/.ssh/dincalifornia.key \
  <(sshpass -p 'tower' ssh $TOWER "cat $BINARY_PATH") \
  root@172.93.160.131:/usr/local/bin/dinerod

# DineroVA
scp -i ~/.ssh/dinerova_key \
  <(sshpass -p 'tower' ssh $TOWER "cat $BINARY_PATH") \
  root@173.249.195.59:/usr/local/bin/dinerod
```

Or use the standard workflow: build on Dell Tower, then `scp` the binary from Dell Tower to each remote server directly.

---

## 4. Start Nodes

```bash
# Dell Tower
sshpass -p 'tower' ssh tower@tower-Precision-7920-Tower.local \
  'cd ~/Dinero-Coin && nohup ./build/dinerod -datadir=data-main > /tmp/dinerod.log 2>&1 &'

# DineroLA
ssh -i ~/.ssh/dincalifornia.key root@172.93.160.131 \
  'systemctl start dinerod'

# DineroVA
ssh -i ~/.ssh/dinerova_key root@173.249.195.59 \
  'systemctl start dinerod'
```

---

## 5. Health Checks

### Verify genesis + premine accepted

```bash
# On each node, check tip height = 1 (genesis + premine)
dinerod -datadir=~/Dinero-Coin/data-main getblockcount
# Expected: 1

# Verify genesis hash matches artifact bundle
dinerod -datadir=~/Dinero-Coin/data-main getblockhash 0
# Expected: <genesis block_hash from artifact bundle>

# Verify premine hash
dinerod -datadir=~/Dinero-Coin/data-main getblockhash 1
# Expected: <premine block_hash from artifact bundle>
```

### Verify startup banner

Check `/tmp/dinerod.log` for:
```
[STARTUP] Utreexo enforcement: ACTIVE from height 0 (mainnet)
[STARTUP] Commitment format: v2 (numLeaves + 64 fixed slots, 2056-byte preimage)
```

### Verify peer connections

```bash
dinerod -datadir=~/Dinero-Coin/data-main getpeerinfo | grep addr
# All 3 nodes should connect to each other
```

---

## 6. Start Mining (Dell Tower Only)

```bash
sshpass -p 'tower' ssh tower@tower-Precision-7920-Tower.local \
  'cd ~/Dinero-Coin && nohup ./build/dinero-solo-miner \
    --rpc-url=http://127.0.0.1:20998 \
    --rpc-cookie=data-main/.cookie \
    --threads=$(nproc) \
    --address=din1pljx7yr8pcdrdxfx7qmqgnvlv4zsj7sg82zpvyraunyalllzsvzaqynrc80 \
    > /tmp/miner.log 2>&1 &'
```

### Verify blocks propagate

```bash
# Wait for a few blocks, then check all nodes agree
# Dell Tower
sshpass -p 'tower' ssh tower@tower-Precision-7920-Tower.local \
  './build/dinerod -datadir=Dinero-Coin/data-main getblockcount'

# DineroLA
ssh -i ~/.ssh/dincalifornia.key root@172.93.160.131 \
  'dinerod -datadir=/root/Dinero-Coin/data-main getblockcount'

# DineroVA
ssh -i ~/.ssh/dinerova_key root@173.249.195.59 \
  'dinerod -datadir=/root/Dinero-Coin/data-main getblockcount'

# All three should show same height
```

---

## 7. Update Downstream Components

After chain is running and stable:

- [ ] Rebuild NodeCore.xcframework: `bash build_nodecore_xcframework.sh`
- [ ] Rebuild DineroDPI with new xcframework
- [ ] Rebuild dinero-qt
- [ ] Update dinero-stratum on DineroLA (if cookie/genesis changed)
- [ ] Verify DineroDPI connects and syncs on physical iPhone

---

## Rollback Plan

If the new chain has issues:

1. Stop all nodes
2. Revert to previous git commit: `git checkout <prev-commit>`
3. Rebuild binary
4. Wipe chainstate again (step 2)
5. Deploy old binary (step 3)
6. Restart (step 4)

The old chain data is already wiped, so there is no "restore old chain" — only "fix forward."

---

## Server Reference

| Server | Access | Data Dir | RPC Port |
|--------|--------|----------|----------|
| Dell Tower | `sshpass -p 'tower' ssh tower@tower-Precision-7920-Tower.local` | `~/Dinero-Coin/data-main` | 20998 |
| DineroLA | `ssh -i ~/.ssh/dincalifornia.key root@172.93.160.131` | `~/Dinero-Coin/data-main` | 20998 |
| DineroVA | `ssh -i ~/.ssh/dinerova_key root@173.249.195.59` | `~/Dinero-Coin/data-main` | 20998 |
