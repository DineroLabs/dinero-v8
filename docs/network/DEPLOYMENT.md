# 🌐 Dinero Node Deployment Guide (GreenCloud Edition)

This guide explains how to deploy and connect Dinero nodes on GreenCloud VPS instances
so they can participate in the P2P mesh and communicate with miners and wallets.

---

## 1️⃣ Required Ports

| Service     | Port  | Protocol | Direction        | Purpose |
|-------------|-------|----------|------------------|---------|
| **P2P**      | 19003 | TCP       | Inbound/Outbound | Node-to-node block and tx relay |
| **RPC**      | 20998 | TCP       | Inbound (trusted)| CLI & wallet communication |
| **WebSocket**| 21001 | TCP       | Inbound/Outbound | Optional realtime updates for dashboards |

---

## 2️⃣ GreenCloud Firewall Rules

GreenCloud VPS instances use **Security Groups** and **iptables/ufw** internally.

### Option A – Using UFW (Ubuntu)

```bash
sudo apt update
sudo apt install ufw -y

# Enable firewall if not already active
sudo ufw enable

# Allow Dinero P2P and WebSocket ports
sudo ufw allow 19003/tcp
sudo ufw allow 21001/tcp

# Allow RPC only from your trusted admin IP
sudo ufw allow from <your-admin-ip> to any port 20998 proto tcp

# Check rules
sudo ufw status verbose
```

Example output:

```
19003/tcp ALLOW Anywhere
21001/tcp ALLOW Anywhere
20998/tcp ALLOW from 192.168.1.100
```

### Option B – Using GreenCloud Panel

1. Log in to the **GreenCloud VPS Panel**.
2. Select your VPS → **Firewall / Security Group**.
3. Add the following inbound rules:

| Type       | Protocol | Port  | Source        |
|------------|----------|-------|---------------|
| Custom TCP | TCP      | 19003 | 0.0.0.0/0     |
| Custom TCP | TCP      | 21001 | 0.0.0.0/0     |
| Custom TCP | TCP      | 20998 | (your admin IP only) |

4. Save and apply the rules.
5. Wait 1–2 minutes for propagation.

---

## 3️⃣ Verify Network Accessibility

Run these tests from your local machine or another node:

### Using nc (Netcat)

```bash
nc -vz <greencloud-ip> 19003
nc -vz <greencloud-ip> 21001
nc -vz <greencloud-ip> 20998
```

Expected output:

```
Connection to <ip> port 19003 [tcp/*] succeeded!
```

### Using the Dinero test script

```bash
python3 scripts/test_dinero_ports.py
```

✅ **OPEN** means the port is reachable.
❌ **CLOSED / FILTERED** means firewall or daemon not listening.

---

## 4️⃣ Dinero Configuration Example

Edit `/root/.dinero/dinero.conf` (or your custom data directory):

```conf
listen=1
bind=0.0.0.0
port=19003

rpcbind=0.0.0.0
rpcallowip=127.0.0.1
rpcallowip=<your-admin-ip>
rpcport=20998

wsport=21001
maxconnections=64
```

Restart the daemon:

```bash
systemctl restart dinerod
# or if run manually:
pkill dinerod
./dinerod -datadir=/root/.dinero -debug=1 -printtoconsole
```

You should see:

```
[P2P] Listening on port 19003
[RPC] Listening on port 20998
[WS ] Listening on port 21001
```

---

## 5️⃣ Health Check Commands

From your local machine:

```bash
python3 scripts/test_dinero_ports.py
```

From the server:

```bash
./dinero-cli getconnectioncount
./dinero-cli getpeerinfo
```

✅ At least one peer and active port listeners confirm the node is fully reachable.

---

## 6️⃣ Troubleshooting

| Symptom | Possible Cause | Fix |
|---------|---------------|-----|
| Ports show CLOSED | Firewall or ufw blocking | `sudo ufw allow 19003/tcp` |
| getpeerinfo empty | No peers or closed ports | Open 19003 + restart daemon |
| Miner can't connect | RPC not reachable | Allow 20998 from miner's IP |
| Handshakes time out | Wrong IP in addnode | Use public IP, not private LAN address |
| All local peers | --nohardseeds flag still active | Remove for production nodes |

✅ Once you complete these steps, your GreenCloud node will accept peers,
participate in block relay, and serve RPC/WebSocket connections for wallets and miners.

---

## 7️⃣ Multi-Node Mesh Setup (Inter-Datacenter Peering)

This section shows how to link several Dinero daemons into a redundant mesh so that
each node relays blocks and transactions across the network without relying on a
central seed.

### 🗺️ Topology Example

```
          ┌────────────┐
          │ Mac Node   │  (Home / Dev)
          │ 127.0.0.1  │
          └─────┬──────┘
                │
                ▼
┌────────────┐        ┌────────────┐
│ CA Server  │◀──────▶│ VA Server  │
│172.93.160.131│      │173.249.195.59│
└────────────┘        └────────────┘
        ▲                   ▲
        │                   │
        └────── future regional nodes (EU, AS, etc.)
```

- Each node keeps at least two outbound and one inbound peers.
- The network remains live even if one node goes offline.

### ⚙️ Step 1 – Enable Listening and Open Ports

On all GreenCloud servers:

```conf
# in /root/.dinero/dinero.conf
listen=1
bind=0.0.0.0
port=19003
maxconnections=64
```

Make sure port 19003 is open (see firewall rules above).

### ⚙️ Step 2 – Configure Static Peers

Edit each node's `dinero.conf` so every node knows at least one neighbor.

**California Node:**

```conf
addnode=173.249.195.59:19003     # Virginia
addnode=<your-mac-public-ip>:19003   # Optional home node
```

**Virginia Node:**

```conf
addnode=172.93.160.131:19003     # California
addnode=<your-mac-public-ip>:19003   # Optional home node
```

**Mac Node (Local/Dev):**

```conf
addnode=172.93.160.131:19003     # California
addnode=173.249.195.59:19003     # Virginia
listen=0                         # (behind NAT)
```

The Mac node operates outbound-only (`listen=0`), while datacenter nodes accept inbound
connections.

### ⚙️ Step 3 – Restart Daemons

```bash
systemctl restart dinerod
# or manual
pkill dinerod
./dinerod -datadir=/root/.dinero -printtoconsole -debug=p2p
```

Within 5–10 seconds logs should show:

```
[HS] Starting handshake with 173.249.195.59:19003
[HS] ✅ Handshake completed
[P2P] Peer connected: 173.249.195.59:19003
```

### ⚙️ Step 4 – Verify Mesh Connectivity

Run from any node:

```bash
./dinero-cli getconnectioncount
./dinero-cli getpeerinfo | grep addr
```

You should see both datacenter IPs and (if online) your Mac's public IP.

Check block relay:

```bash
./dinero-cli getblockcount
```

Values should match across all nodes within ±1 block.

### ⚙️ Step 5 – Optional DNS Seed or Bootstrap List

To simplify onboarding for new peers later, create a DNS record such as:

```
A  seed1.dinero-coin.com  → 172.93.160.131
A  seed2.dinero-coin.com  → 173.249.195.59
```

Then add to `chainparams.cpp`:

```cpp
vSeeds.emplace_back("seed1.dinero-coin.com");
vSeeds.emplace_back("seed2.dinero-coin.com");
```

New wallets or daemons will discover the network automatically.

### ⚙️ Step 6 – Health Check and Monitoring

Every 10 minutes:

```bash
./dinero-cli getpeerinfo | grep "version\|addr"
```

For automated alerts:
- Add a simple cron job to check `getconnectioncount < 1` → restart daemon.
- Use `python3 scripts/test_dinero_ports.py` from your workstation weekly
  to confirm ports remain reachable.

### ✅ Mesh Checklist

| Item | Verified | Notes |
|------|----------|-------|
| CA ↔ VA handshake | ☐ | verify logs |
| Mac ↔ CA/VA outbound | ☐ | works with listen=0 |
| getpeerinfo shows 2+ peers | ☐ | all nodes connected |
| Ports 19003/21001 open | ☐ | pass test script |
| Chain heights match | ☐ | getblockcount equal |

Once these boxes are checked, your Dinero network is a fully meshed,
self-healing P2P cluster ready for mainnet-scale testing.

---

**This deployment guide completes Layer 3 (Network Access) from the v0.2.0-p2p-stability milestone.**
