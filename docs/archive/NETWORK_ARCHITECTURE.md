# 🌐 Dinero Network Architecture

**Production Setup: Mac (Mining + Wallet) ↔ Ubuntu Server (Public Seed Node)**

---

## 🎯 Network Topology

```
┌─────────────────────────────────────────────────────────────┐
│                     Internet / Public                        │
│                                                              │
│   Other Nodes → 96.9.226.98:20999 (P2P Open)               │
│                       ↑                                      │
│                       │                                      │
└───────────────────────┼──────────────────────────────────────┘
                        │
                        │ P2P Connection
                        │
        ┌───────────────┴──────────────────┐
        │                                  │
        │   Ubuntu Seed Node               │
        │   96.9.226.98                    │
        │                                  │
        │   ✅ P2P: :20999 (OPEN)          │
        │   ❌ RPC: :20998 (LOCALHOST)     │
        │   🚫 No wallet keys              │
        │   ✅ Always-on full node         │
        │                                  │
        └───────────────┬──────────────────┘
                        │
                        │ P2P Peering
                        │ (-addnode)
                        │
        ┌───────────────┴──────────────────┐
        │                                  │
        │   Mac (Your Workstation)         │
        │   Local Network                  │
        │                                  │
        │   ✅ RPC: 127.0.0.1:20998        │
        │   🔐 Wallet + Private Keys       │
        │   ⛏️  Mining (dinero-miner)       │
        │   🖥️  GUI (dinero-qt)             │
        │                                  │
        └──────────────────────────────────┘
```

---

## 🖥️ Mac (Workstation) - Mining + Wallet Node

### **Role:**
- **Hot wallet** with private keys
- **Mining** to your own addresses
- **GUI** for wallet management
- **Local full node** for validation

### **What Runs:**
1. **dinerod** (daemon)
   ```bash
   ./build-clean/dinerod \
     -datadir=./data-main \
     -addnode=96.9.226.98:20999 \
     -listen=1 \
     -rpcbind=127.0.0.1
   ```

2. **dinero-miner** (CPU miner)
   ```bash
   ./build-clean/dinero-miner \
     --rpc=http://127.0.0.1:20998/ \
     --datadir=./data-main \
     --address=din1q... \
     --threads=8
   ```

3. **dinero-qt** (GUI wallet)
   ```bash
   ./build-gui/dinero-qt \
     -datadir=./data-main
   ```

### **Security:**
- ✅ RPC on **127.0.0.1 only**
- ✅ Firewall blocks incoming P2P (optional)
- ✅ Private keys **ONLY on Mac**
- ✅ Encrypted wallet with BIP-39 seed

### **Network:**
- Peers with Ubuntu server
- Can sleep/wake without breaking network
- Mines blocks → propagates via server

---

## 🌍 Ubuntu Server - Public Seed Node

### **Role:**
- **Always-on** public full node
- **Seed node** for network discovery
- **Fast block relay** to other peers
- **Infrastructure backbone**

### **What Runs:**
```bash
# Systemd service (production)
sudo systemctl start dinerod

# Equivalent to:
/usr/local/bin/dinerod \
  -datadir=/var/lib/dinero \
  -listen=1 \
  -port=20999 \
  -rpcbind=127.0.0.1 \
  -rpcallowip=127.0.0.1 \
  -maxconnections=64
```

### **Security:**
- ✅ Port **20999 OPEN** (P2P)
- ❌ Port **20998 BLOCKED** (RPC localhost only)
- 🚫 **NO wallet keys**
- 🚫 **NO mining** (optional)
- ✅ Dedicated `dinero` user (unprivileged)
- ✅ Systemd hardening (NoNewPrivileges, PrivateTmp)

### **Firewall:**
```bash
sudo ufw allow 20999/tcp   # P2P
sudo ufw deny 20998/tcp    # RPC (already localhost-only)
```

### **Monitoring:**
```bash
# Check status
sudo systemctl status dinerod

# View logs
sudo journalctl -u dinerod -f

# Check peers
sudo -u dinero dinero-cli -datadir=/var/lib/dinero getpeerinfo

# Check height
sudo -u dinero dinero-cli -datadir=/var/lib/dinero getblockcount
```

---

## 🔄 How Blocks Flow

### **Mining on Mac:**
1. Mac miner finds block → sends to Mac daemon (127.0.0.1:20998)
2. Mac daemon validates → adds to chain
3. Mac daemon announces block to peers (including Ubuntu server)
4. Ubuntu server receives block → validates → adds to chain
5. Ubuntu server announces to OTHER peers (public network)
6. Block propagates across entire network

### **Syncing New Nodes:**
1. New node starts with `-addnode=96.9.226.98:20999`
2. Connects to Ubuntu server
3. Downloads full blockchain from server
4. Stays in sync via P2P gossip

---

## 📊 Recommended Setup

### **For Development/Testing:**
- ✅ Mac: daemon + wallet + miner + GUI
- ✅ Ubuntu: seed node (optional)

### **For Mainnet Launch:**
- ✅ Mac: wallet + miner (personal use)
- ✅ Ubuntu Server 1: public seed (96.9.226.98)
- ✅ Ubuntu Server 2: backup seed (different region)
- ✅ DNS: `seed1.dinero.org` → Server 1
- ✅ DNS: `seed2.dinero.org` → Server 2

### **Optional Services on Ubuntu:**
- **Block Explorer** (reads RPC on 127.0.0.1:20998)
- **Stratum Bridge** (for pool mining)
- **API Server** (blockchain data API)
- **Metrics** (Prometheus/Grafana)

---

## 🔐 Security Best Practices

### **Mac (Hot Wallet):**
1. ✅ Encrypt wallet with strong password
2. ✅ Backup BIP-39 seed phrase (paper, offline)
3. ✅ Never expose RPC to internet
4. ✅ Firewall blocks incoming connections
5. ✅ Regular backups of wallet.db

### **Ubuntu Server (Public Node):**
1. 🚫 **NEVER store wallet keys**
2. 🚫 **NEVER mine to server addresses**
3. ✅ Run as unprivileged `dinero` user
4. ✅ Firewall: only 20999/tcp open
5. ✅ Regular security updates
6. ✅ Monitoring + alerts
7. ✅ SSH key auth only (no passwords)
8. ✅ Fail2ban for SSH protection

---

## 🚀 Quick Start Commands

### **On Mac (Local Mining):**
```bash
cd /Users/haydarevich/Documents/DineroCoin

# Start daemon (if not running)
./start-node.sh

# Start GUI
./build-gui/dinero-qt -datadir=./data-main

# Or mine from terminal
./build-clean/dinero-miner \
  --rpc=http://127.0.0.1:20998/ \
  --datadir=./data-main \
  --address=$(cat ./data-main/mining-address.txt) \
  --threads=8
```

### **On Ubuntu Server (Setup):**
```bash
# On your Mac, transfer the setup script
scp deploy/ubuntu-seed-node-setup.sh root@96.9.226.98:/tmp/

# SSH to server
ssh root@96.9.226.98

# Run setup
cd /root/DineroCoin
bash /tmp/ubuntu-seed-node-setup.sh

# Check status
sudo systemctl status dinerod
sudo journalctl -u dinerod -f
```

---

## 📈 Scaling the Network

### **Phase 1: Dev/Testing (Now)**
- 1 Mac node
- 1 Ubuntu seed (optional)

### **Phase 2: Testnet**
- Multiple dev machines
- 2-3 public seeds
- Community testers

### **Phase 3: Mainnet**
- 5+ public seeds (different regions)
- DNS seeds (`seed1.dinero.org`, etc.)
- Block explorers
- Mining pools (Stratum)
- Public API servers

---

## 🎯 Current Status

**Your Setup:**
- ✅ Mac daemon: Running, connected to server
- ✅ Ubuntu server: Peered with Mac
- ✅ Genesis block: Synced (height 0)
- ✅ GUI: Connected and working
- ⏳ Next step: **START MINING!** ⛏️

**Ready to mine Block 1!** 🚀

