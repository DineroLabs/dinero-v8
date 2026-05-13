# 7-Day Stability Test - Dell Tower

**Test Start:** 2026-01-07 06:10:22 EST
**Test End:** 2026-01-14 06:10:22 EST
**Location:** Dell Precision 7920 Tower (tower@192.168.1.114)
**Service:** dinerod.service (systemd)
**Purpose:** Validate continuous operation before external seed node deployment

---

## Daily Checklist

Copy this section for each day's check:

### Day 1 - 2026-01-07 ✅

**Date/Time:** 2026-01-07 06:10:22 EST
**Checked By:** ___________

**Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________
  - Memory: ___________

**Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0)

**Uptime:**
- [ ] Started: `systemctl show dinerod --property=ActiveEnterTimestamp`
  - Time: ___________

**RPC Connectivity:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)

**Logs Check:**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l`
  - Error Count: ___________
  - Critical Issues: ___________

**Resource Usage:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Trend: ___________ (stable/growing/shrinking)

- [ ] CPU: `ps aux | grep dinerod | awk '{print $3 "%"}'`
  - Current: ___________ %

**Notes:**
___________________________________________________________
___________________________________________________________

---

### Day 2 - 2026-01-08

**Date/Time:** ___________
**Checked By:** ___________

**Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________ (should be same as Day 1)
  - Memory: ___________

**Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0)

**Uptime:**
- [ ] Started: `systemctl show dinerod --property=ActiveEnterTimestamp`
  - Time: ___________ (should be Day 1 start time)

**RPC Connectivity:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)

**Logs Check:**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l`
  - Error Count: ___________
  - Critical Issues: ___________

**Resource Usage:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Trend: ___________ (compared to Day 1)

- [ ] CPU: `ps aux | grep dinerod | awk '{print $3 "%"}'`
  - Current: ___________ %

**Notes:**
___________________________________________________________
___________________________________________________________

---

### Day 3 - 2026-01-09

**Date/Time:** ___________
**Checked By:** ___________

**Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________ (should be same as Day 1)
  - Memory: ___________

**Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0)

**Uptime:**
- [ ] Started: `systemctl show dinerod --property=ActiveEnterTimestamp`
  - Time: ___________ (should be Day 1 start time)

**RPC Connectivity:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)

**Logs Check:**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l`
  - Error Count: ___________
  - Critical Issues: ___________

**Resource Usage:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Trend: ___________ (compared to Days 1-2)

- [ ] CPU: `ps aux | grep dinerod | awk '{print $3 "%"}'`
  - Current: ___________ %

**Notes:**
___________________________________________________________
___________________________________________________________

---

### Day 4 - 2026-01-10

**Date/Time:** ___________
**Checked By:** ___________

**Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________ (should be same as Day 1)
  - Memory: ___________

**Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0)

**Uptime:**
- [ ] Started: `systemctl show dinerod --property=ActiveEnterTimestamp`
  - Time: ___________ (should be Day 1 start time)

**RPC Connectivity:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)

**Logs Check:**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l`
  - Error Count: ___________
  - Critical Issues: ___________

**Resource Usage:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Trend: ___________ (compared to Days 1-3)

- [ ] CPU: `ps aux | grep dinerod | awk '{print $3 "%"}'`
  - Current: ___________ %

**Notes:**
___________________________________________________________
___________________________________________________________

---

### Day 5 - 2026-01-11

**Date/Time:** ___________
**Checked By:** ___________

**Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________ (should be same as Day 1)
  - Memory: ___________

**Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0)

**Uptime:**
- [ ] Started: `systemctl show dinerod --property=ActiveEnterTimestamp`
  - Time: ___________ (should be Day 1 start time)

**RPC Connectivity:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)

**Logs Check:**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l`
  - Error Count: ___________
  - Critical Issues: ___________

**Resource Usage:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Trend: ___________ (compared to Days 1-4)

- [ ] CPU: `ps aux | grep dinerod | awk '{print $3 "%"}'`
  - Current: ___________ %

**Notes:**
___________________________________________________________
___________________________________________________________

---

### Day 6 - 2026-01-12

**Date/Time:** ___________
**Checked By:** ___________

**Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________ (should be same as Day 1)
  - Memory: ___________

**Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0)

**Uptime:**
- [ ] Started: `systemctl show dinerod --property=ActiveEnterTimestamp`
  - Time: ___________ (should be Day 1 start time)

**RPC Connectivity:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)

**Logs Check:**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l`
  - Error Count: ___________
  - Critical Issues: ___________

**Resource Usage:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Trend: ___________ (compared to Days 1-5)

- [ ] CPU: `ps aux | grep dinerod | awk '{print $3 "%"}'`
  - Current: ___________ %

**Notes:**
___________________________________________________________
___________________________________________________________

---

### Day 7 - 2026-01-13

**Date/Time:** ___________
**Checked By:** ___________

**Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________ (should be same as Day 1)
  - Memory: ___________

**Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0)

**Uptime:**
- [ ] Started: `systemctl show dinerod --property=ActiveEnterTimestamp`
  - Time: ___________ (should be Day 1 start time)

**RPC Connectivity:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)

**Logs Check:**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l`
  - Error Count: ___________
  - Critical Issues: ___________

**Resource Usage:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Trend: ___________ (compared to Days 1-6)

- [ ] CPU: `ps aux | grep dinerod | awk '{print $3 "%"}'`
  - Current: ___________ %

**Notes:**
___________________________________________________________
___________________________________________________________

---

### Final Check - 2026-01-14 (After 7 Days)

**Date/Time:** ___________
**Checked By:** ___________

**Final Service Health:**
- [ ] Service Status: `sudo systemctl status dinerod`
  - Active: ___________
  - PID: ___________ (should be same as Day 1)
  - Total Uptime: ___________

**Final Restart Count:**
- [ ] Restarts: `systemctl show dinerod --property=NRestarts`
  - Count: ___________ (MUST be 0 for success)

**Final Resource Check:**
- [ ] Memory: `ps aux | grep dinerod | awk '{print $6/1024 " MB"}'`
  - Current: ___________ MB
  - Min: ___________ MB
  - Max: ___________ MB
  - Average: ___________ MB

- [ ] CPU Average: ___________ %

**Total Error Count (7 days):**
- [ ] Errors: `sudo journalctl -u dinerod -p err --since "7 days ago" | wc -l`
  - Total: ___________
  - Critical: ___________

**RPC Final Test:**
- [ ] Block Height: `./build/dinero-cli blockchain.getblockcount`
  - Height: ___________
- [ ] Peer Count: `./build/dinero-cli getconnectioncount`
  - Peers: ___________ (0 expected - isolated)
- [ ] Full Blockchain Info: `./build/dinero-cli getblockchaininfo`
  - ✓ Working: _____

---

## Success Criteria Summary

Mark each criterion as PASS or FAIL:

- [ ] **Service Remained Active:** ___________
  - Criterion: Service status = "active (running)" for entire 7 days

- [ ] **Zero Unplanned Restarts:** ___________
  - Criterion: NRestarts = 0

- [ ] **Memory Stable:** ___________
  - Criterion: No continuous memory growth, stays within reasonable bounds

- [ ] **No Critical Errors:** ___________
  - Criterion: Zero or minimal errors in journal logs

- [ ] **RPC Responsive:** ___________
  - Criterion: RPC responds to queries throughout test

- [ ] **Uptime = 7 Days:** ___________
  - Criterion: ActiveEnterTimestamp matches Day 1 start time

---

## Test Result

**OVERALL:** ___________  (PASS / FAIL)

**If PASS:**
1. Document final configuration in deployment guide
2. Tag stable release (v2.2.9 or similar)
3. Deploy to first external seed node
4. Begin external network testing

**If FAIL:**
1. Document failure mode and symptoms
2. Identify root cause
3. Apply fixes
4. Restart 7-day test

---

## Notes and Observations

**What Worked Well:**
___________________________________________________________
___________________________________________________________
___________________________________________________________

**Issues Encountered:**
___________________________________________________________
___________________________________________________________
___________________________________________________________

**Lessons Learned:**
___________________________________________________________
___________________________________________________________
___________________________________________________________

**Next Steps:**
___________________________________________________________
___________________________________________________________
___________________________________________________________

---

## Automated Daily Check Script

Save this as `~/check_stability.sh` on Dell tower:

```bash
#!/bin/bash
echo "═══════════════════════════════════════════════"
echo "DineroCoin 7-Day Stability Check"
echo "Date: $(date)"
echo "═══════════════════════════════════════════════"
echo ""

echo "Service Status:"
sudo systemctl is-active dinerod && echo "✅ Active" || echo "❌ Inactive"
echo ""

echo "PID:"
sudo systemctl show dinerod --property=MainPID | cut -d= -f2
echo ""

echo "Restart Count:"
RESTARTS=$(sudo systemctl show dinerod --property=NRestarts | cut -d= -f2)
echo "$RESTARTS"
if [ "$RESTARTS" -eq 0 ]; then
    echo "✅ Zero restarts (good)"
else
    echo "⚠️  WARNING: Service has restarted!"
fi
echo ""

echo "Uptime:"
sudo systemctl show dinerod --property=ActiveEnterTimestamp | cut -d= -f2
echo ""

echo "Memory Usage:"
ps aux | grep dinerod | grep -v grep | awk '{print $6/1024 " MB"}'
echo ""

echo "CPU Usage:"
ps aux | grep dinerod | grep -v grep | awk '{print $3 "%"}'
echo ""

echo "RPC Test:"
cd ~/Dinero-Coin
./build/dinero-cli blockchain.getblockcount 2>/dev/null && echo "✅ RPC responding" || echo "❌ RPC not responding"
echo ""

echo "Recent Errors (last 24h):"
sudo journalctl -u dinerod -p err --since "24 hours ago" | wc -l
echo ""

echo "═══════════════════════════════════════════════"
```

Run daily:
```bash
chmod +x ~/check_stability.sh
./check_stability.sh
```

Or schedule with cron (daily at 9 AM):
```bash
echo "0 9 * * * /home/tower/check_stability.sh >> /home/tower/stability_log.txt 2>&1" | crontab -
```
