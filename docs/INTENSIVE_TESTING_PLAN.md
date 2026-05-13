# DineroCoin Intensive Testing Plan (Pre-Mainnet)

**Purpose:** Find and fix ALL bugs before mainnet launch
**Environment:** Mac (local development)
**Duration:** During Dell tower 7-day stability test
**Goal:** Zero surprises on mainnet

---

## Testing Philosophy

**Test everything. Break everything. Fix everything.**

- Test happy paths (normal use)
- Test edge cases (weird inputs)
- Test failure scenarios (what if it crashes?)
- Test concurrency (multiple operations at once)
- Test limits (maximum values, empty values)
- Test recovery (restart after crash)

---

## Test Environment Setup on Mac

### 1. Clean Slate Testing

For each test session, start fresh:

```bash
# Stop any running daemon
./build/dinero-cli stop 2>/dev/null || true
pkill -9 dinerod 2>/dev/null || true

# Backup existing data (if important)
mv ~/.dinero ~/.dinero.backup.$(date +%s) 2>/dev/null || true

# Fresh start
rm -rf ~/.dinero

# Start daemon
./build/dinerod -daemon

# Wait for startup
sleep 5

# Verify running
./build/dinero-cli getblockchaininfo
```

### 2. Multi-Node Local Testing

Test P2P networking with multiple local nodes:

```bash
# Node 1
./build/dinerod -daemon -datadir=~/.dinero-node1 -port=20999 -rpcport=20997

# Node 2
./build/dinerod -daemon -datadir=~/.dinero-node2 -port=21000 -rpcport=21001

# Node 3
./build/dinerod -daemon -datadir=~/.dinero-node3 -port=21002 -rpcport=21003

# Connect them
./build/dinero-cli -datadir=~/.dinero-node1 addnode "127.0.0.1:21000" add
./build/dinero-cli -datadir=~/.dinero-node1 addnode "127.0.0.1:21002" add

# Verify connections
./build/dinero-cli -datadir=~/.dinero-node1 getconnectioncount
```

---

## Testing Checklist

### Phase 1: Daemon Basics

**Test 1.1: Genesis Initialization**
- [ ] Fresh start creates genesis block at height 0
- [ ] Genesis hash matches expected: `00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74`
- [ ] Genesis merkle root correct
- [ ] Premine block 1 exists with 2,627,900 DIN
- [ ] Can restart daemon and reload genesis successfully

**Commands:**
```bash
./build/dinero-cli getblockchaininfo
./build/dinero-cli getblock 0
./build/dinero-cli getblock 1
./build/dinero-cli stop && sleep 3 && ./build/dinerod -daemon && sleep 5
./build/dinero-cli getblockchaininfo
```

**Test 1.2: Daemon Startup/Shutdown**
- [ ] Clean shutdown with `dinero-cli stop`
- [ ] Restart works without errors
- [ ] Crash recovery (kill -9 and restart)
- [ ] Multiple rapid restart cycles

**Commands:**
```bash
./build/dinero-cli stop
./build/dinerod -daemon
sleep 5
./build/dinero-cli getblockchaininfo

# Crash test
pkill -9 dinerod
sleep 2
./build/dinerod -daemon
sleep 5
./build/dinero-cli getblockchaininfo
```

**Test 1.3: RPC Server**
- [ ] RPC responds to all commands
- [ ] Cookie authentication works
- [ ] Invalid commands return proper errors
- [ ] Concurrent RPC calls work

**Commands:**
```bash
# Test all RPC methods
./build/dinero-cli help
./build/dinero-cli getblockchaininfo
./build/dinero-cli getconnectioncount
./build/dinero-cli getpeerinfo
./build/dinero-cli getbestblockhash
./build/dinero-cli getblock $(./build/dinero-cli getbestblockhash)
./build/dinero-cli gettxoutsetinfo

# Invalid command test
./build/dinero-cli invalid_command_xyz  # Should return error

# Concurrent test (run in parallel)
for i in {1..10}; do
  ./build/dinero-cli getblockchaininfo &
done
wait
```

---

### Phase 2: Mining Tests

**Test 2.1: CPU Miner - Single Block**
- [ ] Miner starts successfully
- [ ] Generates mining address
- [ ] Finds block 2 (first after premine)
- [ ] Block hash meets difficulty target
- [ ] Coinbase reward is correct (100 DIN)
- [ ] Block appears in blockchain

**Commands:**
```bash
# Build miner if not built
cmake --build build --target dinero-miner -j8

# Mine one block
./build/dinero-miner --address $(./build/dinero-cli getnewaddress) --max-blocks 1

# Verify
./build/dinero-cli getblockcount  # Should be 2
./build/dinero-cli getblock $(./build/dinero-cli getbestblockhash)
```

**Test 2.2: CPU Miner - Multiple Blocks**
- [ ] Mine 10 blocks in a row
- [ ] Difficulty adjusts correctly (ASERT)
- [ ] Block times are reasonable
- [ ] No duplicate blocks
- [ ] Blockchain height increases correctly

**Commands:**
```bash
./build/dinero-miner --address $(./build/dinero-cli getnewaddress) --max-blocks 10

# Check results
./build/dinero-cli getblockcount  # Should be 12 (genesis + premine + 10)
./build/dinero-cli getblockchaininfo

# Check difficulty progression
for i in {0..12}; do
  echo "Block $i:"
  ./build/dinero-cli getblock $(./build/dinero-cli getblockhash $i) | grep -E "height|difficulty|time"
done
```

**Test 2.3: Mining Edge Cases**
- [ ] Stop mining mid-block (Ctrl+C)
- [ ] Restart mining after stop
- [ ] Mine with invalid address (should fail)
- [ ] Mine while daemon is stopped (should fail)

**Commands:**
```bash
# Test graceful stop (Ctrl+C after few seconds)
./build/dinero-miner --address $(./build/dinero-cli getnewaddress) --max-blocks 5
# Press Ctrl+C after 5-10 seconds

# Try again
./build/dinero-miner --address $(./build/dinero-cli getnewaddress) --max-blocks 1

# Invalid address
./build/dinero-miner --address "invalid_address_xyz" --max-blocks 1  # Should fail

# Mine with stopped daemon
./build/dinero-cli stop
./build/dinero-miner --address "din1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqfvd3xa" --max-blocks 1  # Should fail
```

---

### Phase 3: Wallet Tests

**Test 3.1: Address Generation**
- [ ] Generate new address
- [ ] Address format is valid (starts with "din1")
- [ ] Generate 100 addresses (stress test)
- [ ] All addresses are unique

**Commands:**
```bash
# Single address
ADDR=$(./build/dinero-cli getnewaddress)
echo "Generated: $ADDR"

# Verify format
echo "$ADDR" | grep -E "^din1[a-z0-9]{58}$" && echo "✓ Valid format" || echo "✗ Invalid format"

# Generate 100 addresses
for i in {1..100}; do
  ./build/dinero-cli getnewaddress
done | sort | uniq -d  # Should output nothing (all unique)
```

**Test 3.2: Receiving Coins**
- [ ] Mine to address
- [ ] Check balance after mining
- [ ] Coinbase maturity (100 blocks)
- [ ] Balance updates correctly

**Commands:**
```bash
# Generate address and mine to it
WALLET_ADDR=$(./build/dinero-cli getnewaddress)
echo "Mining to: $WALLET_ADDR"

# Mine 10 blocks
./build/dinero-miner --address "$WALLET_ADDR" --max-blocks 10

# Check balance (should be 1000 DIN from 10 blocks)
./build/dinero-cli getbalance

# Mine 100 more blocks for maturity
./build/dinero-miner --address "$WALLET_ADDR" --max-blocks 100

# Check spendable balance
./build/dinero-cli getbalance
```

**Test 3.3: Sending Coins**
- [ ] Create transaction
- [ ] Send to valid address
- [ ] Transaction appears in mempool
- [ ] Transaction gets mined
- [ ] Balance updates correctly
- [ ] Recipient receives coins

**Commands:**
```bash
# Need mature coins first (mine 110 blocks)
SENDER=$(./build/dinero-cli getnewaddress)
./build/dinero-miner --address "$SENDER" --max-blocks 110

# Create recipient
RECIPIENT=$(./build/dinero-cli getnewaddress)

# Send 50 DIN
TXID=$(./build/dinero-cli sendtoaddress "$RECIPIENT" 50)
echo "Transaction ID: $TXID"

# Check mempool
./build/dinero-cli getrawmempool

# Mine block to confirm
./build/dinero-miner --address "$SENDER" --max-blocks 1

# Verify transaction
./build/dinero-cli gettransaction "$TXID"
./build/dinero-cli getbalance
```

**Test 3.4: Wallet Backup/Restore**
- [ ] Backup wallet
- [ ] Generate new addresses
- [ ] Restore wallet
- [ ] Verify addresses restored

**Commands:**
```bash
# Backup
./build/dinero-cli backupwallet ~/wallet_backup.dat

# Generate new addresses (after backup)
for i in {1..5}; do
  ./build/dinero-cli getnewaddress
done

# Stop daemon
./build/dinero-cli stop
sleep 3

# Restore backup (simulating recovery)
rm -rf ~/.dinero
mkdir -p ~/.dinero
cp ~/wallet_backup.dat ~/.dinero/wallet.dat

# Restart
./build/dinerod -daemon
sleep 5

# Verify restoration
./build/dinero-cli getaddressesbyaccount ""
```

---

### Phase 4: P2P Network Tests

**Test 4.1: Two-Node Sync**
- [ ] Start two nodes
- [ ] Connect them manually
- [ ] Mine on node 1
- [ ] Node 2 receives block
- [ ] Both nodes at same height

**Commands:**
```bash
# Clean start
./build/dinero-cli stop 2>/dev/null
rm -rf ~/.dinero-node1 ~/.dinero-node2

# Start nodes
./build/dinerod -daemon -datadir=~/.dinero-node1 -port=20999 -rpcport=20997
./build/dinerod -daemon -datadir=~/.dinero-node2 -port=21000 -rpcport=21001
sleep 5

# Connect nodes
./build/dinero-cli -datadir=~/.dinero-node1 addnode "127.0.0.1:21000" add

# Verify connection
./build/dinero-cli -datadir=~/.dinero-node1 getconnectioncount  # Should be 1
./build/dinero-cli -datadir=~/.dinero-node2 getconnectioncount  # Should be 1

# Mine on node 1
ADDR1=$(./build/dinero-cli -datadir=~/.dinero-node1 getnewaddress)
./build/dinero-miner --rpc-port 20997 --address "$ADDR1" --max-blocks 5

# Check both nodes
./build/dinero-cli -datadir=~/.dinero-node1 getblockcount  # Should be 7
./build/dinero-cli -datadir=~/.dinero-node2 getblockcount  # Should be 7
```

**Test 4.2: Three-Node Network**
- [ ] Start three nodes
- [ ] Connect in mesh (all to all)
- [ ] Mine on node 1
- [ ] All nodes sync
- [ ] Disconnect node 2
- [ ] Mine on node 1
- [ ] Node 3 still syncs
- [ ] Reconnect node 2
- [ ] Node 2 catches up

**Commands:**
```bash
# Start three nodes
./build/dinerod -daemon -datadir=~/.dinero-node1 -port=20999 -rpcport=20997
./build/dinerod -daemon -datadir=~/.dinero-node2 -port=21000 -rpcport=21001
./build/dinerod -daemon -datadir=~/.dinero-node3 -port=21002 -rpcport=21003
sleep 5

# Mesh connect
./build/dinero-cli -datadir=~/.dinero-node1 addnode "127.0.0.1:21000" add
./build/dinero-cli -datadir=~/.dinero-node1 addnode "127.0.0.1:21002" add
./build/dinero-cli -datadir=~/.dinero-node2 addnode "127.0.0.1:21002" add

# Verify all connected
./build/dinero-cli -datadir=~/.dinero-node1 getconnectioncount  # 2
./build/dinero-cli -datadir=~/.dinero-node2 getconnectioncount  # 2
./build/dinero-cli -datadir=~/.dinero-node3 getconnectioncount  # 2

# Mine and verify all sync
ADDR=$(./build/dinero-cli -datadir=~/.dinero-node1 getnewaddress)
./build/dinero-miner --rpc-port 20997 --address "$ADDR" --max-blocks 3

./build/dinero-cli -datadir=~/.dinero-node1 getblockcount
./build/dinero-cli -datadir=~/.dinero-node2 getblockcount
./build/dinero-cli -datadir=~/.dinero-node3 getblockcount
# All should be same height
```

**Test 4.3: Block Propagation Speed**
- [ ] Time how fast blocks propagate
- [ ] Should be < 5 seconds for local network

**Commands:**
```bash
# Mine and time propagation
time (
  ./build/dinero-miner --rpc-port 20997 --address "$ADDR" --max-blocks 1
  sleep 2
  ./build/dinero-cli -datadir=~/.dinero-node2 getblockcount
)
```

---

### Phase 5: Edge Cases & Stress Tests

**Test 5.1: Empty Block Mining**
- [ ] Mine block with no transactions (only coinbase)
- [ ] Block validates correctly

**Test 5.2: Maximum Size Block**
- [ ] Create many small transactions
- [ ] Fill mempool
- [ ] Mine block with many transactions

**Test 5.3: Reorg Handling**
- [ ] Create fork with two miners
- [ ] Resolve fork (longest chain wins)
- [ ] Verify reorg happens correctly

**Commands:**
```bash
# Two miners on separate nodes
# Node 1 mines 5 blocks
./build/dinero-miner --rpc-port 20997 --address "$ADDR1" --max-blocks 5 &

# Node 2 mines 7 blocks (will win)
./build/dinero-miner --rpc-port 21001 --address "$ADDR2" --max-blocks 7 &

# Wait
wait

# Verify longest chain won
./build/dinero-cli -datadir=~/.dinero-node1 getblockcount
./build/dinero-cli -datadir=~/.dinero-node2 getblockcount
```

**Test 5.4: Invalid Transaction Rejection**
- [ ] Try to send more coins than balance
- [ ] Try to send to invalid address
- [ ] Try to send negative amount
- [ ] Try to double-spend

**Commands:**
```bash
# Insufficient balance
./build/dinero-cli sendtoaddress "$RECIPIENT" 999999999  # Should fail

# Invalid address
./build/dinero-cli sendtoaddress "not_a_valid_address" 1  # Should fail

# Negative amount
./build/dinero-cli sendtoaddress "$RECIPIENT" -10  # Should fail
```

**Test 5.5: Rapid Start/Stop Cycles**
- [ ] Start and stop daemon 10 times rapidly
- [ ] No corruption
- [ ] Blockchain state consistent

**Commands:**
```bash
for i in {1..10}; do
  echo "Cycle $i"
  ./build/dinerod -daemon
  sleep 3
  ./build/dinero-cli getblockcount
  ./build/dinero-cli stop
  sleep 2
done

# Final verification
./build/dinerod -daemon
sleep 5
./build/dinero-cli getblockchaininfo
```

---

### Phase 6: Performance Tests

**Test 6.1: Sync Speed**
- [ ] Node 1 mines 100 blocks
- [ ] Node 2 starts fresh and syncs
- [ ] Time to full sync

**Commands:**
```bash
# Mine 100 blocks on node 1
./build/dinero-miner --rpc-port 20997 --address "$ADDR1" --max-blocks 100

# Start fresh node 2 and time sync
rm -rf ~/.dinero-node2
./build/dinerod -daemon -datadir=~/.dinero-node2 -port=21000 -rpcport=21001
sleep 5
./build/dinero-cli -datadir=~/.dinero-node2 addnode "127.0.0.1:20999" add

# Monitor sync
while true; do
  HEIGHT=$(./build/dinero-cli -datadir=~/.dinero-node2 getblockcount)
  echo "Height: $HEIGHT / 102"
  if [ "$HEIGHT" -ge 102 ]; then
    break
  fi
  sleep 1
done
```

**Test 6.2: Memory Usage During Sync**
- [ ] Monitor memory during 1000 block sync
- [ ] Ensure no leaks (memory should stabilize)

**Commands:**
```bash
# Monitor memory while syncing
while true; do
  ps aux | grep dinerod | grep -v grep | awk '{print $6/1024 " MB"}'
  sleep 5
done
```

**Test 6.3: RPC Throughput**
- [ ] 1000 rapid getblockchaininfo calls
- [ ] Time to complete

**Commands:**
```bash
time (
  for i in {1..1000}; do
    ./build/dinero-cli getblockchaininfo > /dev/null
  done
)
```

---

## Bug Tracking Template

For each bug found, document:

```markdown
### Bug #XXX: [Short Description]

**Severity:** Critical / High / Medium / Low

**Steps to Reproduce:**
1. Step 1
2. Step 2
3. Expected: X, Got: Y

**Root Cause:**
[Analysis of what's wrong]

**Fix:**
[What was changed]

**Verification:**
[How you confirmed it's fixed]

**Commit:** [commit hash]
```

---

## Daily Testing Routine

**Each day:**
1. Run 1-2 test phases
2. Document bugs found
3. Fix bugs immediately
4. Re-test to verify fixes
5. Commit fixes to GitHub
6. Check Dell tower stability once

**Example day:**
- Morning: Check Dell tower (5 min)
- 9am-12pm: Phase 1 tests, find 3 bugs, fix them
- 12pm-1pm: Break
- 1pm-5pm: Phase 2 tests, find 2 bugs, fix them
- 5pm: Check Dell tower again
- Commit all fixes

---

## Success Criteria

Before mainnet launch:

- [ ] All Phase 1-6 tests pass
- [ ] Zero critical bugs remain
- [ ] All found bugs documented and fixed
- [ ] Dell tower 7-day test passed
- [ ] Code review complete
- [ ] Documentation complete

---

**Remember: It's better to find bugs now than during mainnet launch!**
