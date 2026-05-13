# Dinero Server Quick Reference

## 🖥️ Your Servers

| Server | IP | Role | Ports |
|--------|-----|------|-------|
| **Virginia** | 173.249.195.59 | Main + Registry | RPC: 21999, WS: 21000, P2P: 20999, Registry: 8080 |
| **California** | 172.93.160.131 | Node | RPC: 21999, WS: 21000, P2P: 20999 |
| **Frankfurt** | TBD | Node | RPC: 21999, WS: 21000, P2P: 20999 |

## 🚀 One-Line Deployment

### On Virginia (Main + Registry)
```bash
curl -sSL https://raw.githubusercontent.com/dinerocoin/dinero/master/deploy_updates.sh | bash -s Virginia 173.249.195.59
```

### On California
```bash
curl -sSL https://raw.githubusercontent.com/dinerocoin/dinero/master/deploy_updates.sh | bash -s California 172.93.160.131
```

### On Frankfurt
```bash
curl -sSL https://raw.githubusercontent.com/dinerocoin/dinero/master/deploy_updates.sh | bash -s Frankfurt YOUR_IP
```

## 📋 Manual Deployment Steps

### 1. SSH into Server
```bash
ssh user@173.249.195.59  # Virginia
ssh user@172.93.160.131  # California
```

### 2. Navigate to Repo
```bash
cd ~/DineroCoin
```

### 3. Run Deployment Script
```bash
./deploy_updates.sh <server_name> <server_ip>
```

Example:
```bash
./deploy_updates.sh Virginia 173.249.195.59
./deploy_updates.sh California 172.93.160.131
```

## 🔧 Post-Deployment Verification

### On Each Server

```bash
# 1. Check daemon is running
~/DineroCoin/build/dinero-cli getinfo

# 2. Test HTTP server
curl http://localhost:21999/serverinfo.json

# 3. Test external access
curl http://YOUR_SERVER_IP:21999/serverinfo.json

# 4. Check logs
tail -f ~/.dinero/debug.log
```

### On Virginia (Registry Server)

```bash
# 5. Check registry status
sudo systemctl status dinero-registry

# 6. Test registry API
curl http://localhost:8080/api/status
curl http://localhost:8080/nodes.json

# 7. View registry logs
sudo journalctl -u dinero-registry -f
```

## 🔥 Firewall Configuration

### On All Servers
```bash
# Allow RPC/HTTP port
sudo ufw allow 21999/tcp

# Allow WebSocket port
sudo ufw allow 21000/tcp

# Allow P2P port
sudo ufw allow 20999/tcp

# Check status
sudo ufw status
```

### On Virginia (Registry Server)
```bash
# Allow registry port
sudo ufw allow 8080/tcp

# OR if using nginx with SSL
sudo ufw allow 443/tcp
sudo ufw allow 80/tcp
```

## 📊 What's New (Last 48 Hours)

### Features Added
1. ✅ **HTTP Server** - Full HTTP server alongside RPC
2. ✅ **serverinfo.json** - Node metadata endpoint
3. ✅ **CORS Support** - Browser-friendly API
4. ✅ **Global Registry** - Network-wide node discovery
5. ✅ **Web Dashboard** - Visual network status
6. ✅ **Self-Registration** - Nodes can auto-register

### Files Modified
- `src/httprpc.cpp` - HTTP server implementation

### New Files/Directories
- `registry/` - Complete registry system (13 files)
- `deploy_updates.sh` - Automated deployment script
- `DEPLOY_TO_SERVERS.md` - Deployment guide

## 🧪 Testing Checklist

Run these commands on each server after deployment:

### Virginia
```bash
# Daemon
~/DineroCoin/build/dinero-cli getinfo

# HTTP endpoint (local)
curl http://localhost:21999/serverinfo.json

# HTTP endpoint (external)
curl http://173.249.195.59:21999/serverinfo.json

# Registry status
sudo systemctl status dinero-registry

# Registry API
curl http://localhost:8080/api/status
curl http://localhost:8080/nodes.json

# Registry (external)
curl http://173.249.195.59:8080/api/status
```

### California
```bash
# Daemon
~/DineroCoin/build/dinero-cli getinfo

# HTTP endpoint (local)
curl http://localhost:21999/serverinfo.json

# HTTP endpoint (external)
curl http://172.93.160.131:21999/serverinfo.json

# Check if registered with Virginia registry
curl http://173.249.195.59:8080/nodes.json | grep California
```

## 🐛 Common Issues & Fixes

### Issue: "HTTP server not responding"
```bash
# Check if daemon is running
ps aux | grep dinerod

# Check if port is open locally
netstat -tulpn | grep 21999

# Check firewall
sudo ufw status

# Restart daemon
~/DineroCoin/build/dinero-cli stop
sleep 5
~/DineroCoin/build/dinerod -daemon
```

### Issue: "Can't access from outside"
```bash
# Check firewall
sudo ufw allow 21999/tcp
sudo ufw reload

# Check binding (should show 0.0.0.0, not 127.0.0.1)
netstat -tulpn | grep 21999

# Test from external server
curl http://YOUR_IP:21999/serverinfo.json
```

### Issue: "Registry not seeing nodes"
```bash
# Check registry is running
sudo systemctl status dinero-registry

# Check registry logs
sudo journalctl -u dinero-registry -n 50

# Test direct connection to nodes
curl http://173.249.195.59:21999/serverinfo.json
curl http://172.93.160.131:21999/serverinfo.json

# Restart registry
sudo systemctl restart dinero-registry
```

### Issue: "Build failed"
```bash
# Install dependencies
sudo apt-get update
sudo apt-get install -y build-essential libtool autotools-dev automake \
  pkg-config bsdmainutils python3 libssl-dev libevent-dev libboost-all-dev

# Clean and rebuild
cd ~/DineroCoin
make clean
./autogen.sh
./configure
make -j$(nproc)
```

## 🔄 Updating After Initial Deployment

```bash
# Pull latest changes
cd ~/DineroCoin
git pull

# Rebuild
make -j$(nproc)

# Restart daemon
~/DineroCoin/build/dinero-cli stop
sleep 5
~/DineroCoin/build/dinerod -daemon

# Restart registry (Virginia only)
sudo systemctl restart dinero-registry
```

## 📁 Important File Locations

### On All Servers
```
~/DineroCoin/                    # Repository root
~/DineroCoin/build/dinerod       # Daemon binary
~/DineroCoin/build/dinero-cli    # CLI tool
~/.dinero/dinero.conf            # Configuration
~/.dinero/debug.log              # Daemon logs
~/.dinero/dinero.conf.backup.*   # Config backups
~/dinero_backup_*/               # Full backups
```

### On Virginia (Registry)
```
~/DineroCoin/registry/                      # Registry source
/etc/systemd/system/dinero-registry.service # Service file
```

## 🔗 Useful URLs After Deployment

### Virginia
- HTTP: http://173.249.195.59:21999/
- Server Info: http://173.249.195.59:21999/serverinfo.json
- Registry Dashboard: http://173.249.195.59:8080/
- Registry API: http://173.249.195.59:8080/api/status
- Node List: http://173.249.195.59:8080/nodes.json

### California
- HTTP: http://172.93.160.131:21999/
- Server Info: http://172.93.160.131:21999/serverinfo.json

## 📞 Emergency Commands

### Stop Everything
```bash
# Stop daemon
~/DineroCoin/build/dinero-cli stop

# Stop registry (Virginia only)
sudo systemctl stop dinero-registry
```

### Rollback to Backup
```bash
# Find latest backup
ls -dt ~/dinero_backup_* | head -1

# Stop current daemon
~/DineroCoin/build/dinero-cli stop

# Start from backup
BACKUP=$(ls -dt ~/dinero_backup_* | head -1)
cd $BACKUP/DineroCoin
./build/dinerod -daemon
```

### View All Logs
```bash
# Daemon logs
tail -f ~/.dinero/debug.log

# Registry logs (Virginia)
sudo journalctl -u dinero-registry -f

# System logs
tail -f /var/log/syslog
```

## ✅ Success Indicators

After deployment, you should see:

### Virginia
✅ Daemon responds to `dinero-cli getinfo`
✅ HTTP server returns data at `:21999`
✅ serverinfo.json accessible externally
✅ Registry service is active
✅ Registry shows 2+ nodes in dashboard
✅ No errors in debug.log or registry logs

### California
✅ Daemon responds to `dinero-cli getinfo`
✅ HTTP server returns data at `:21999`
✅ serverinfo.json accessible externally
✅ Appears in Virginia registry's node list
✅ No errors in debug.log

## 🎯 Quick Commands Reference

```bash
# Daemon status
~/DineroCoin/build/dinero-cli getinfo

# Stop daemon
~/DineroCoin/build/dinero-cli stop

# Start daemon
~/DineroCoin/build/dinerod -daemon

# View daemon logs
tail -f ~/.dinero/debug.log

# Test HTTP
curl http://localhost:21999/serverinfo.json

# Test external HTTP
curl http://$(curl -s ifconfig.me):21999/serverinfo.json

# Registry status (Virginia)
sudo systemctl status dinero-registry

# View registry logs (Virginia)
sudo journalctl -u dinero-registry -f

# Restart registry (Virginia)
sudo systemctl restart dinero-registry

# Full rebuild
cd ~/DineroCoin && make clean && ./autogen.sh && ./configure && make -j$(nproc)
```

---

**Keep this reference handy!** 📖

Save it somewhere accessible for quick troubleshooting.
