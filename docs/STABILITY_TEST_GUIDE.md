# 7-Day Stability Test Guide

This guide explains how to run and monitor the 7-day continuous operation stability test for the DineroCoin daemon on the Dell Tower production node.

## Overview

**Goal**: Run dinerod continuously for 7+ days with zero unplanned restarts to verify production readiness.

**Success Criteria**:
- ✅ Service remains active (running) continuously
- ✅ Zero restarts (NRestarts = 0)
- ✅ Memory usage stable (no leaks)
- ✅ No critical errors in logs
- ✅ RPC responding to queries
- ✅ Process uptime = 7 days

## Test Started

**Start Time**: 2026-01-07 06:10:22 EST
**Target End**: 2026-01-14 06:10:22 EST (7 days)

**Location**: Dell Tower (tower@192.168.1.114)
**Service**: dinerod.service (systemd)

## Daily Monitoring

### Automated Daily Check

Run the stability check script once per day:

```bash
cd ~/Dinero-Coin
./tools/stability_check.sh
```

**What it checks**:
1. Service status (active/running)
2. Uptime (days/hours/minutes)
3. Restart count (MUST be 0)
4. Memory usage (current + peak)
5. CPU usage (total + average)
6. RPC connectivity
7. Recent errors in journal
8. Disk space

**Output locations**:
- Screen: Colored summary with all metrics
- Log file: `~/stability_test_log.txt` (CSV format for trending)

### Quick Status Check

For a quick status without full report:

```bash
# Service status
sudo systemctl status dinerod

# Uptime
systemctl show dinerod --property=ActiveEnterTimestamp

# Critical metric: Restart count
systemctl show dinerod --property=NRestarts
```

### Alert-Only Mode

To run the script in cron and only see output if there are issues:

```bash
./tools/stability_check.sh --alert-only
```

This will:
- Exit silently if everything is OK (exit code 0)
- Print issues and exit with error code if problems detected

## Manual Monitoring Commands

### Real-Time Logs

```bash
# Watch live logs (Ctrl+C to exit)
sudo journalctl -u dinerod -f

# Show logs from last hour
sudo journalctl -u dinerod --since "1 hour ago"

# Show only errors
sudo journalctl -u dinerod -p err --since today

# Show logs from specific date
sudo journalctl -u dinerod --since "2026-01-07 00:00:00"
```

### Service Metrics

```bash
# All metrics at once
systemctl show dinerod

# Specific metrics
systemctl show dinerod --property=ActiveState,SubState,NRestarts,MemoryCurrent,CPUUsageNSec

# Check if enabled for boot
systemctl is-enabled dinerod
```

### Process Information

```bash
# Process details
ps aux | grep dinerod | grep -v grep

# Memory usage over time
watch -n 5 'ps aux | grep dinerod | grep -v grep'

# File descriptors
lsof -p $(pgrep dinerod) | wc -l
```

### RPC Health Check

```bash
cd ~/Dinero-Coin

# Block height
./build/dinero-cli blockchain.getblockcount

# Peer count
./build/dinero-cli getconnectioncount

# Full node info
./build/dinero-cli getinfo 2>/dev/null || echo "getinfo not available"
```

### Disk Space Monitoring

```bash
# Data directory size
du -sh ~/.dinero

# Available disk space
df -h ~/.dinero

# Growth over last 24h
du -sh ~/.dinero && sleep 86400 && du -sh ~/.dinero
```

## Expected Behavior

### Normal

- **Uptime**: Continuously increasing
- **Restarts**: Always 0
- **Memory**: Stable around 30-50 MB
- **CPU**: Low average (<5%)
- **Logs**: Mostly "Failed to connect to seed nodes" (expected on isolated network)
- **RPC**: Always responding
- **Peers**: 0 (isolated network)

### Abnormal (Action Required)

| Issue | Symptom | Action |
|-------|---------|--------|
| Service stopped | `systemctl status dinerod` shows inactive | Check logs: `sudo journalctl -u dinerod -n 50` |
| Restart detected | `NRestarts > 0` | **TEST FAILED** - Investigate crash cause |
| Memory leak | Memory growing continuously | Check for open file handles, monitor logs |
| RPC not responding | CLI commands timeout | Check service logs, verify process running |
| High CPU | CPU usage sustained >50% | Check for stuck threads in logs |
| Disk full | Disk usage >95% | Clean up logs, check for large files |

## Automated Monitoring with Cron

To run daily checks automatically at 9 AM:

```bash
# Edit crontab
crontab -e

# Add this line:
0 9 * * * /home/tower/Dinero-Coin/tools/stability_check.sh --alert-only >> /home/tower/stability_cron.log 2>&1 || echo "Stability check failed at $(date)" | mail -s "DineroCoin Alert" admin@example.com
```

For email alerts, install `mailutils`:

```bash
sudo apt-get install mailutils
```

## Progress Tracking

### View Progress Chart

The stability check script shows a progress table:

```
Last 7 Days:
Date                | Status | Uptime      | Restarts | Memory  | RPC
--------------------+--------+-------------+----------+---------+-----
2026-01-07 09:00:00 | UP     | 2h 50m      | 0        | 32 MB   | YES
2026-01-08 09:00:00 | UP     | 1d 2h 50m   | 0        | 34 MB   | YES
2026-01-09 09:00:00 | UP     | 2d 2h 50m   | 0        | 33 MB   | YES
...
```

### Calculate Days Remaining

```bash
# Get start time
START=$(systemctl show dinerod --property=ActiveEnterTimestamp --value)
START_EPOCH=$(date -d "$START" +%s)

# Calculate elapsed
NOW_EPOCH=$(date +%s)
ELAPSED_DAYS=$(( (NOW_EPOCH - START_EPOCH) / 86400 ))
REMAINING_DAYS=$(( 7 - ELAPSED_DAYS ))

echo "Days elapsed: $ELAPSED_DAYS of 7"
echo "Days remaining: $REMAINING_DAYS"
```

## Troubleshooting

### Service Won't Start

```bash
# Check for errors
sudo journalctl -u dinerod -n 50

# Check binary exists
ls -lh ~/Dinero-Coin/build/dinerod

# Check config
cat ~/.dinero/dinero.conf

# Try manual start
cd ~/Dinero-Coin
./build/dinerod
```

### Service Keeps Restarting

```bash
# Disable auto-restart temporarily
sudo systemctl set-property dinerod.service Restart=no

# Start manually to see errors
./build/dinerod

# Re-enable auto-restart
sudo systemctl set-property dinerod.service Restart=always
```

### High Memory Usage

```bash
# Check memory details
ps -o pid,vsz,rss,comm -p $(pgrep dinerod)

# Check for file descriptor leaks
lsof -p $(pgrep dinerod) | wc -l

# Check for zombie processes
ps aux | grep defunct
```

### RPC Not Responding

```bash
# Check if port is listening
netstat -tulpn | grep 20998

# Check RPC cookie
cat ~/.dinero/.cookie

# Test with curl
curl --user __cookie__:$(cat ~/.dinero/.cookie) \
  --data-binary '{"jsonrpc":"1.0","id":"test","method":"blockchain.getblockcount","params":[]}' \
  -H 'content-type: text/plain;' \
  http://127.0.0.1:20998/
```

## After 7 Days

### Success Checklist

If all criteria met after 7 days:

- [ ] Uptime = 7+ days continuously
- [ ] NRestarts = 0 (zero restarts)
- [ ] Memory stable (no growth trend)
- [ ] No critical errors in logs
- [ ] RPC responding throughout
- [ ] All daily checks passed

### Next Steps

1. **Document final metrics**:
   ```bash
   ./tools/stability_check.sh > stability_test_final_report.txt
   ```

2. **Create stable release tag**:
   ```bash
   git tag -a v2.3.0-stable -m "7-day stability test passed on Dell Tower"
   git push origin v2.3.0-stable
   ```

3. **Deploy to external seed nodes**:
   - Configure firewall rules
   - Update DNS entries
   - Enable external connections
   - Begin external peer testing

4. **Continue monitoring**:
   - Keep daily checks running
   - Monitor peer connections
   - Track blockchain sync

## Support

**Issues during test?**

1. Check logs: `sudo journalctl -u dinerod -f`
2. Run full diagnostic: `./tools/diagnose_node.sh`
3. Review this guide
4. Document issue in GitHub issues

**Emergency stop** (breaks test):

```bash
sudo systemctl stop dinerod
```

**Emergency restart** (resets restart counter):

```bash
sudo systemctl restart dinerod
# Note: This FAILS the stability test - restart count must stay at 0
```

## Log Files

- **Systemd journal**: `sudo journalctl -u dinerod`
- **Stability log**: `~/stability_test_log.txt`
- **Daemon output**: Captured in journal
- **RPC debug**: In journal (if debug=rpc enabled)

## Useful Links

- Systemd Service Guide: `docs/SYSTEMD_SERVICE_GUIDE.md`
- Node Diagnostic Tool: `tools/diagnose_node.sh`
- Configuration: `~/.dinero/dinero.conf`
- Service File: `/etc/systemd/system/dinerod.service`
