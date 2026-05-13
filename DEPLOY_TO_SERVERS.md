# Deploy Recent Updates to Linux Servers

## 🚨 What's New in Last 48 Hours

### Major Additions

1. **HTTP Server Integration** (`src/httprpc.cpp`)
   - Full HTTP server alongside RPC
   - `/serverinfo.json` endpoint
   - CORS support
   - Health check endpoint

2. **Global Node Registry** (entire `registry/` directory)
   - Python-based registry server
   - Self-registration support
   - Web dashboard
   - REST API

3. **Configuration Files**
   - Enhanced `dinero.conf` examples
   - Systemd service files
   - Nginx configurations

### Files Modified/Added

```
DineroCoin/
├── src/
│   └── httprpc.cpp                    # HTTP server (MODIFIED)
├── registry/                          # NEW DIRECTORY
│   ├── dinero_registry.py
│   ├── dinero_registry_extended.py
│   ├── daemon_integration_example.cpp
│   ├── test_registration.sh
│   ├── dinero-registry.service
│   ├── Dockerfile
│   ├── docker-compose.yml
│   ├── nginx.conf.example
│   ├── README.md
│   ├── QUICKSTART.md
│   ├── ARCHITECTURE.md
│   ├── IMPLEMENTATION_SUMMARY.md
│   └── INDEX.md
└── doc/
    └── serverinfo-setup.md            # NEW
```

## 📋 Deployment Checklist

### Server 1: Virginia (173.249.195.59)
- [ ] Update daemon code (rebuild)
- [ ] Update dinero.conf
- [ ] Deploy registry server
- [ ] Configure nginx
- [ ] Test endpoints

### Server 2: California (172.93.160.131)
- [ ] Update daemon code (rebuild)
- [ ] Update dinero.conf
- [ ] Test endpoints
- [ ] Register with Virginia registry

### Server 3: Frankfurt (if applicable)
- [ ] Update daemon code (rebuild)
- [ ] Update dinero.conf
- [ ] Test endpoints
- [ ] Register with Virginia registry

## 🚀 Quick Deployment Script

Save this as `deploy_updates.sh` and run on each server:

```bash
#!/bin/bash
set -e

echo "======================================"
echo "Dinero Server Update Script"
echo "======================================"

# Configuration
REPO_DIR="$HOME/DineroCoin"
SERVER_NAME="${1:-Unknown}"
SERVER_IP="${2:-auto}"

# Auto-detect IP if not provided
if [ "$SERVER_IP" = "auto" ]; then
    SERVER_IP=$(curl -s ifconfig.me)
fi

echo "Server: $SERVER_NAME"
echo "IP: $SERVER_IP"
echo ""

# Step 1: Backup current installation
echo "📦 Step 1: Backing up current installation..."
BACKUP_DIR="$HOME/dinero_backup_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$BACKUP_DIR"
cp -r "$REPO_DIR" "$BACKUP_DIR/" || echo "Warning: Could not backup (first install?)"

# Step 2: Pull latest code
echo "📥 Step 2: Pulling latest code..."
cd "$REPO_DIR"
git stash || true
git pull origin master || {
    echo "❌ Git pull failed. Check network/credentials."
    exit 1
}

# Step 3: Rebuild daemon
echo "🔨 Step 3: Rebuilding daemon..."
cd "$REPO_DIR"
make clean || true
./autogen.sh
./configure
make -j$(nproc)

# Step 4: Stop running daemon
echo "🛑 Step 4: Stopping current daemon..."
./build/dinero-cli stop || true
sleep 5

# Step 5: Update dinero.conf
echo "⚙️  Step 5: Updating dinero.conf..."
CONF_FILE="$HOME/.dinero/dinero.conf"

# Backup existing conf
if [ -f "$CONF_FILE" ]; then
    cp "$CONF_FILE" "$CONF_FILE.backup.$(date +%Y%m%d)"
fi

# Add new settings if not present
cat >> "$CONF_FILE" <<'EOF'

# === HTTP Server Settings (Added $(date +%Y-%m-%d)) ===
httpserver=1
httpserverport=21999
httpcors=*

# === Server Info Settings ===
serverinfo.enabled=1
serverinfo.name=$SERVER_NAME
serverinfo.network=mainnet

# === Registry Settings (Optional) ===
# registry=1
# registryurl=https://status.dinero-coin.com/api/register
# externalip=$SERVER_IP

EOF

echo "✅ Config updated (backup at $CONF_FILE.backup.*)"

# Step 6: Restart daemon
echo "🚀 Step 6: Starting updated daemon..."
cd "$REPO_DIR"
./build/dinerod -daemon

echo ""
echo "⏳ Waiting for daemon to start..."
sleep 10

# Step 7: Test new endpoints
echo "🧪 Step 7: Testing new endpoints..."
echo ""

echo "Test 1: RPC still works"
./build/dinero-cli getblockcount || echo "⚠️  RPC test failed"

echo ""
echo "Test 2: HTTP server"
curl -s http://localhost:21999/ | head -5 || echo "⚠️  HTTP test failed"

echo ""
echo "Test 3: serverinfo.json"
curl -s http://localhost:21999/serverinfo.json || echo "⚠️  serverinfo test failed"

echo ""
echo "Test 4: Check from external IP"
curl -s http://$SERVER_IP:21999/serverinfo.json || echo "⚠️  External access test failed (check firewall)"

echo ""
echo "======================================"
echo "✅ Deployment Complete!"
echo "======================================"
echo ""
echo "Next steps:"
echo "1. Verify daemon is running: ./build/dinero-cli getinfo"
echo "2. Check logs: tail -f ~/.dinero/debug.log"
echo "3. Test HTTP: curl http://$SERVER_IP:21999/serverinfo.json"
echo ""

# Step 8: Display current status
echo "📊 Current Status:"
./build/dinero-cli getinfo 2>/dev/null || echo "Daemon starting..."
```

## 🔧 Manual Deployment Steps

### On ALL Servers (Virginia, California, Frankfurt)

#### 1. Pull Latest Code

```bash
cd ~/DineroCoin
git stash  # Save any local changes
git pull origin master
```

#### 2. Rebuild Daemon

```bash
make clean
./autogen.sh
./configure
make -j$(nproc)
```

#### 3. Stop Current Daemon

```bash
./build/dinero-cli stop
# Wait 10 seconds
sleep 10
```

#### 4. Update dinero.conf

Edit `~/.dinero/dinero.conf` and add:

**Virginia (173.249.195.59):**
```ini
# HTTP Server
httpserver=1
httpserverport=21999
httpcors=*

# Server Info
serverinfo.enabled=1
serverinfo.name=Virginia
serverinfo.network=mainnet

# Registry (Virginia hosts the registry)
registry=0
externalip=173.249.195.59
```

**California (172.93.160.131):**
```ini
# HTTP Server
httpserver=1
httpserverport=21999
httpcors=*

# Server Info
serverinfo.enabled=1
serverinfo.name=California
serverinfo.network=mainnet

# Registry (register with Virginia)
registry=1
registryurl=http://173.249.195.59:8080/api/register
externalip=172.93.160.131
```

**Frankfurt (if applicable):**
```ini
# HTTP Server
httpserver=1
httpserverport=21999
httpcors=*

# Server Info
serverinfo.enabled=1
serverinfo.name=Frankfurt
serverinfo.network=mainnet

# Registry
registry=1
registryurl=http://173.249.195.59:8080/api/register
externalip=YOUR_FRANKFURT_IP
```

#### 5. Restart Daemon

```bash
./build/dinerod -daemon
```

#### 6. Test Endpoints

```bash
# Wait for startup
sleep 10

# Test RPC
./build/dinero-cli getblockcount

# Test HTTP server (local)
curl http://localhost:21999/serverinfo.json

# Test HTTP server (external)
curl http://YOUR_SERVER_IP:21999/serverinfo.json
```

### Only on Virginia (Registry Host)

#### 7. Deploy Registry Server

```bash
# Navigate to registry directory
cd ~/DineroCoin/registry

# Install Python dependencies
pip3 install requests

# Create systemd service
sudo cp dinero-registry.service /etc/systemd/system/

# Edit service to set correct paths
sudo nano /etc/systemd/system/dinero-registry.service

# Enable and start service
sudo systemctl daemon-reload
sudo systemctl enable dinero-registry
sudo systemctl start dinero-registry

# Check status
sudo systemctl status dinero-registry

# View logs
sudo journalctl -u dinero-registry -f
```

#### 8. Configure Nginx (Optional but Recommended)

```bash
# Copy nginx config
sudo cp nginx.conf.example /etc/nginx/sites-available/dinero-registry

# Edit for your domain
sudo nano /etc/nginx/sites-available/dinero-registry
# Change: status.dinero-coin.com → your domain

# Enable site
sudo ln -s /etc/nginx/sites-available/dinero-registry /etc/nginx/sites-enabled/

# Test config
sudo nginx -t

# Reload nginx
sudo systemctl reload nginx

# Get SSL certificate (if using domain)
sudo certbot --nginx -d status.dinero-coin.com
```

## 🧪 Verification Tests

Run these on each server after deployment:

```bash
#!/bin/bash
# test_deployment.sh

echo "=== Dinero Deployment Tests ==="
echo ""

# Test 1: Daemon running
echo "1. Daemon Status:"
./build/dinero-cli getinfo | head -5
echo ""

# Test 2: HTTP server accessible
echo "2. HTTP Server (local):"
curl -s http://localhost:21999/ | head -3
echo ""

# Test 3: serverinfo.json
echo "3. Server Info Endpoint:"
curl -s http://localhost:21999/serverinfo.json | jq '.'
echo ""

# Test 4: External access (replace with your IP)
SERVER_IP=$(curl -s ifconfig.me)
echo "4. External Access (IP: $SERVER_IP):"
curl -s http://$SERVER_IP:21999/serverinfo.json | jq '.name, .ip, .connections'
echo ""

# Test 5: CORS headers
echo "5. CORS Headers:"
curl -sI http://localhost:21999/serverinfo.json | grep -i "access-control"
echo ""

echo "=== Tests Complete ==="
```

## 🔥 Firewall Configuration

Ensure port 21999 is open on all servers:

```bash
# UFW (Ubuntu/Debian)
sudo ufw allow 21999/tcp
sudo ufw status

# firewalld (CentOS/RHEL)
sudo firewall-cmd --permanent --add-port=21999/tcp
sudo firewall-cmd --reload

# iptables (manual)
sudo iptables -A INPUT -p tcp --dport 21999 -j ACCEPT
sudo iptables-save > /etc/iptables/rules.v4
```

For registry server (Virginia only):
```bash
sudo ufw allow 8080/tcp  # If running registry directly
# OR
sudo ufw allow 443/tcp   # If running behind nginx with SSL
sudo ufw allow 80/tcp    # For Let's Encrypt verification
```

## 📊 What You Should See After Deployment

### On Each Daemon:

```bash
$ curl http://localhost:21999/serverinfo.json
{
  "name": "Virginia",
  "ip": "173.249.195.59",
  "rpc_port": 21999,
  "ws_port": 21000,
  "p2p_port": 20999,
  "network": "mainnet",
  "uptime": 3600,
  "connections": 8,
  "features": ["contracts", "bridge", "token_auth"]
}
```

### On Registry Server (Virginia):

```bash
$ curl http://localhost:8080/api/status
{
  "status": "ok",
  "total_nodes_alive": 2,
  "total_nodes_configured": 3,
  "last_update": "2025-11-03T12:00:00Z"
}

$ curl http://localhost:8080/nodes.json
{
  "timestamp": "2025-11-03T12:00:00Z",
  "total_nodes": 2,
  "nodes": [
    {
      "name": "Virginia",
      "ip": "173.249.195.59",
      ...
    },
    {
      "name": "California",
      "ip": "172.93.160.131",
      ...
    }
  ]
}
```

## 🐛 Troubleshooting

### Issue: HTTP server not responding

```bash
# Check if daemon is running
ps aux | grep dinerod

# Check if port is open
netstat -tulpn | grep 21999

# Check firewall
sudo ufw status

# Check logs
tail -f ~/.dinero/debug.log | grep -i http
```

### Issue: serverinfo.json returns empty or error

```bash
# Check config
cat ~/.dinero/dinero.conf | grep serverinfo

# Test locally first
curl http://localhost:21999/serverinfo.json

# Check daemon logs
tail -100 ~/.dinero/debug.log
```

### Issue: Registry not seeing nodes

```bash
# On registry server
sudo journalctl -u dinero-registry -f

# Test direct connection
curl http://173.249.195.59:21999/serverinfo.json
curl http://172.93.160.131:21999/serverinfo.json

# Check registry config
cat ~/DineroCoin/registry/dinero_registry.py | grep DEFAULT_NODES
```

### Issue: Can't connect externally

```bash
# Check firewall
sudo ufw status
sudo iptables -L -n | grep 21999

# Check if binding to all interfaces
netstat -tulpn | grep 21999
# Should show: 0.0.0.0:21999 (not 127.0.0.1:21999)

# Test from another server
curl http://YOUR_SERVER_IP:21999/serverinfo.json
```

## 📝 Rollback Plan (If Needed)

If something goes wrong:

```bash
# Stop new daemon
./build/dinero-cli stop

# Restore from backup
BACKUP_DIR=$(ls -dt ~/dinero_backup_* | head -1)
echo "Restoring from: $BACKUP_DIR"

# Restore binaries
cd $BACKUP_DIR/DineroCoin
./build/dinerod -daemon

# Restore config
cp ~/.dinero/dinero.conf.backup.* ~/.dinero/dinero.conf

echo "Rollback complete. Check: ./build/dinero-cli getinfo"
```

## ✅ Post-Deployment Checklist

- [ ] All daemons running on all servers
- [ ] HTTP endpoints responding on all servers
- [ ] serverinfo.json accessible externally on all servers
- [ ] Registry server running on Virginia
- [ ] Registry seeing all configured nodes
- [ ] Nginx configured (if using SSL)
- [ ] Firewall rules updated
- [ ] All tests passing
- [ ] Logs looking healthy
- [ ] No error messages

## 📞 Support

If you encounter issues:

1. Check logs: `tail -f ~/.dinero/debug.log`
2. Check registry logs: `sudo journalctl -u dinero-registry -f`
3. Test locally before external access
4. Verify firewall rules
5. Check process is running: `ps aux | grep dinero`

---

**Good luck with the deployment!** 🚀

After successful deployment, you'll have:
- ✅ HTTP server running on all nodes
- ✅ serverinfo.json endpoint accessible
- ✅ Global registry tracking all nodes
- ✅ Web dashboard showing network status
