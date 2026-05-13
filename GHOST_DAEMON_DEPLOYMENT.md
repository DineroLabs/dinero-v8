# 🛡️ Ghost Daemon Prevention - Deployment Guide

**Status**: ✅ **IMPLEMENTED** - All components ready for deployment

---

## 📋 Overview

The ghost daemon prevention system consists of three layers:

1. **Runtime Self-Identification** - Build info logged at startup and exposed via metrics
2. **Safe Restart Script** - Production-grade restart script that prevents ghost daemons
3. **Watchdog Script** - Automated monitoring that detects and fixes mismatches

---

## ✅ What Was Implemented

### 1. Enhanced Build Info in getmetrics

**Location**: `src/daemon/rpc/telemetry_rpc_handlers.cpp`

**New Metric**:
```
dinero_build_info{commit="abc123",version="0.1.0",build_time="2025-01-...",checksum="ff27..."} 1
```

**Query Example**:
```bash
curl -s http://127.0.0.1:20997/metrics | grep dinero_build_info
```

### 2. Production Restart Script

**Location**: `scripts/restart_dinero.sh`

**Features**:
- Kills all existing dinerod processes
- Cleans up stale locks and sockets
- Verifies binary exists before starting
- Starts daemon with proper logging
- Verifies RPC endpoint responds

**Usage**:
```bash
./scripts/restart_dinero.sh [datadir] [rpcport] [port] [wsport]
```

**Example**:
```bash
./scripts/restart_dinero.sh /root/.dinero 20997 20999 21001
```

### 3. Watchdog Script

**Location**: `scripts/check_dinero_version.sh`

**Features**:
- Checks if daemon is running
- Verifies RPC endpoint responds
- Extracts consensus checksum from metrics
- Compares against expected checksum
- Auto-restarts if mismatch detected

**Usage**:
```bash
./scripts/check_dinero_version.sh [expected_checksum] [rpc_port]
```

**Example**:
```bash
./scripts/check_dinero_version.sh ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430 20997
```

---

## 🚀 Deployment Steps

### Step 1: Deploy Scripts to Production Servers

```bash
# Copy scripts to production servers
scp scripts/restart_dinero.sh root@173.249.195.59:/usr/local/bin/
scp scripts/check_dinero_version.sh root@173.249.195.59:/usr/local/bin/

# Make executable
ssh root@173.249.195.59 'chmod +x /usr/local/bin/restart_dinero.sh /usr/local/bin/check_dinero_version.sh'
```

### Step 2: Get Expected Consensus Checksum

On each server, after deploying the correct build:

```bash
# Start daemon
/usr/local/bin/restart_dinero.sh

# Get consensus checksum
curl -s http://127.0.0.1:20997/metrics | grep dinero_consensus_info
# Should output: dinero_consensus_info{checksum="ff279196e33c326f..."} 1

# Extract checksum
EXPECTED_CHECKSUM=$(curl -s http://127.0.0.1:20997/metrics | grep -oP 'dinero_consensus_info{checksum="\K[^"]+')
echo "Expected checksum: ${EXPECTED_CHECKSUM}"
```

### Step 3: Configure Watchdog Script

Edit `/usr/local/bin/check_dinero_version.sh` on each server:

```bash
# Set expected checksum at top of script
EXPECTED_CHECKSUM="ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430"
```

Or pass as argument in cron:
```bash
*/5 * * * * /usr/local/bin/check_dinero_version.sh ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430 20997 >> /root/dinero_watchdog.log 2>&1
```

### Step 4: Set Up Cron Job

```bash
# Edit crontab
crontab -e

# Add this line (runs every 5 minutes)
*/5 * * * * /usr/local/bin/check_dinero_version.sh >> /root/dinero_watchdog.log 2>&1
```

### Step 5: Test the System

```bash
# Test restart script
/usr/local/bin/restart_dinero.sh

# Test watchdog script manually
/usr/local/bin/check_dinero_version.sh

# Check watchdog log
tail -f /root/dinero_watchdog.log

# Check metrics endpoint
curl -s http://127.0.0.1:20997/metrics | grep dinero_build_info
```

---

## 🔍 Monitoring & Verification

### Check Build Info via Metrics

```bash
# Get full build info
curl -s http://127.0.0.1:20997/metrics | grep dinero_build_info

# Expected output:
# dinero_build_info{commit="abc123",version="0.1.0",build_time="2025-01-...",checksum="ff27..."} 1
```

### Check Consensus Checksum

```bash
# Get consensus checksum
curl -s http://127.0.0.1:20997/metrics | grep dinero_consensus_info

# Expected output:
# dinero_consensus_info{checksum="ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430"} 1
```

### Check Startup Logs

```bash
# View daemon startup logs
tail -f /root/dinero.log | grep -E "Build ID|Consensus checksum|Dinero Daemon"
```

**Expected output**:
```
Dinero Daemon v0.1.0 (abc123)
Build ID: abc123
Built: 2025-01-XX...
🔐 Consensus checksum: ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430
```

---

## 🎯 Benefits

### Before (Without Protection)
- ❌ Ghost daemons running silently
- ❌ No way to verify which build is running
- ❌ Manual restart process prone to errors
- ❌ No automated detection of mismatches

### After (With Protection)
- ✅ Build ID logged at startup
- ✅ Full build info in metrics
- ✅ Automated restart script
- ✅ Watchdog detects mismatches
- ✅ Auto-healing via cron

---

## 📊 Monitoring Dashboard Queries

### Grafana Prometheus Queries

**Build Info Panel**:
```promql
dinero_build_info
```

**Consensus Checksum Alert**:
```promql
dinero_consensus_info{checksum!="ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430"}
```

**Version Info**:
```promql
dinero_version_info
```

---

## 🔧 Troubleshooting

### Issue: Watchdog script can't extract checksum

**Solution**: Check if metrics endpoint is responding:
```bash
curl -s http://127.0.0.1:20997/metrics | head -20
```

### Issue: Restart script fails to start daemon

**Solution**: Check binary path and permissions:
```bash
ls -la /root/DineroCoin/build/bin/dinerod
chmod +x /root/DineroCoin/build/bin/dinerod
```

### Issue: Multiple daemons still running

**Solution**: Manual cleanup:
```bash
pkill -9 dinerod
rm -f /root/.dinero/.lock
/usr/local/bin/restart_dinero.sh
```

---

## ✅ Verification Checklist

- [ ] Scripts deployed to `/usr/local/bin/`
- [ ] Scripts are executable (`chmod +x`)
- [ ] Expected checksum configured in watchdog script
- [ ] Cron job configured (runs every 5 minutes)
- [ ] Test restart script works
- [ ] Test watchdog script works
- [ ] Metrics endpoint returns build info
- [ ] Startup logs show build ID and checksum

---

## 📝 Summary

**All ghost daemon prevention components are now implemented:**

1. ✅ **Build ID** - Logged at startup, exposed in metrics
2. ✅ **Consensus Checksum** - Logged at startup, exposed in metrics
3. ✅ **Safe Restart Script** - Production-grade restart automation
4. ✅ **Watchdog Script** - Automated mismatch detection and healing
5. ✅ **Cron Integration** - Automated monitoring every 5 minutes

**The system is ready for deployment!** 🚀

