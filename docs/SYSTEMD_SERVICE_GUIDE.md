# DineroCoin Systemd Service Management Guide

**Purpose:** Run DineroCoin daemon as a system service with automatic restart and boot startup

**Date:** 2026-01-07

---

## Quick Installation

On your Dell tower Linux server, run:

```bash
cd ~/Dinero-Coin
chmod +x tools/install_systemd_service.sh
sudo ./tools/install_systemd_service.sh
```

This will:
- ✅ Install systemd service
- ✅ Enable auto-start on boot
- ✅ Auto-restart on crash
- ✅ Start the daemon immediately

---

## Service Management Commands

### Check Status
```bash
sudo systemctl status dinerod
```

### View Live Logs
```bash
# All logs (live tail)
sudo journalctl -u dinerod -f

# Today's logs
sudo journalctl -u dinerod -f --since today

# Last 100 lines
sudo journalctl -u dinerod -n 100

# Errors only
sudo journalctl -u dinerod -p err
```

### Start/Stop/Restart
```bash
# Start daemon
sudo systemctl start dinerod

# Stop daemon
sudo systemctl stop dinerod

# Restart daemon
sudo systemctl restart dinerod

# Reload configuration (without restart)
sudo systemctl reload-or-restart dinerod
```

### Enable/Disable Auto-Start
```bash
# Enable auto-start on boot
sudo systemctl enable dinerod

# Disable auto-start on boot
sudo systemctl disable dinerod

# Check if enabled
sudo systemctl is-enabled dinerod
```

### Check if Running
```bash
# Quick check
sudo systemctl is-active dinerod

# Detailed check
ps aux | grep dinerod

# Using dinero-cli
./build/dinero-cli getconnectioncount
```

---

## Systemd Service Configuration

**Location:** `/etc/systemd/system/dinerod.service`

**Key Features:**
- **Auto-restart:** Daemon restarts automatically if it crashes
- **Boot startup:** Starts on system boot
- **Restart policy:**
  - Waits 10 seconds after crash
  - Maximum 5 restarts in 200 seconds
  - Then gives up (prevents restart loops)
- **Timeouts:**
  - 60 seconds to start
  - 300 seconds (5 min) to stop gracefully
- **Resource limits:**
  - 65,536 open files
  - 4,096 processes

**User/Group:** `tower` (runs as your user, not root)

**Working Directory:** `/home/tower/Dinero-Coin`

**Data Directory:** `/home/tower/.dinero`

---

## Monitoring for 7-Day Stability Test

### Daily Checks

**1. Service Health**
```bash
sudo systemctl status dinerod
```
Look for:
- ✅ Active: active (running)
- ✅ Main PID: [some number]
- ❌ Failed or restarting

**2. Resource Usage**
```bash
cd ~/Dinero-Coin
./tools/diagnose_node.sh | grep -A5 "Resource Usage"
```

**3. Uptime**
```bash
systemctl show dinerod --property=ActiveEnterTimestamp
```

**4. Restart Count** (should be 0 for stable daemon)
```bash
systemctl show dinerod --property=NRestarts
```

**5. Memory Usage**
```bash
systemctl status dinerod | grep Memory
```

### Automated Monitoring Script

Create a daily check:

```bash
cat > ~/check_dinerod_health.sh <<'EOF'
#!/bin/bash
echo "═══════════════════════════════════════════"
echo "DineroCoin Daemon Health Check"
echo "═══════════════════════════════════════════"
echo ""
echo "Status:"
systemctl is-active dinerod && echo "✅ Running" || echo "❌ Stopped"
echo ""
echo "Uptime:"
systemctl show dinerod --property=ActiveEnterTimestamp
echo ""
echo "Restart Count:"
systemctl show dinerod --property=NRestarts
echo ""
echo "Memory:"
systemctl status dinerod --no-pager | grep Memory
echo ""
echo "Recent Logs (last 10 lines):"
journalctl -u dinerod -n 10 --no-pager
echo ""
echo "Blockchain Height:"
~/Dinero-Coin/build/dinero-cli blockchain.getblockcount 2>/dev/null || echo "RPC not responding"
echo ""
echo "Peer Count:"
~/Dinero-Coin/build/dinero-cli getconnectioncount 2>/dev/null || echo "RPC not responding"
EOF

chmod +x ~/check_dinerod_health.sh
```

Run daily:
```bash
./check_dinerod_health.sh
```

Or add to cron for automatic daily checks:
```bash
# Run every day at 9 AM and log to file
echo "0 9 * * * /home/tower/check_dinerod_health.sh >> /home/tower/dinerod_health.log 2>&1" | crontab -
```

---

## Troubleshooting

### Service Won't Start

**Check logs:**
```bash
sudo journalctl -u dinerod -n 50
```

**Check if binary exists:**
```bash
ls -lh ~/Dinero-Coin/build/dinerod
```

**Check permissions:**
```bash
# Service file should be owned by root
ls -l /etc/systemd/system/dinerod.service

# Binary should be executable by tower user
ls -l ~/Dinero-Coin/build/dinerod
```

**Manually test daemon:**
```bash
# Stop service
sudo systemctl stop dinerod

# Run manually to see errors
~/Dinero-Coin/build/dinerod -printtoconsole

# Press Ctrl+C to stop, then restart service
sudo systemctl start dinerod
```

### Service Keeps Restarting

**Check restart count:**
```bash
systemctl show dinerod --property=NRestarts
```

**View crash logs:**
```bash
sudo journalctl -u dinerod -p err
```

**Common causes:**
- Configuration error in `~/.dinero/dinero.conf`
- Disk space full
- Port already in use (20997, 20999, 3333)
- Corrupted blockchain database

**Fix:**
```bash
# Check disk space
df -h

# Check ports
netstat -lntp | grep -E "20997|20999|3333"

# Check config
cat ~/.dinero/dinero.conf

# If database corrupted, reindex (WARNING: slow)
sudo systemctl stop dinerod
~/Dinero-Coin/build/dinerod -reindex
```

### Service Won't Stop

**Force stop:**
```bash
sudo systemctl stop dinerod
sudo pkill -9 dinerod
```

**Check if stopped:**
```bash
ps aux | grep dinerod
```

### Logs Not Appearing

**Check journald:**
```bash
sudo systemctl status systemd-journald
```

**Manual log check:**
```bash
tail -f ~/.dinero/debug.log
```

---

## Updating the Service

If you change the service file:

```bash
# Edit the file
sudo nano /etc/systemd/system/dinerod.service

# Reload systemd
sudo systemctl daemon-reload

# Restart service
sudo systemctl restart dinerod

# Verify
sudo systemctl status dinerod
```

---

## Removing the Service

```bash
# Stop and disable
sudo systemctl stop dinerod
sudo systemctl disable dinerod

# Remove service file
sudo rm /etc/systemd/system/dinerod.service

# Reload systemd
sudo systemctl daemon-reload
```

---

## Best Practices for 7-Day Test

1. **Don't restart manually** unless necessary (testing auto-restart)
2. **Monitor daily** using `check_dinerod_health.sh`
3. **Track uptime** from start to end (should be 168+ hours)
4. **Check restart count** (should remain 0)
5. **Monitor resource usage** (memory should be stable)
6. **Review logs weekly** for warnings or errors
7. **Document any issues** for post-test analysis

### Success Criteria

After 7+ days:
- ✅ Service still running
- ✅ Zero unplanned restarts
- ✅ Memory usage stable (no leaks)
- ✅ No errors in logs
- ✅ RPC responding
- ✅ System resources healthy

Then you're ready to deploy to external seed nodes!

---

## Production Deployment Checklist

Once 7-day test passes:

- [ ] Document final systemd configuration
- [ ] Create deployment script for external servers
- [ ] Test service on clean VPS
- [ ] Configure firewall for ports 20997, 20999, 3333
- [ ] Set up monitoring/alerting (optional: Prometheus, Grafana)
- [ ] Create backup/restore procedures
- [ ] Document rollback plan

---

**Good luck with the 7-day stability test!**
