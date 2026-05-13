# 🌐 DineroCoin P2P Networking - How Connections Work

## How GUI Discovers Connections

```
GUI (Qt/QML)
    ↓ [RPC call every 5 seconds]
RPC: getpeerinfo
    ↓
Daemon (dinerod)
    ↓ [Query P2P Manager]
P2P Manager
    ↓ [Returns list of connected peers]
    [{address: "172.93.160.131:20999", connected: true, ...}]
    ↓
GUI displays: "Connections: 1"
```

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   DineroCoin Node                       │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  1. RPC Server (Port 20998)                            │
│     └─ GUI/CLI connects here                            │
│                                                         │
│  2. P2P Manager                                         │
│     ├─ Manages peer connections                         │
│     ├─ Listens on port 20999                           │
│     └─ Connects to seed nodes                           │
│                                                         │
│  3. Peer Discovery                                      │
│     ├─ Seed nodes (hardcoded or -addnode)              │
│     ├─ DNS seeds (not yet implemented)                  │
│     └─ Peer exchange (addr messages)                    │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

---

## How P2P Connection Works

### Step 1: Node Starts

```cpp
// From main.cpp:585-593
for (const std::string& seed : config.seed_nodes) {
    std::string address = "172.93.160.131";  // DineroLA
    uint16_t port = 20999;
    p2p_manager->add_seed_node(address, port);
}
```

### Step 2: P2P Manager Connects

```cpp
// From peer_manager_v2.cpp:13-21
void PeerManager::dial_next() {
    auto peer = std::make_shared<Peer>(io_, host, port);
    peer->start();  // Initiates TCP connection
}
```

### Step 3: TCP Handshake

```
Your Node                     DineroLA (172.93.160.131:20999)
    │                                    │
    ├─────── TCP SYN ──────────────────>│
    │<────── TCP SYN-ACK ───────────────┤
    ├─────── TCP ACK ──────────────────>│
    │                                    │
    ├─────── VERSION message ──────────>│
    │<────── VERSION message ────────────┤
    ├─────── VERACK message ───────────>│
    │<────── VERACK message ─────────────┤
    │                                    │
    │    ✅ Connection established!      │
```

### Step 4: Message Exchange

```
Your Node                     DineroLA
    │                            │
    ├─── getheaders ────────────>│
    │<─── headers ────────────────┤
    │                            │
    ├─── getblocks ─────────────>│
    │<─── inv (block) ────────────┤
    │                            │
    ├─── getdata (block) ───────>│
    │<─── block ──────────────────┤
    │                            │
    │   🔄 Blockchain syncs!     │
```

---

## How to Connect to DineroLA

### Your Servers:
- **DineroLA:** `172.93.160.131:20999` (Main, most powerful)
- **DineroCA:** `96.9.226.98:20999`
- **DineroVA:** `173.249.195.59:20999`

### Method 1: Command Line (Temporary)

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Connect to DineroLA
./build/dinerod \
  -datadir=./data \
  -testnet \
  -rpcport=20998 \
  -addnode=172.93.160.131:20999 \
  -addnode=96.9.226.98:20999 \
  -addnode=173.249.195.59:20999
```

### Method 2: Config File (Permanent)

Create `./data/dinero.conf`:

```ini
# Network
testnet=1
port=20999

# RPC
rpcport=20998
rpcallowip=127.0.0.1

# Seed nodes (your servers)
addnode=172.93.160.131:20999  # DineroLA (main)
addnode=96.9.226.98:20999     # DineroCA
addnode=173.249.195.59:20999  # DineroVA
```

Then run:
```bash
./build/dinerod -datadir=./data -testnet
```

### Method 3: Via RPC (Dynamic)

```bash
# Add node while daemon is running
curl -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  --cookie ./data/.cookie \
  -d '{"method":"addnode","params":["172.93.160.131:20999","add"]}'
```

---

## Checking Connections

### Via GUI:
```
Overview tab → Connections: X
```

### Via RPC:
```bash
curl -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  --cookie ./data/.cookie \
  -d '{"method":"getpeerinfo"}'
```

**Expected result:**
```json
{
  "result": [
    {
      "addr": "172.93.160.131:20999",
      "connected": true,
      "inbound": false,
      "user_agent": "Dinero:0.1.0",
      "their_height": 1,
      "synced_blocks": 1
    },
    {
      "addr": "96.9.226.98:20999",
      "connected": true,
      ...
    }
  ]
}
```

---

## Connection Flow Diagram

```
┌─────────────────────────────────────────────────────────┐
│          Your Mac (Local Node)                          │
│                                                         │
│  RPC: 127.0.0.1:20998 ←── GUI connects here            │
│  P2P: 0.0.0.0:20999   ←── Peers connect here           │
│                                                         │
│  Seed nodes configured:                                 │
│    - 172.93.160.131:20999 (DineroLA)                   │
│    - 96.9.226.98:20999    (DineroCA)                   │
│    - 173.249.195.59:20999 (DineroVA)                   │
└─────────────────────────────────────────────────────────┘
                    ↓ ↓ ↓
        ┌───────────┘ │ └───────────┐
        ↓              ↓              ↓
┌──────────────┐ ┌──────────────┐ ┌──────────────┐
│  DineroLA    │ │  DineroCA    │ │  DineroVA    │
│  172.93...   │ │  96.9...     │ │  173.249...  │
│  Port 20999  │ │  Port 20999  │ │  Port 20999  │
└──────────────┘ └──────────────┘ └──────────────┘
```

---

## Why Connections = 0 Right Now

### Possible Reasons:

1. **No seed nodes configured**
   ```bash
   # Check if seed nodes are in config
   grep -i addnode ./data/dinero.conf
   ```

2. **Daemon not running on servers**
   ```bash
   # SSH to DineroLA and check
   ssh -i ~/.ssh/dinerola.key root@172.93.160.131
   ps aux | grep dinerod
   ```

3. **Firewall blocking P2P port**
   ```bash
   # Check if port 20999 is listening on server
   ssh -i ~/.ssh/dinerola.key root@172.93.160.131
   netstat -tuln | grep 20999
   ```

4. **Different networks (testnet vs mainnet)**
   - Your local: `testnet`
   - Servers: need to be running `testnet` too!

---

## Quick Fix: Connect to Your Servers NOW

### Step 1: Ensure servers are running in testnet mode

```bash
# SSH to each server and check
ssh -i ~/.ssh/dinerola.key root@172.93.160.131 "ps aux | grep dinerod"

# Should see something like:
# ./build/dinerod -datadir=/root/DineroCoin/data -testnet
```

### Step 2: Restart your local daemon with seed nodes

```bash
cd /Users/haydarevich/Documents/DineroCoin

# Kill existing daemon
pkill dinerod

# Start with your servers as seeds
./build/dinerod \
  -datadir=./data \
  -testnet \
  -rpcport=20998 \
  -addnode=172.93.160.131:20999 \
  -addnode=96.9.226.98:20999 \
  -addnode=173.249.195.59:20999 &

# Wait 5 seconds, then check connections
sleep 5
curl -X POST http://127.0.0.1:20998/ \
  -H "Content-Type: application/json" \
  --cookie ./data/.cookie \
  -d '{"method":"getpeerinfo"}' | jq .
```

### Step 3: Verify in GUI

```bash
# Relaunch GUI
./launch-gui-updated.command

# Check Overview tab → should show "Connections: 3"
```

---

## P2P Message Types

Once connected, nodes exchange these messages:

| Message Type | Purpose |
|-------------|---------|
| `version`   | Initial handshake |
| `verack`    | Acknowledge version |
| `ping/pong` | Keep connection alive |
| `getheaders`| Request block headers |
| `headers`   | Send block headers |
| `getblocks` | Request full blocks |
| `block`     | Send full block data |
| `inv`       | Announce new blocks/txs |
| `getdata`   | Request announced data |
| `tx`        | Send transaction |
| `addr`      | Share peer addresses |

---

## Troubleshooting

### Problem: Connections stay at 0

**Solution 1: Check if P2P port is open**
```bash
# On your Mac
lsof -i :20999
# Should show: dinerod listening on *:20999
```

**Solution 2: Check if servers are reachable**
```bash
# Test connectivity to DineroLA
nc -zv 172.93.160.131 20999
# Expected: "Connection to 172.93.160.131 20999 port [tcp/*] succeeded!"
```

**Solution 3: Check daemon logs**
```bash
# On your Mac
tail -f ./data/debug.log | grep -i "p2p\|peer\|connect"
```

---

## Success Criteria

✅ **Connection established when you see:**
```
[P2P] Dialing 172.93.160.131:20999
[P2P] Peer 172.93.160.131:20999 handshake complete
[P2P] Requested headers from 172.93.160.131:20999
```

✅ **Syncing when you see:**
```
Received 500 headers from peer
Added block 2 (0a1b2c3d...)
Added block 3 (4e5f6a7b...)
```

✅ **GUI shows:**
```
Height: increasing (1, 2, 3, ...)
Connections: 3 (or however many servers are online)
```

---

## Summary

**How GUI gets connections:**
1. GUI calls `getpeerinfo` RPC every 5 seconds
2. Daemon queries P2P Manager
3. P2P Manager returns list of connected peers
4. GUI displays count

**How to connect to DineroLA:**
```bash
# Add this flag when starting daemon:
-addnode=172.93.160.131:20999
```

**Why 0 connections now:**
- No seed nodes configured by default
- Need to explicitly add your servers

**Quick fix:**
```bash
# Restart daemon with seed nodes
./build/dinerod -datadir=./data -testnet -rpcport=20998 \
  -addnode=172.93.160.131:20999 \
  -addnode=96.9.226.98:20999 \
  -addnode=173.249.195.59:20999
```

