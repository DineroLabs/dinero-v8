# Dinero Operations Runbook

## Quick Start

### 1. First Run Setup
```bash
# Create datadir
mkdir -p ~/Library/Application\ Support/DineroCoin  # macOS
# or
mkdir -p %APPDATA%\DineroCoin  # Windows
# or  
mkdir -p ~/.local/share/DineroCoin  # Linux

# Write config
cat > ~/Library/Application\ Support/DineroCoin/dinero.conf << EOF
server=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=20998
port=20999
rpccookiefile=~/.dinero/.cookie
EOF

# Start daemon
./dinerod -datadir="$HOME/.dinero"
```

### 2. Health Check
```bash
# Quick health check
./scripts/din-health.sh

# Expected output:
# Status: healthy
# Chain: din
# Height: 12345
# Mining: enabled
# Hashrate: 1000000 H/s
```

## Service Installation

### macOS (launchd)
```bash
# Install service
./scripts/install-service.sh

# Check status
launchctl list | grep dinero

# View logs
tail -f ~/Library/Logs/dinerod.log
```

### Windows (Service)
```powershell
# Install service
$exe = "$env:ProgramFiles\Dinero\dinerod.exe"
$datadir = "$env:APPDATA\DineroCoin"
sc.exe create DineroD binPath= "`"$exe`" -datadir=`"$datadir`"" start= auto
sc.exe failure DineroD reset= 0 actions= restart/1000
net start DineroD

# Check status
sc.exe query DineroD
```

### Linux (systemd)
```bash
# Install service
mkdir -p ~/.config/systemd/user
cp contrib/systemd/dinerod.service ~/.config/systemd/user/
systemctl --user enable --now dinerod

# Check status
systemctl --user status dinerod
```

## Troubleshooting

### Daemon Won't Start
```bash
# Check config
cat ~/Library/Application\ Support/DineroCoin/dinero.conf

# Check permissions
ls -la ~/Library/Application\ Support/DineroCoin/

# Run in foreground for debugging
./dinerod -datadir="$HOME/.dinero" -debug
```

### RPC Connection Issues
```bash
# Check if daemon is running
ps aux | grep dinerod

# Check RPC port
netstat -an | grep 20998

# Test RPC directly
curl -u $(cat ~/Library/Application\ Support/DineroCoin/.cookie) \
     -X POST \
     -H "Content-Type: application/json" \
     -d '{"jsonrpc":"2.0","id":1,"method":"gethealth","params":[]}' \
     http://127.0.0.1:20998
```

### Cookie Authentication Issues
```bash
# Check cookie file exists
ls -la ~/Library/Application\ Support/DineroCoin/.cookie

# Check cookie format (should be user:pass)
cat ~/Library/Application\ Support/DineroCoin/.cookie

# Regenerate cookie (restart daemon)
pkill dinerod
./dinerod -datadir="$HOME/.dinero"
```

## Health Monitoring

### Automated Health Checks
```bash
# Add to crontab for regular monitoring
*/5 * * * * /path/to/scripts/din-health.sh >> /var/log/dinero-health.log 2>&1
```

### Health Metrics
- **Status**: healthy/degraded/unhealthy
- **Chain**: Network identifier (din)
- **Height**: Current block height
- **Mining**: Mining status (enabled/disabled)
- **Hashrate**: Current hashrate in H/s
- **Mempool**: Number of pending transactions
- **Connections**: Number of peer connections
- **Uptime**: Daemon uptime in seconds

### Alert Thresholds
- **Height**: No new blocks for >10 minutes
- **Mining**: Hashrate drops below 1000 H/s
- **Mempool**: Size exceeds 1000 transactions
- **Connections**: Zero peer connections
- **Uptime**: Daemon restarted recently

## Configuration Management

### Environment Variables
```bash
# Override default paths
export DINERO_DATADIR="/custom/datadir"
export DINERO_COOKIE_FILE="/custom/cookie"
export DINERO_RPC_URL="http://127.0.0.1:20998"

# Static credentials (alternative to cookie)
export DINERO_RPC_USER="myuser"
export DINERO_RPC_PASSWORD="mypass"

# SQLite wallet durability (production defaults)
export DIN_WAL_STRONG="1"           # Enable SAFE durability mode
export DINERO_WALLET_SYNC="FULL"    # Force synchronous=FULL for maximum safety
```

### Configuration Files
```ini
# dinero.conf - Main configuration
server=1                    # Enable RPC server
rpcbind=127.0.0.1          # Bind to localhost only
rpcallowip=127.0.0.1       # Allow localhost connections
rpcport=20998              # RPC port
port=20999                 # P2P port
rpccookiefile=<DATADIR>/.cookie  # Cookie authentication file

# Optional: Static credentials
rpcuser=myuser
rpcpassword=mypass

# Optional: Mining configuration
miningaddress=din1q...
generatelimit=1000000
```

## Backup and Recovery

### Data Backup
```bash
# Backup datadir
tar -czf dinero-backup-$(date +%Y%m%d).tar.gz \
    ~/Library/Application\ Support/DineroCoin/

# Backup specific components
cp ~/Library/Application\ Support/DineroCoin/explorer.db explorer-backup.db
cp ~/Library/Application\ Support/DineroCoin/wallet.db wallet-backup.db
```

### Recovery
```bash
# Restore from backup
tar -xzf dinero-backup-20240101.tar.gz -C ~/

# Verify recovery
./scripts/din-health.sh
```

## Performance Tuning

### Mining Optimization
```bash
# Check CPU usage
top -p $(pgrep dinerod)

# Monitor memory usage
ps -o pid,vsz,rss,comm -p $(pgrep dinerod)

# Adjust mining threads
# Edit dinero.conf:
generatelimit=1000000  # Hash rate limit
```

### Database Optimization
```bash
# Check database size
du -sh ~/Library/Application\ Support/DineroCoin/explorer.db

# Compact database (if needed)
# Restart daemon to trigger compaction
```

## Security Best Practices

### Network Security
- Always bind RPC to 127.0.0.1
- Use cookie authentication
- Never expose RPC to external networks
- Use firewall rules if needed

### File Permissions
```bash
# Secure datadir
chmod 700 ~/Library/Application\ Support/DineroCoin/
chmod 600 ~/Library/Application\ Support/DineroCoin/.cookie
chmod 600 ~/Library/Application\ Support/DineroCoin/dinero.conf
```

### Monitoring
- Monitor log files for suspicious activity
- Set up alerts for unusual behavior
- Regular security updates
- Backup important data

## Emergency Procedures

### Daemon Crash
```bash
# Check crash logs
tail -100 ~/Library/Logs/dinerod.log

# Restart service
launchctl unload ~/Library/LaunchAgents/org.dinero.dinerod.plist
launchctl load ~/Library/LaunchAgents/org.dinero.dinerod.plist
```

### Data Corruption
```bash
# Stop daemon
pkill dinerod

# Restore from backup
cp explorer-backup.db ~/Library/Application\ Support/DineroCoin/explorer.db

# Restart daemon
./dinerod -datadir="$HOME/.dinero"
```

### Network Issues
```bash
# Check network connectivity
ping 127.0.0.1

# Check port availability
netstat -an | grep 20998

# Restart network service
sudo ifconfig en0 down && sudo ifconfig en0 up
```
