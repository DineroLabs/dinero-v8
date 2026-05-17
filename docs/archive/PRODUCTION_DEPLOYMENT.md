# 🚀 Dinero Production Deployment Guide

## ⚠️ Security Requirements for Production

### ✅ **REQUIRED Settings for Public Deployment**

**DO:**
- ✅ Start **WITHOUT** `-dev` flag (enables cookie authentication)
- ✅ Keep RPC bound to localhost: `-rpcbind=127.0.0.1`
- ✅ Use default RPC port or custom: `-rpcport=20998`
- ✅ Forward P2P port only: `20999/tcp` on router
- ✅ Keep `.cookie` file permissions at `0600` (owner read/write only)

**DON'T:**
- ❌ Never use `-dev` in production
- ❌ Never bind RPC to `0.0.0.0` or public IP
- ❌ Never expose RPC port `20998` to internet
- ❌ Never share `.cookie` file contents publicly
- ❌ Never run as root (use dedicated user)

## Production Start Commands

### Mainnet (Production)
```bash
# Start daemon with authentication
./dinerod -datadir=/var/lib/dinero \
          -rpcbind=127.0.0.1 \
          -rpcport=20998 \
          -port=20999 \
          -daemon

# Cookie will be at: /var/lib/dinero/.cookie
```

### Testnet (Testing)
```bash
# Start testnet daemon
./dinerod -datadir=/var/lib/dinero-testnet \
          -rpcbind=127.0.0.1 \
          -rpcport=21998 \
          -port=21999 \
          -testnet \
          -daemon
```

### Development (Local Testing Only)
```bash
# Development mode (NO AUTHENTICATION - LOCAL ONLY)
./dinerod -datadir=./test-data \
          -dev
```

## RPC Access

### With Authentication (Production)
```bash
# Get cookie
COOKIE=$(cat /var/lib/dinero/.cookie)

# Make authenticated RPC call
curl --user "$COOKIE" \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":1,"method":"getinfo","params":[]}' \
     http://127.0.0.1:20998/
```

### Without Authentication (Dev Mode Only)
```bash
# Development mode only - NO COOKIE NEEDED
curl -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":1,"method":"getinfo","params":[]}' \
     http://127.0.0.1:20998/
```

## Network Configuration

### Firewall Rules

**Allow:**
```bash
# P2P port (required for network connectivity)
sudo ufw allow 20999/tcp comment "Dinero P2P"
```

**Block:**
```bash
# RPC port (keep localhost-only)
sudo ufw deny 20998/tcp comment "Dinero RPC - localhost only"
```

### Router Port Forwarding

**Forward:**
- External `20999/tcp` → Internal `<your_machine>:20999` (P2P)

**Do NOT Forward:**
- RPC port `20998` (security risk)

## File Permissions

```bash
# Data directory
chmod 700 /var/lib/dinero

# Cookie file (automatically set by daemon)
chmod 600 /var/lib/dinero/.cookie

# Blockchain data
chmod 600 /var/lib/dinero/blockchain_state.json
chmod 700 /var/lib/dinero/blocks/
```

## Monitoring & Health Checks

### Check Daemon Status
```bash
COOKIE=$(cat /var/lib/dinero/.cookie)

# Get basic info
curl --user "$COOKIE" \
     -d '{"jsonrpc":"2.0","id":1,"method":"getinfo"}' \
     http://127.0.0.1:20998/

# Check connections
curl --user "$COOKIE" \
     -d '{"jsonrpc":"2.0","id":2,"method":"getconnectioncount"}' \
     http://127.0.0.1:20998/

# Check supply/economics
curl --user "$COOKIE" \
     -d '{"jsonrpc":"2.0","id":3,"method":"getsupply"}' \
     http://127.0.0.1:20998/
```

### Log Files
```bash
# Daemon logs (if using systemd)
journalctl -u dinerod -f

# Or check data directory
tail -f /var/lib/dinero/debug.log
```

## Systemd Service (Linux)

```ini
# /etc/systemd/system/dinerod.service
[Unit]
Description=Dinero Cryptocurrency Daemon
After=network.target

[Service]
Type=forking
User=dinero
Group=dinero
ExecStart=/usr/local/bin/dinerod -datadir=/var/lib/dinero \
                                   -rpcbind=127.0.0.1 \
                                   -rpcport=20998 \
                                   -port=20999 \
                                   -daemon
ExecStop=/usr/bin/curl --user $(cat /var/lib/dinero/.cookie) \
                         -d '{"method":"stop"}' \
                         http://127.0.0.1:20998/
Restart=on-failure
RestartSec=10s
LimitNOFILE=8192

[Install]
WantedBy=multi-user.target
```

```bash
# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable dinerod
sudo systemctl start dinerod
sudo systemctl status dinerod
```

## Security Checklist

Before public deployment:

- [ ] RPC bound to `127.0.0.1` only
- [ ] Cookie authentication enabled (no `-dev` flag)
- [ ] `.cookie` file has `0600` permissions
- [ ] RPC port `20998` NOT exposed to internet
- [ ] P2P port `20999` forwarded on router
- [ ] Firewall configured correctly
- [ ] Running as non-root user
- [ ] Data directory permissions: `700`
- [ ] Logs monitoring configured
- [ ] Backup strategy in place

## Multi-Node Setup (Seed Nodes)

### Node 1 (Seed Node)
```bash
./dinerod -datadir=/var/lib/dinero-seed \
          -rpcport=20998 \
          -port=20999 \
          -daemon
```

### Node 2 (Connects to Seed)
```bash
./dinerod -datadir=/var/lib/dinero-node2 \
          -rpcport=21998 \
          -port=21999 \
          -addnode=<seed_node_ip>:20999 \
          -daemon
```

## Backup & Recovery

### Critical Files to Backup
```bash
# Blockchain state
/var/lib/dinero/blockchain_state.json
/var/lib/dinero/blocks/

# Wallet (if using internal wallet)
/var/lib/dinero/wallet/

# Configuration
/var/lib/dinero/dinero.conf
```

### Backup Script
```bash
#!/bin/bash
BACKUP_DIR="/backup/dinero-$(date +%Y%m%d)"
mkdir -p "$BACKUP_DIR"

# Stop daemon
curl --user "$(cat /var/lib/dinero/.cookie)" \
     -d '{"method":"stop"}' http://127.0.0.1:20998/

sleep 5

# Backup
tar -czf "$BACKUP_DIR/dinero-blockchain.tar.gz" /var/lib/dinero/

# Restart daemon
systemctl start dinerod
```

## Emergency Procedures

### Daemon Won't Start
```bash
# Check permissions
ls -la /var/lib/dinero/

# Check port conflicts
lsof -i :20999
lsof -i :20998

# Check logs
tail -100 /var/lib/dinero/debug.log
```

### Corrupted Blockchain
```bash
# Backup current data
mv /var/lib/dinero /var/lib/dinero.backup

# Restart from genesis
./dinerod -datadir=/var/lib/dinero -daemon
```

---

**REMEMBER: Never expose RPC to the internet. Always use cookie authentication in production.**

