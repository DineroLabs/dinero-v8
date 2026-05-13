# 🚀 Dinero Deployment - Quick Reference

## ⚡ First Time Setup (5 minutes)

```bash
# 1. Set up bare repo on Virginia
./setup_remote_repos.sh

# 2. Commit & push
git add .
git commit -m "Mainnet deployment ready"
git push virginia feat/sqlite-raii  # or 'main' if merged

# 3. SSH to Virginia, clone & build
ssh root@173.249.195.59
cd ~ && git clone ~/repos/dinero.git DineroCoin
cd DineroCoin && ./linux_build.sh

# 4. SSH to California, clone & build
ssh root@172.93.160.131
cd ~ && git clone ssh://root@173.249.195.59/~/repos/dinero.git DineroCoin
cd DineroCoin && ./linux_build.sh
```

✅ Done! Both servers now have native Linux x86-64 binaries.

---

## 🔄 Daily Workflow

### Every code change:

```bash
# On Mac
git add .
git commit -m "Fix consensus bug"
git push virginia feat/sqlite-raii
./deploy_all.sh --restart          # Deploy + restart + auto-verify
```

That's it! Both servers automatically pull, rebuild, restart, and verify versions match.

### Quick version check:

```bash
./deploy_all.sh --verify           # Just check, don't deploy
dinero-cli -rpcport=20998 node.info | grep git_commit
```

---

## 🛠 Manual Operations

### Check daemon status (any server):
```bash
ssh root@173.249.195.59
~/DineroCoin/build/bin/dinero-cli -rpcport=20998 getblockchaininfo
```

### View logs:
```bash
ssh root@173.249.195.59
tail -f ~/dinero-data/daemon.log
```

### Restart daemon manually:
```bash
ssh root@173.249.195.59
pkill -9 dinerod
~/DineroCoin/build/bin/dinerod -daemon -datadir=~/dinero-data -rpcport=20998
```

### Rebuild just one server:
```bash
ssh root@173.249.195.59
cd ~/DineroCoin && git pull && ./linux_build.sh
```

---

## 🚨 Emergency Rollback

```bash
ssh root@173.249.195.59
cd ~/DineroCoin
git log --oneline -5            # Find last good commit
git reset --hard abc1234        # Roll back to good commit
./linux_build.sh                # Rebuild
pkill -9 dinerod                # Kill old daemon
./build/bin/dinerod -daemon -datadir=~/dinero-data  # Start fresh
```

---

## 📊 Architecture

```
Mac (ARM64)           →  Virginia (x86-64)      →  California (x86-64)
- Write code              - Git hub (bare)          - Production node
- Git commit              - Production node         - Native build
- Git push                - Native build
```

**Key point**: Mac ARM64 binaries ≠ Linux x86-64 binaries  
**Solution**: Each machine compiles native code

---

## 🔐 Security Checklist

- ✅ No GitHub (fully private)
- ✅ SSH key authentication
- ✅ `.gitignore` excludes sensitive data (wallet.db, .cookie, keys)
- ✅ Bare repo only accessible from your servers
- ✅ No secrets in version control

---

## 🎯 Server Details

| Server | IP | Role | Data Dir |
|--------|-----|------|----------|
| Virginia | 173.249.195.59 | Primary + Git hub | `~/dinero-data` |
| California | 172.93.160.131 | Secondary | `~/dinero-data` |

Both run: `~/DineroCoin/build/bin/dinerod -daemon -datadir=~/dinero-data -rpcport=20998`

---

## ✅ Success Indicators

After `./deploy_all.sh --restart`, the script automatically verifies:

```bash
# Automatic verification output:
🔍 Verifying version consistency across all nodes
📍 Local (Mac):      abc123def456...
📍 Virginia (repo):  abc123def456...
   Virginia (daemon): abc123def456...
📍 California (repo):  abc123def456...
   California (daemon): abc123def456...

✅ All nodes are synchronized and running matching versions!
```

### Manual health check:

```bash
./health_check.sh   # Shows version + block height + sync status
```

---

---

## 🏥 Health Monitoring (NEW!)

### Set up automated monitoring:

```bash
./setup_health_monitoring.sh   # One-time setup
tail -f ~/dinero-health.log     # Watch health checks
```

**What it monitors** (every 30 minutes):
- ✅ Daemons running
- ✅ Versions synchronized
- ✅ Block heights synchronized
- ✅ Alerts via email/Slack (optional)

---

## 📚 Full Documentation

- `VERSION_TRACKING_COMPLETE.md` - Version tracking & health monitoring
- `DEPLOYMENT_WORKFLOW.md` - Complete deployment architecture
- `DEPLOYMENT_CHECKLIST.md` - One-time setup steps

