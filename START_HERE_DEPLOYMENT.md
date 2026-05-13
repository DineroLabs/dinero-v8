# 🚀 START HERE - Deploying Recent Updates to Linux Servers

## ⚡ Quick Start (For the Impatient)

**You want to deploy the last 48 hours of work to your Linux servers?**

### On Each Server, Run This:

```bash
cd ~/DineroCoin
./deploy_updates.sh <server_name> <server_ip>
```

**Examples:**
```bash
# Virginia (main + registry)
./deploy_updates.sh Virginia 173.249.195.59

# California
./deploy_updates.sh California 172.93.160.131
```

**That's it!** ✅ The script handles everything automatically.

---

## 📚 Which File Should I Read?

You have **4 key files** for this deployment. Here's which one to use:

### 1. **DEPLOYMENT_SUMMARY.md** ← START HERE
**Read this first** - 5-minute executive summary

**Contains:**
- What's being deployed
- Timeline (70 minutes total)
- Success criteria
- Quick checklist

**Read if:** You want the big picture before starting

---

### 2. **deploy_updates.sh** ← THE SCRIPT
**Run this** - Automated deployment script

**Does:**
- Pulls latest code
- Rebuilds daemon
- Updates config
- Restarts services
- Runs tests
- Deploys registry (Virginia only)

**Use if:** You want automated, hands-off deployment

---

### 3. **DEPLOY_TO_SERVERS.md** ← DETAILED GUIDE
**Reference this** - Complete step-by-step manual

**Contains:**
- Manual deployment steps
- Troubleshooting guide
- Firewall configuration
- Rollback procedures
- Verification tests

**Read if:** Script fails or you want to understand details

---

### 4. **SERVER_QUICK_REFERENCE.md** ← QUICK COMMANDS
**Bookmark this** - Command reference card

**Contains:**
- SSH commands
- Testing commands
- Common fixes
- File locations
- Useful URLs

**Use if:** You need quick commands during/after deployment

---

## 🎯 Deployment Decision Tree

```
START
  │
  ├─ Want automated deployment?
  │  └─ YES → Run deploy_updates.sh
  │  └─ NO  → Follow DEPLOY_TO_SERVERS.md
  │
  ├─ Want overview first?
  │  └─ YES → Read DEPLOYMENT_SUMMARY.md
  │
  ├─ Need quick commands?
  │  └─ YES → Use SERVER_QUICK_REFERENCE.md
  │
  └─ Something broke?
     └─ YES → Check "Troubleshooting" in DEPLOY_TO_SERVERS.md
```

---

## 🚦 Deployment Steps (Simplified)

### Step 1: Prepare (5 minutes)
```bash
# Read deployment summary
cat DEPLOYMENT_SUMMARY.md

# SSH to first server
ssh user@173.249.195.59
```

### Step 2: Deploy Virginia (15 minutes)
```bash
cd ~/DineroCoin
./deploy_updates.sh Virginia 173.249.195.59
```

Wait for completion, verify tests pass.

### Step 3: Verify Virginia (5 minutes)
```bash
# Test HTTP endpoint
curl http://173.249.195.59:21999/serverinfo.json

# Test registry
curl http://173.249.195.59:8080/api/status
```

### Step 4: Deploy California (15 minutes)
```bash
# SSH to California
ssh user@172.93.160.131

cd ~/DineroCoin
./deploy_updates.sh California 172.93.160.131
```

### Step 5: Verify Network (5 minutes)
```bash
# Check registry shows both nodes
curl http://173.249.195.59:8080/nodes.json

# Should show Virginia + California
```

### Step 6: Celebrate! 🎉
You now have a monitored Dinero network!

---

## 📦 What's Being Deployed?

### New Features (Last 48 Hours)
1. **HTTP Server** - Full HTTP API on port 21999
2. **serverinfo.json** - Node metadata endpoint
3. **CORS Support** - Browser-friendly headers
4. **Global Registry** - Network monitoring system
5. **Web Dashboard** - Visual status page
6. **Self-Registration** - Auto node discovery

### Files Changed
- `src/httprpc.cpp` - HTTP server implementation
- `~/.dinero/dinero.conf` - New config options
- New: `registry/` directory (13 files)

---

## ✅ Success Checklist

After deployment, you should have:

### On All Servers
- [ ] Daemon running and synced
- [ ] HTTP server responding on :21999
- [ ] serverinfo.json accessible
- [ ] External access working (firewall open)
- [ ] No errors in logs

### On Virginia (Registry)
- [ ] Registry service active
- [ ] Dashboard accessible at :8080
- [ ] API showing 2+ nodes
- [ ] Web interface loading

---

## 🐛 If Something Goes Wrong

### Quick Fixes

**Problem**: Script fails to build
```bash
# Install missing dependencies
sudo apt-get update
sudo apt-get install -y build-essential libtool autotools-dev automake \
  pkg-config libssl-dev libevent-dev libboost-all-dev
```

**Problem**: Can't access HTTP externally
```bash
# Open firewall
sudo ufw allow 21999/tcp
sudo ufw reload
```

**Problem**: Registry not seeing nodes
```bash
# Check registry logs
sudo journalctl -u dinero-registry -f

# Restart registry
sudo systemctl restart dinero-registry
```

**Problem**: Want to rollback
```bash
# Stop daemon
~/DineroCoin/build/dinero-cli stop

# Find backup
ls -dt ~/dinero_backup_* | head -1

# Start from backup
cd $(ls -dt ~/dinero_backup_* | head -1)/DineroCoin
./build/dinerod -daemon
```

### Get More Help
- Detailed troubleshooting: See `DEPLOY_TO_SERVERS.md` section "Troubleshooting"
- Quick commands: See `SERVER_QUICK_REFERENCE.md` section "Common Issues"

---

## 📞 What If I Just Want To...

**"I just want to see what's new"**
→ Read: `DEPLOYMENT_SUMMARY.md` - Section "What Needs to Be Deployed"

**"I just want to deploy quickly"**
→ Run: `./deploy_updates.sh <server_name> <ip>`

**"I want to understand everything first"**
→ Read: `DEPLOY_TO_SERVERS.md` - Full manual deployment guide

**"I need specific commands"**
→ Use: `SERVER_QUICK_REFERENCE.md` - Command reference

**"Something's broken, need to fix it"**
→ Check: `DEPLOY_TO_SERVERS.md` - Troubleshooting section

**"I want to rollback"**
→ See: `DEPLOY_TO_SERVERS.md` - Rollback Plan section

---

## 🎓 Understanding Your Files

You have many deployment files in this repo. Here's what's relevant:

### For THIS Deployment (HTTP + Registry)
✅ **deploy_updates.sh** - Use this
✅ **DEPLOYMENT_SUMMARY.md** - Read this
✅ **DEPLOY_TO_SERVERS.md** - Reference this
✅ **SERVER_QUICK_REFERENCE.md** - Bookmark this

### Old/Unrelated Deployment Files
❌ deploy_hard_fork.sh - Old hardfork deployment
❌ deploy_encryption_servers.sh - Old encryption update
❌ DEPLOY_TO_LINUX.md - Old generic guide
❌ deploy/ folder - Legacy deployment scripts

**Ignore the old files.** Use the NEW files listed above.

---

## 🎯 Your Deployment Path

### Recommended Order:

1. **Read** (5 min): `DEPLOYMENT_SUMMARY.md`
2. **Prepare** (5 min): Review checklist, open SSH
3. **Deploy Virginia** (15 min): Run `deploy_updates.sh`
4. **Verify Virginia** (5 min): Test endpoints
5. **Deploy California** (15 min): Run `deploy_updates.sh`
6. **Verify Network** (5 min): Check registry
7. **Document** (5 min): Note any issues
8. **Celebrate** (∞ min): You're done! 🎉

**Total Time**: ~55 minutes (less if builds are cached)

---

## 🚀 Ready to Start?

### Choose Your Path:

**Path A: Automated (Recommended)**
```bash
ssh user@173.249.195.59
cd ~/DineroCoin
./deploy_updates.sh Virginia 173.249.195.59
```

**Path B: Manual**
```bash
ssh user@173.249.195.59
# Follow steps in DEPLOY_TO_SERVERS.md
```

**Path C: Learn First**
```bash
cat DEPLOYMENT_SUMMARY.md
cat DEPLOY_TO_SERVERS.md
# Then choose Path A or B
```

---

## 📋 Final Checklist Before You Begin

- [ ] Read `DEPLOYMENT_SUMMARY.md` (5 min)
- [ ] Have SSH access to all servers
- [ ] Noted current block height (for verification)
- [ ] Scheduled time (70 min for all servers)
- [ ] Opened terminal windows for each server
- [ ] Saved this file location for reference
- [ ] Ready to run `deploy_updates.sh`

---

## 🎉 After Deployment

You'll have:

1. **HTTP API** on each node (http://IP:21999/serverinfo.json)
2. **Registry Dashboard** (http://173.249.195.59:8080/)
3. **Network Monitoring** (Real-time node health)
4. **Auto-Discovery** (Wallets can find nodes)
5. **Transparency** (Public network status)

**Your Dinero network will be transformed from isolated nodes into a coordinated, transparent ecosystem!**

---

**Questions?** Everything is documented in the 4 key files above.

**Let's deploy!** 🚀
