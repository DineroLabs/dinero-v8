# Dinero Node - Linux Server Deployment

**Optimized for Ubuntu 22.04.5 LTS Server**
- Memory: 1GB RAM
- CPU: 1 Core  
- Storage: 15GB
- IPv4: 96.9.226.98

## 🚀 Quick Installation

1. **Upload files to your server:**
   ```bash
   scp dinero-linux-server.tar.gz root@96.9.226.98:/tmp/
   ```

2. **Extract and install:**
   ```bash
   ssh root@96.9.226.98
   cd /tmp
   tar -xzf dinero-linux-server.tar.gz
   cd dinero-linux-server
   chmod +x *.sh
   sudo ./install.sh
   ```

3. **Copy binaries (you need to build these for Linux first):**
   ```bash
   # Copy your Linux-compiled binaries to:
   sudo cp dinerod /opt/dinero/
   sudo cp dinero-cli /opt/dinero/
   sudo chown dinero:dinero /opt/dinero/dinerod /opt/dinero/dinero-cli
   sudo chmod +x /opt/dinero/dinerod /opt/dinero/dinero-cli
   ```

4. **Start the node:**
   ```bash
   sudo systemctl start dinerod
   sudo systemctl status dinerod
   ```

## 📊 Server Configuration

### Memory Optimization (1GB RAM)
- `dbcache=128` (reduced from default 300MB)
- `maxmempool=50` (reduced mempool size)
- `maxconnections=32` (fewer connections)
- `prune=2000` (keep only 2GB of blocks)

### Network Settings
- **P2P Port:** 23999 (open to internet)
- **RPC Port:** 20998 (⚠️ secure this in production)
- **WebSocket:** 22999 (for real-time data)

### Security Features
- Dedicated `dinero` user
- UFW firewall configured
- Fail2ban protection
- Log rotation
- Systemd service with security restrictions

## 🔧 Management Commands

```bash
# Service management
sudo systemctl start dinerod     # Start
sudo systemctl stop dinerod      # Stop  
sudo systemctl restart dinerod   # Restart
sudo systemctl status dinerod    # Status

# Monitoring
sudo journalctl -u dinerod -f    # Live logs
./status.sh                      # Full status
htop                            # System resources

# RPC commands
cd /opt/dinero
./dinero-cli -datadir=./data getblockchaininfo
./dinero-cli -datadir=./data getmininginfo
./dinero-cli -datadir=./data getnetworkinfo
```

## 🌐 External Access

Your node will be accessible at:
- **P2P Network:** `96.9.226.98:23999`
- **RPC API:** `96.9.226.98:20998` 
- **WebSocket:** `96.9.226.98:22999`

⚠️ **Security Warning:** RPC is currently open to all IPs. For production:
1. Restrict `rpcallowip` to specific IPs
2. Use SSL/TLS certificates
3. Set strong RPC credentials

## 📈 Monitoring

### Log Files
- **Service logs:** `sudo journalctl -u dinerod`
- **Daemon logs:** `/opt/dinero/data/logs/daemon.log`
- **System logs:** `/var/log/syslog`

### Performance Monitoring
```bash
# Disk usage
df -h

# Memory usage  
free -h

# Network connections
ss -tulpn | grep dinero

# Process status
ps aux | grep dinero
```

## 🔄 Updates

To update the node:
1. Stop the service: `sudo systemctl stop dinerod`
2. Replace binaries in `/opt/dinero/`
3. Start the service: `sudo systemctl start dinerod`

## 🆘 Troubleshooting

### Common Issues

**Node won't start:**
```bash
sudo journalctl -u dinerod -n 50
```

**RPC not responding:**
```bash
# Check if port is listening
sudo netstat -tlnp | grep 20998

# Check firewall
sudo ufw status
```

**Out of disk space:**
```bash
# Check usage
df -h

# Clean logs
sudo journalctl --vacuum-time=7d
```

**Memory issues:**
```bash
# Check memory
free -h

# Reduce cache if needed (edit /etc/dinero/dinero.conf)
dbcache=64
```

This configuration is optimized for your server specs and should run efficiently within the 1GB RAM limit!
