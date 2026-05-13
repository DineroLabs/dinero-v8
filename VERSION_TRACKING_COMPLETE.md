# ✅ Version Tracking & Health Monitoring - COMPLETE

**Date**: November 8, 2025  
**Status**: ✅ Production-ready version tracking system

---

## 🎯 Features Implemented

### 1️⃣ **Build Commit Hash in dinerod**
- ✅ CMake captures full + short git commit hash at build time
- ✅ Exposed via `node.info` RPC method
- ✅ Shows: version, git_commit (full SHA), build_date

### 2️⃣ **`--verify` Flag in deploy_all.sh**
- ✅ Checks git commit consistency across all nodes
- ✅ Compares: Mac (local) ↔ Virginia (repo) ↔ California (repo)
- ✅ Detects: running daemon version vs. built version
- ✅ Reports: version mismatches and suggests actions

### 3️⃣ **Automated Health Check (Cron)**
- ✅ Monitors all nodes every 30 minutes
- ✅ Detects: version drift, daemon crashes, sync issues
- ✅ Alerts: email + Slack notifications (optional)
- ✅ Logs: continuous health status to `~/dinero-health.log`

---

## 🚀 Quick Start

### Check Version Info

```bash
# From any server running dinerod
dinero-cli -rpcport=20998 node.info

# Expected output:
{
  "version": "1.0.0",
  "git_commit": "abc123def456...",  # Full 40-char SHA
  "build_date": "2025-11-08T17:45:00+0000",
  ...
}
```

### Verify Version Consistency

```bash
# From Mac
cd ~/Documents/DineroCoin
./deploy_all.sh --verify

# Output shows:
# 📍 Local (Mac):      abc123def456...
# 📍 Virginia (repo):  abc123def456...
# 📍 Virginia (daemon): abc123def456...
# 📍 California (repo):  abc123def456...
# 📍 California (daemon): abc123def456...
# ✅ All nodes are synchronized!
```

### Set Up Health Monitoring

```bash
# One-time setup on Mac
./setup_health_monitoring.sh

# This creates cron job that runs every 30 minutes
# Logs to ~/dinero-health.log
```

---

## 📋 Complete Workflow

### Daily Deployment with Version Verification

```bash
# 1. Make code changes on Mac
vim src/consensus/pow.cpp

# 2. Commit changes
git add .
git commit -m "Improve difficulty adjustment"

# 3. Push to Virginia
git push virginia feat/sqlite-raii

# 4. Deploy + restart + verify
./deploy_all.sh --restart

# Output will show:
# 🚀 Deploying to Virginia...
# ✅ Build complete (commit: abc123def456)
# 🚀 Deploying to California...
# ✅ Build complete (commit: abc123def456)
# 🔍 Verifying version consistency...
# ✅ All nodes are synchronized and running matching versions!
```

### Manual Version Check

```bash
# Just verify without deploying
./deploy_all.sh --verify

# If mismatch detected:
# ⚠️  WARNING: California daemon is running old version (needs restart)
# → Run: ./deploy_all.sh --restart
```

### View Health Check Logs

```bash
# Tail live health checks
tail -f ~/dinero-health.log

# Sample output:
# [2025-11-08 18:00:00] Health Check:
#   Local (Mac):         abc123def456
#   Virginia (repo):     abc123def456
#   Virginia (daemon):   abc123def456 @ height 1234
#   California (repo):   abc123def456
#   California (daemon): abc123def456 @ height 1234
# [2025-11-08 18:00:00] ✅ All nodes healthy and synchronized
```

---

## 🔧 Technical Implementation

### 1. CMake Version Capture

```cmake
# CMakeLists.txt (lines 28-50)
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  OUTPUT_VARIABLE GIT_HASH ...)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
                  OUTPUT_VARIABLE GIT_HASH_FULL ...)
endif()

target_compile_definitions(dinerod PRIVATE
  DINERO_CLI_VERSION="${PROJECT_VERSION}"
  DINERO_CLI_GIT_SHA="${GIT_HASH}"
  DINERO_GIT_COMMIT_FULL="${GIT_HASH_FULL}"
  DINERO_CLI_BUILD_DATE="${BUILD_TIME}"
)
```

### 2. Version Exposure in RPC

```cpp
// src/rpc/diagnostics_rpc_handlers.cpp
Json::Value rpc_node_info(...) {
    auto version_info = dinero::cli::getVersionInfo();
    result["version"] = version_info.version;
    result["git_commit"] = version_info.gitSha;  // Full SHA
    result["build_date"] = version_info.buildDate;
    ...
}
```

### 3. Version Helper Functions

```cpp
// include/cli/version.h
struct VersionInfo {
    std::string version;      // "1.0.0"
    std::string gitSha;       // Full 40-char commit hash
    std::string buildDate;    // ISO timestamp
    std::string schemaTag;    // "din.cli.v1"
};

VersionInfo getVersionInfo();  // Called by RPC handlers
```

---

## 🎛️ Scripts Reference

### `deploy_all.sh`

**Purpose**: Deploy to all production nodes with version verification

**Usage**:
```bash
./deploy_all.sh              # Deploy without restarting daemons
./deploy_all.sh --restart    # Deploy and restart daemons
./deploy_all.sh --verify     # Only verify (no deployment)
```

**What it does**:
1. SSH to each server
2. `git pull` latest code
3. Build native x86-64 binaries
4. (Optional) Restart daemon
5. Verify version consistency across all nodes

**Output**:
- Shows git commit hash of each build
- Compares all nodes
- Warns if any mismatch detected

---

### `health_check.sh`

**Purpose**: Monitor node health and version consistency

**Usage**:
```bash
./health_check.sh                    # Run manually
# Or let cron run it automatically
```

**Checks**:
- ✅ Daemons are running
- ✅ Git repos are synchronized
- ✅ Daemons running correct version
- ✅ Block heights are synchronized (< 10 blocks apart)

**Alerts on**:
- Daemon crash
- Version drift
- Repo desync
- Blockchain sync issues

**Configuration** (edit top of script):
```bash
ALERT_EMAIL="your@email.com"                    # Optional: email alerts
SLACK_WEBHOOK="https://hooks.slack.com/..."     # Optional: Slack alerts
```

---

### `setup_health_monitoring.sh`

**Purpose**: One-time setup for automated health checks

**Usage**:
```bash
./setup_health_monitoring.sh
```

**What it does**:
1. Adds cron job: `*/30 * * * * ~/DineroCoin/health_check.sh`
2. Runs test health check
3. Shows configuration instructions

**Cron schedule**: Every 30 minutes (customize as needed)

---

## 🚨 Troubleshooting Scenarios

### Scenario 1: Version Mismatch After Deployment

```bash
$ ./deploy_all.sh --verify
⚠️  WARNING: California daemon is running old version (needs restart)

# Solution:
./deploy_all.sh --restart
```

### Scenario 2: Repo Desync Between Nodes

```bash
⚠️  WARNING: Virginia and California repos are out of sync

# Solution:
ssh root@172.93.160.131
cd ~/DineroCoin
git fetch origin
git reset --hard origin/main  # Or your branch
./linux_build.sh
```

### Scenario 3: Health Check Shows Daemon Crash

```bash
[2025-11-08 20:00:00] 🚨 ALERT: Virginia daemon is NOT RUNNING

# Solution:
ssh root@173.249.195.59
cd ~/DineroCoin
./build/bin/dinerod -daemon -datadir=~/dinero-data -rpcport=20998
```

### Scenario 4: Block Height Divergence

```bash
[2025-11-08 21:00:00] 🚨 ALERT: Block height DIVERGENCE: Virginia @ 1500, California @ 1450

# Possible causes:
# - Network partition
# - One node stalled
# - Chain reorg

# Solution:
# Check both daemons, restart if needed
./deploy_all.sh --restart
```

---

## 📊 Health Check Alert Examples

### Email Alert (if configured)

```
Subject: Dinero Health Check Alert

🚨 Dinero Alert: California daemon running OLD VERSION 
(daemon: abc123, repo: def456)
```

### Slack Alert (if configured)

```
🚨 Dinero Alert: Virginia and California repos are OUT OF SYNC 
(VA: abc123, CA: def456)
```

### Log Entry (always)

```
[2025-11-08 22:00:00] 🚨 ALERT: Virginia daemon is NOT RUNNING
[2025-11-08 22:00:00] Health Check:
  Local (Mac):         abc123def456
  Virginia (repo):     abc123def456
  Virginia (daemon):   not_running
  California (repo):   abc123def456
  California (daemon): abc123def456 @ height 1600
```

---

## ✅ Benefits

### Version Tracking
- ✅ **Traceability** - Always know exactly what code is running
- ✅ **Debugging** - Link bugs to specific commits
- ✅ **Audit trail** - Complete deployment history
- ✅ **Rollback** - Know exact version to revert to

### Automated Verification
- ✅ **Consistency** - Catch version mismatches immediately
- ✅ **Confidence** - Deploy with verification in one command
- ✅ **No guessing** - Always know if nodes are in sync

### Health Monitoring
- ✅ **Proactive** - Catch issues before users report them
- ✅ **Alerting** - Get notified of problems immediately
- ✅ **Historical** - Log shows pattern of issues over time
- ✅ **Peace of mind** - Sleep well knowing system is monitored

---

## 🔮 Future Enhancements (Optional)

### 1. Web Dashboard
Create a simple web page showing real-time node status:
```bash
# Shows all nodes, versions, block heights, uptime
http://localhost:8080/health
```

### 2. Metrics Collection
Log version changes and alert frequency to Prometheus/Grafana

### 3. Auto-Recovery
Extend health_check.sh to automatically restart crashed daemons

### 4. Deploy Tags
Tag each production deployment in git:
```bash
git tag -a v1.2.3-prod -m "Deployed to production"
git push virginia v1.2.3-prod
```

---

## 📚 Documentation Files

| File | Purpose |
|------|---------|
| `VERSION_TRACKING_COMPLETE.md` | This file - complete guide |
| `DEPLOYMENT_WORKFLOW.md` | Overall deployment architecture |
| `DEPLOYMENT_QUICKSTART.md` | Daily reference card |
| `DEPLOYMENT_CHECKLIST.md` | One-time setup steps |

---

## 🎉 Summary

**What you can now do**:

1. **Always know** what version is running on each node
2. **Verify** version consistency with one command
3. **Get alerted** if nodes drift out of sync
4. **Deploy confidently** with automatic verification
5. **Monitor continuously** via automated health checks

**Commands to remember**:
```bash
./deploy_all.sh --restart   # Deploy + restart + verify
./deploy_all.sh --verify    # Just check versions
tail -f ~/dinero-health.log # Watch health checks
```

**✅ Your production infrastructure is now enterprise-grade!**

---

**Questions?** All scripts are self-documented with `--help` or comments at the top.

