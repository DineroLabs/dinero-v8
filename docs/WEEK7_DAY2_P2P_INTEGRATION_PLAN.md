# Week 7 Day 2 - P2P Integration Testing & Validation Plan

**Date**: 2025-11-06
**Goal**: Validate WebSocket server and peer tracking implementations under real network conditions
**Prerequisites**: ✅ Week 7 P2P Implementation Complete (5/8 real implementations)

---

## 🎯 Objectives

1. **Live Multi-Node Testing** - Verify P2P connectivity with real daemons
2. **WebSocket Event Validation** - Confirm real-time events fire correctly
3. **Peer Tracking Accuracy** - Validate peer info RPC returns correct data
4. **Performance Benchmarking** - Measure handshake latency, event delivery time
5. **Integration Documentation** - Create runbooks for production deployment

---

## 📅 Test Plan Overview

| Test Suite | Duration | Priority | Pass Criteria |
|-------------|----------|----------|---------------|
| 1. P2P Handshake | 30 min | 🔴 Critical | 100% success rate |
| 2. WebSocket Auth | 20 min | 🔴 Critical | Auth blocks unauthorized |
| 3. Peer Info RPC | 20 min | 🔴 Critical | Accurate peer list |
| 4. WebSocket Events | 40 min | 🟡 High | Events fire <1s delay |
| 5. Multi-Node Sync | 60 min | 🟡 High | Blocks propagate |
| 6. Performance Bench | 30 min | 🟢 Optional | Latency <100ms |

**Total Time**: 3-4 hours

---

## 1️⃣ P2P Handshake Test (30 min)

### Goal
Verify two Dinero daemons can connect and complete P2P handshake.

### Setup

```bash
# Clean any existing test data
pkill -9 dinerod
rm -rf /tmp/node1 /tmp/node2

# Node 1: Listening node
mkdir -p /tmp/node1
./build/dinerod \
  --regtest \
  --rpcport=20001 \
  --port=30001 \
  --datadir=/tmp/node1 \
  --debug=net,p2p \
  -daemon

# Wait for Node 1 to start
sleep 3

# Node 2: Connecting node
mkdir -p /tmp/node2
./build/dinerod \
  --regtest \
  --rpcport=20002 \
  --port=30002 \
  --datadir=/tmp/node2 \
  --debug=net,p2p \
  --addnode=127.0.0.1:30001 \
  -daemon

# Wait for connection
sleep 5
```

### Verification

```bash
echo "=== Node 1 Peer List ==="
./build/dinero-cli -rpcport=20001 p2p.getpeerinfo

echo ""
echo "=== Node 2 Peer List ==="
./build/dinero-cli -rpcport=20002 p2p.getpeerinfo

echo ""
echo "=== Node 1 Debug Log (Handshake) ==="
tail -50 /tmp/node1/debug.log | grep -E "version|verack|connected"

echo ""
echo "=== Node 2 Debug Log (Handshake) ==="
tail -50 /tmp/node2/debug.log | grep -E "version|verack|connected"
```

### Expected Output

**Node 1 peer list**:
```json
{
  "connected_peers": 1,
  "max_outbound": 8,
  "peers": [
    {
      "addr": "127.0.0.1:30002",
      "services": "0000000000000000",
      "relaytxes": true,
      "version": 70015,
      "subver": "/Dinero:1.0.0/",
      "inbound": true
    }
  ]
}
```

**Node 2 peer list**:
```json
{
  "connected_peers": 1,
  "max_outbound": 8,
  "peers": [
    {
      "addr": "127.0.0.1:30001",
      "services": "0000000000000000",
      "relaytxes": true,
      "version": 70015,
      "subver": "/Dinero:1.0.0/",
      "inbound": false
    }
  ]
}
```

### Pass Criteria
- ✅ Both nodes show `"connected_peers": 1`
- ✅ Peer addresses match (Node 1 sees 30002, Node 2 sees 30001)
- ✅ Inbound/outbound flags correct (Node 1: inbound=true, Node 2: inbound=false)
- ✅ Debug logs show "version" and "verack" messages exchanged

### Troubleshooting
```bash
# If connection fails, check:

# 1. Ports are not in use
lsof -i :30001
lsof -i :30002

# 2. Firewall allows local connections
# (Usually not an issue for 127.0.0.1)

# 3. Check debug logs for errors
tail -100 /tmp/node1/debug.log | grep -i error
tail -100 /tmp/node2/debug.log | grep -i error

# 4. Verify daemons are running
ps aux | grep dinerod
```

---

## 2️⃣ WebSocket Authentication Test (20 min)

### Goal
Verify WebSocket connections require authentication.

### Setup

```bash
# Use Node 1 from previous test (still running)
# RPC port: 20001

# Get auth cookie
COOKIE=$(cat /tmp/node1/.cookie)
echo "Cookie: $COOKIE"

# Create base64 auth header
AUTH=$(echo -n "__cookie__:$COOKIE" | base64)
echo "Auth: $AUTH"
```

### Test 1: Unauthenticated Connection (Should Fail)

```bash
# Install wscat if not available
# npm install -g wscat

# Try to connect without auth (should be rejected)
wscat -c ws://localhost:20001 <<EOF
{"jsonrpc":"2.0","method":"getblockcount","id":1}
EOF
```

**Expected**: Connection rejected or authentication error response

### Test 2: Authenticated Connection (Should Succeed)

```bash
# Connect with proper auth
wscat -c ws://localhost:20001 -H "Authorization: Basic $AUTH" <<EOF
{"jsonrpc":"2.0","method":"getblockcount","id":1}
EOF
```

**Expected**: Connection accepted, command executed

**Example Output**:
```json
{
  "jsonrpc": "2.0",
  "result": 0,
  "id": 1
}
```

### Pass Criteria
- ✅ Unauthenticated connection rejected
- ✅ Authenticated connection accepted
- ✅ RPC commands work over WebSocket
- ✅ Authentication error has clear message

---

## 3️⃣ Peer Info RPC Accuracy Test (20 min)

### Goal
Verify `p2p.getpeerinfo` returns accurate data under various conditions.

### Test 1: No Peers
```bash
# Start isolated node
rm -rf /tmp/node-isolated
./build/dinerod --regtest --rpcport=20010 --datadir=/tmp/node-isolated -daemon
sleep 3

# Check peer list (should be empty)
./build/dinero-cli -rpcport=20010 p2p.getpeerinfo
```

**Expected**:
```json
{
  "connected_peers": 0,
  "max_outbound": 8,
  "peers": []
}
```

### Test 2: Multiple Peers
```bash
# Start 3 nodes: 1 hub + 2 spokes
rm -rf /tmp/hub /tmp/spoke1 /tmp/spoke2

# Hub
./build/dinerod --regtest --rpcport=20101 --port=30101 --datadir=/tmp/hub -daemon
sleep 3

# Spoke 1
./build/dinerod --regtest --rpcport=20102 --port=30102 --datadir=/tmp/spoke1 --addnode=127.0.0.1:30101 -daemon
sleep 3

# Spoke 2
./build/dinerod --regtest --rpcport=20103 --port=30103 --datadir=/tmp/spoke2 --addnode=127.0.0.1:30101 -daemon
sleep 5

# Hub should see 2 peers
./build/dinero-cli -rpcport=20101 p2p.getpeerinfo | jq '.connected_peers'
```

**Expected**: `2`

### Test 3: Peer Disconnection
```bash
# Kill Spoke 1
pkill -f "datadir=/tmp/spoke1"
sleep 3

# Hub should now see 1 peer
./build/dinero-cli -rpcport=20101 p2p.getpeerinfo | jq '.connected_peers'
```

**Expected**: `1`

### Pass Criteria
- ✅ Isolated node reports 0 peers
- ✅ Hub with 2 spokes reports 2 peers
- ✅ After disconnection, peer count updates
- ✅ Peer addresses are correct

---

## 4️⃣ WebSocket Real-Time Events Test (40 min)

### Goal
Verify WebSocket server broadcasts events in real-time.

### Setup WebSocket Listener

Create `test_ws_events.js`:
```javascript
const WebSocket = require('ws');
const fs = require('fs');

// Read cookie
const cookie = fs.readFileSync('/tmp/node1/.cookie', 'utf8').trim();
const auth = Buffer.from(`__cookie__:${cookie}`).toString('base64');

// Connect with auth
const ws = new WebSocket('ws://localhost:20001', {
  headers: {
    'Authorization': `Basic ${auth}`
  }
});

ws.on('open', () => {
  console.log('✅ WebSocket connected');

  // Subscribe to block events
  ws.send(JSON.stringify({
    jsonrpc: '2.0',
    method: 'subscribe',
    params: ['block'],
    id: 1
  }));
});

ws.on('message', (data) => {
  console.log('📨 Event received:', data.toString());
});

ws.on('error', (err) => {
  console.error('❌ Error:', err.message);
});

ws.on('close', () => {
  console.log('🔌 WebSocket closed');
});
```

### Test 1: Block Event
```bash
# Terminal 1: Start WebSocket listener
node test_ws_events.js

# Terminal 2: Mine a block
./build/dinero-cli -rpcport=20001 generatetoaddress 1 din1q7gs8mgsnzmw3ur4wtt7snknhedzz5rx5xdvn94

# Terminal 1 should receive:
# 📨 Event received: {"jsonrpc":"2.0","method":"block.new","params":{"height":1,"hash":"..."}}
```

### Test 2: Transaction Event
```bash
# Terminal 2: Send a transaction (if wallet has funds)
./build/dinero-cli -rpcport=20001 sendtoaddress din1qtest 10.0

# Terminal 1 should receive:
# 📨 Event received: {"jsonrpc":"2.0","method":"tx.new","params":{"txid":"..."}}
```

### Test 3: Peer Event
```bash
# Terminal 2: Connect new peer
./build/dinerod --regtest --rpcport=20999 --port=30999 --datadir=/tmp/new-peer --addnode=127.0.0.1:30001 -daemon

# Terminal 1 should receive:
# 📨 Event received: {"jsonrpc":"2.0","method":"peer.connected","params":{"addr":"127.0.0.1:30999"}}
```

### Pass Criteria
- ✅ WebSocket connection established with auth
- ✅ Block event received within 1 second of block creation
- ✅ Transaction event received (if applicable)
- ✅ Peer event received when new peer connects
- ✅ Events have correct JSON-RPC 2.0 format

---

## 5️⃣ Multi-Node Block Propagation Test (60 min)

### Goal
Verify blocks mined on one node propagate to peers.

### Setup 3-Node Network

```bash
# Clean slate
pkill -9 dinerod
rm -rf /tmp/miner /tmp/relay /tmp/observer

# Node 1: Miner
./build/dinerod --regtest --rpcport=21001 --port=31001 --datadir=/tmp/miner -daemon
sleep 3

# Node 2: Relay (connects to Miner)
./build/dinerod --regtest --rpcport=21002 --port=31002 --datadir=/tmp/relay --addnode=127.0.0.1:31001 -daemon
sleep 3

# Node 3: Observer (connects to Relay)
./build/dinerod --regtest --rpcport=21003 --port=31003 --datadir=/tmp/observer --addnode=127.0.0.1:31002 -daemon
sleep 5
```

### Test: Mine and Propagate

```bash
echo "=== Initial Heights ==="
echo -n "Miner:    " && ./build/dinero-cli -rpcport=21001 getblockcount
echo -n "Relay:    " && ./build/dinero-cli -rpcport=21002 getblockcount
echo -n "Observer: " && ./build/dinero-cli -rpcport=21003 getblockcount

echo ""
echo "=== Mining 10 blocks on Miner ==="
./build/dinero-cli -rpcport=21001 generatetoaddress 10 din1q7gs8mgsnzmw3ur4wtt7snknhedzz5rx5xdvn94

sleep 5

echo ""
echo "=== Heights After Mining ==="
echo -n "Miner:    " && ./build/dinero-cli -rpcport=21001 getblockcount
echo -n "Relay:    " && ./build/dinero-cli -rpcport=21002 getblockcount
echo -n "Observer: " && ./build/dinero-cli -rpcport=21003 getblockcount

echo ""
echo "=== Best Block Hashes (should match) ==="
echo -n "Miner:    " && ./build/dinero-cli -rpcport=21001 getbestblockhash
echo -n "Relay:    " && ./build/dinero-cli -rpcport=21002 getbestblockhash
echo -n "Observer: " && ./build/dinero-cli -rpcport=21003 getbestblockhash
```

### Expected Output

```
=== Initial Heights ===
Miner:    0
Relay:    0
Observer: 0

=== Mining 10 blocks on Miner ===
[mining output]

=== Heights After Mining ===
Miner:    10
Relay:    10
Observer: 10

=== Best Block Hashes (should match) ===
Miner:    abc123...
Relay:    abc123...
Observer: abc123...
```

### Pass Criteria
- ✅ All 3 nodes reach same height (10)
- ✅ All 3 nodes have same best block hash
- ✅ Propagation completes within 5 seconds
- ✅ No orphan blocks or chain splits

---

## 6️⃣ Performance Benchmarking (30 min)

### Goal
Measure P2P handshake latency and WebSocket event delivery time.

### Benchmark 1: Handshake Latency

```bash
#!/bin/bash
# test_handshake_latency.sh

echo "=== P2P Handshake Latency Benchmark ==="
echo ""

for i in {1..10}; do
  rm -rf /tmp/bench-node1 /tmp/bench-node2

  # Start listening node
  ./build/dinerod --regtest --rpcport=22001 --port=32001 --datadir=/tmp/bench-node1 -daemon > /dev/null 2>&1
  sleep 2

  # Measure connection time
  START=$(date +%s%N)
  ./build/dinerod --regtest --rpcport=22002 --port=32002 --datadir=/tmp/bench-node2 --addnode=127.0.0.1:32001 -daemon > /dev/null 2>&1

  # Wait for connection
  while [ $(./build/dinero-cli -rpcport=22001 p2p.getpeerinfo 2>/dev/null | jq -r '.connected_peers // 0') -eq 0 ]; do
    sleep 0.1
  done

  END=$(date +%s%N)
  LATENCY=$(( (END - START) / 1000000 ))  # Convert to milliseconds

  echo "Run $i: ${LATENCY}ms"

  # Cleanup
  pkill -f "datadir=/tmp/bench"
  sleep 1
done
```

**Expected**: <500ms average handshake latency

### Benchmark 2: WebSocket Event Delivery

```bash
#!/bin/bash
# test_ws_event_latency.sh

echo "=== WebSocket Event Latency Benchmark ==="
echo ""

# Start node
rm -rf /tmp/ws-bench
./build/dinerod --regtest --rpcport=23001 --datadir=/tmp/ws-bench -daemon
sleep 3

# Start WebSocket listener in background
node test_ws_events.js > /tmp/ws-events.log 2>&1 &
WS_PID=$!
sleep 2

# Mine 10 blocks and measure event delivery time
for i in {1..10}; do
  START=$(date +%s%N)
  ./build/dinero-cli -rpcport=23001 generatetoaddress 1 din1q7gs8mgsnzmw3ur4wtt7snknhedzz5rx5xdvn94 > /dev/null

  # Wait for event in log
  while ! tail -1 /tmp/ws-events.log | grep -q "block.new"; do
    sleep 0.01
  done

  END=$(date +%s%N)
  LATENCY=$(( (END - START) / 1000000 ))

  echo "Block $i event latency: ${LATENCY}ms"
done

# Cleanup
kill $WS_PID
pkill -f "datadir=/tmp/ws-bench"
```

**Expected**: <100ms average event delivery latency

### Pass Criteria
- ✅ Handshake latency <500ms (target: <200ms)
- ✅ WebSocket event latency <100ms (target: <50ms)
- ✅ Consistent performance across 10 runs (no outliers >2x avg)

---

## 📊 Test Results Template

### Test Execution Summary

| Test Suite | Status | Pass/Fail | Notes |
|------------|--------|-----------|-------|
| P2P Handshake | ⬜ | ⬜ / ⬜ | |
| WebSocket Auth | ⬜ | ⬜ / ⬜ | |
| Peer Info RPC | ⬜ | ⬜ / ⬜ | |
| WebSocket Events | ⬜ | ⬜ / ⬜ | |
| Block Propagation | ⬜ | ⬜ / ⬜ | |
| Performance Bench | ⬜ | ⬜ / ⬜ | |

### Performance Metrics

```
P2P Handshake Latency:
  Average: ___ms
  Min:     ___ms
  Max:     ___ms

WebSocket Event Latency:
  Average: ___ms
  Min:     ___ms
  Max:     ___ms

Block Propagation:
  3-node propagation time: ___s
```

### Issues Found

```
1. [Issue Description]
   - Severity: Critical / High / Medium / Low
   - Reproduction: [Steps]
   - Fix: [Solution or workaround]

2. [Issue Description]
   ...
```

---

## 🚀 Production Deployment Checklist

After all tests pass, prepare for production deployment:

### 1. Configuration
- [ ] Set proper `--datadir` (not /tmp)
- [ ] Configure firewall rules for P2P port (default: 20998)
- [ ] Set `--maxconnections` limit (default: 125)
- [ ] Enable `--debug=net,p2p` for initial monitoring
- [ ] Set up log rotation (debug.log can grow large)

### 2. Monitoring
- [ ] Set up Grafana dashboard for peer count
- [ ] Alert if peer count drops below threshold (e.g., <3 peers)
- [ ] Monitor WebSocket connection count
- [ ] Track block propagation time

### 3. Security
- [ ] Ensure `.cookie` file has proper permissions (600)
- [ ] Restrict RPC/WebSocket access to localhost or VPN
- [ ] Use reverse proxy (nginx) for public WebSocket endpoints
- [ ] Rate limit WebSocket connections

### 4. Documentation
- [ ] Update operational runbooks
- [ ] Document common troubleshooting steps
- [ ] Create backup/recovery procedures

---

## 🎯 Success Criteria

Week 7 Day 2 is successful when:

1. ✅ All 6 test suites pass
2. ✅ P2P handshake <500ms average
3. ✅ WebSocket events <100ms latency
4. ✅ 3-node network syncs correctly
5. ✅ No critical issues found
6. ✅ Production deployment checklist complete

---

## 📚 Additional Resources

### Debugging Commands
```bash
# Check if daemon is running
ps aux | grep dinerod

# Check listening ports
lsof -i :20998

# Watch debug log in real-time
tail -f ~/.dinero/debug.log | grep -E "version|verack|block|peer"

# Check peer count
./dinero-cli p2p.getpeerinfo | jq '.connected_peers'

# Test RPC connectivity
./dinero-cli getblockcount
```

### Useful Scripts

**kill_all_test_nodes.sh**:
```bash
#!/bin/bash
pkill -9 dinerod
rm -rf /tmp/node* /tmp/hub /tmp/spoke* /tmp/miner /tmp/relay /tmp/observer
echo "✅ All test nodes killed and cleaned"
```

**start_test_network.sh**:
```bash
#!/bin/bash
# Start 3-node test network
./build/dinerod --regtest --rpcport=20001 --port=30001 --datadir=/tmp/node1 -daemon
sleep 2
./build/dinerod --regtest --rpcport=20002 --port=30002 --datadir=/tmp/node2 --addnode=127.0.0.1:30001 -daemon
sleep 2
./build/dinerod --regtest --rpcport=20003 --port=30003 --datadir=/tmp/node3 --addnode=127.0.0.1:30001 -daemon
sleep 3
echo "✅ 3-node test network started"
```

---

**Test Plan Created**: 2025-11-06
**Estimated Time**: 3-4 hours
**Status**: 🟡 **Ready for Execution**

---

*"Integration tests are where theory meets reality. Run them thoroughly."*
— Week 7 Day 2 Principle
