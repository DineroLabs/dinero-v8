# Dinero Server Deployment - Executive Summary

## 📦 What Needs to Be Deployed

Your Linux servers are missing **48 hours of development work**. Here's what needs to be updated:

### Critical Updates
1. **HTTP Server Integration** - New HTTP endpoint alongside RPC
2. **serverinfo.json Endpoint** - Node metadata API
3. **Global Node Registry** - Network-wide monitoring system
4. **Configuration Updates** - New dinero.conf settings

## 🎯 Deployment Goal

Transform your isolated nodes into a **coordinated, monitored network** with:
- ✅ Public HTTP API on each node
- ✅ Real-time network monitoring dashboard
- ✅ Automatic node discovery for wallets/services
- ✅ Transparent network health metrics

## 🖥️ Server Deployment Plan

### Virginia (173.249.195.59) - PRIMARY
**Role**: Main node + Registry host

**Tasks**:
1. Update daemon code (rebuild)
2. Update dinero.conf
3. Deploy registry server
4. Configure nginx (optional)
5. Open firewall ports

**Expected Downtime**: ~10 minutes

### California (172.93.160.131) - SECONDARY
**Role**: Node

**Tasks**:
1. Update daemon code (rebuild)
2. Update dinero.conf
3. Enable auto-registration with Virginia
4. Open firewall ports

**Expected Downtime**: ~10 minutes

### Frankfurt (if applicable) - TERTIARY
**Role**: Node

**Tasks**: Same as California

## ⚡ Quick Deployment Options

### Option 1: Automated Script (RECOMMENDED)

**On each server, run:**
```bash
cd ~/DineroCoin
./deploy_updates.sh <server_name> <server_ip>
```

**Examples:**
```bash
# Virginia
./deploy_updates.sh Virginia 173.249.195.59

# California
./deploy_updates.sh California 172.93.160.131
```

**Time**: ~15 minutes per server (includes build)

### Option 2: Manual Step-by-Step

Follow detailed instructions in `DEPLOY_TO_SERVERS.md`

**Time**: ~30 minutes per server

## 📋 Pre-Deployment Checklist

Before you start:

- [ ] SSH access to all servers verified
- [ ] Git credentials configured
- [ ] Sufficient disk space (~2GB for builds)
- [ ] Backup of current dinero.conf exists
- [ ] Current blockchain height noted (for verification)
- [ ] Scheduled maintenance window (if needed)

## 🔥 Required Firewall Changes

### On ALL Servers
```bash
sudo ufw allow 21999/tcp  # HTTP/RPC
sudo ufw allow 21000/tcp  # WebSocket
sudo ufw allow 20999/tcp  # P2P
```

### On Virginia (Registry Host)
```bash
sudo ufw allow 8080/tcp   # Registry
# OR
sudo ufw allow 443/tcp    # HTTPS (if using nginx)
sudo ufw allow 80/tcp     # HTTP redirect
```

## ✅ Post-Deployment Verification

### On Each Node

**Test 1: Daemon Running**
```bash
~/DineroCoin/build/dinero-cli getinfo
```
Expected: Current block height, peer count

**Test 2: HTTP Server**
```bash
curl http://localhost:21999/serverinfo.json
```
Expected: JSON with node metadata

**Test 3: External Access**
```bash
curl http://YOUR_IP:21999/serverinfo.json
```
Expected: Same JSON as Test 2

### On Virginia (Registry)

**Test 4: Registry Running**
```bash
sudo systemctl status dinero-registry
```
Expected: Active (running)

**Test 5: Registry API**
```bash
curl http://localhost:8080/api/status
```
Expected: JSON with node count

**Test 6: Dashboard**
```
Open: http://173.249.195.59:8080/
```
Expected: Web page showing all nodes

## 📊 Success Criteria

Deployment is successful when:

✅ All daemons responding to RPC commands
✅ HTTP endpoints accessible on all nodes
✅ serverinfo.json returns valid JSON
✅ External HTTP access working (firewall open)
✅ Registry service running on Virginia
✅ Registry dashboard shows 2+ nodes
✅ No errors in logs
✅ Blockchain still syncing/synced

## ⏱️ Deployment Timeline

| Phase | Duration | Description |
|-------|----------|-------------|
| **Preparation** | 5 min | Review docs, prepare commands |
| **Virginia Deploy** | 15 min | Update code, rebuild, configure registry |
| **Virginia Test** | 5 min | Verify all endpoints working |
| **California Deploy** | 15 min | Update code, rebuild, configure |
| **California Test** | 5 min | Verify endpoints, check registry |
| **Frankfurt Deploy** | 15 min | Update code, rebuild, configure |
| **Frankfurt Test** | 5 min | Verify endpoints, check registry |
| **Final Verification** | 10 min | Full network test, documentation |
| **TOTAL** | ~70 min | Complete deployment across all servers |

## 🔄 Rollback Plan

If something goes wrong:

```bash
# 1. Stop new daemon
~/DineroCoin/build/dinero-cli stop

# 2. Find backup
BACKUP=$(ls -dt ~/dinero_backup_* | head -1)

# 3. Start from backup
cd $BACKUP/DineroCoin
./build/dinerod -daemon

# 4. Restore config
cp ~/.dinero/dinero.conf.backup.* ~/.dinero/dinero.conf
```

**Rollback Time**: ~5 minutes

## 🎯 What You'll Have After Deployment

### Network Map
```
┌─────────────────────────────────────────────────┐
│  Virginia (173.249.195.59)                     │
│  ┌──────────────┐        ┌─────────────────┐   │
│  │  Daemon      │        │  Registry       │   │
│  │  :21999      │◄──────►│  :8080          │   │
│  └──────────────┘        └─────────────────┘   │
│         ▲                         ▲             │
│         │                         │             │
└─────────┼─────────────────────────┼─────────────┘
          │                         │
          │   ┌─────────────────────┘
          │   │
┌─────────▼───▼───────────────────────────────────┐
│  California (172.93.160.131)                    │
│  ┌──────────────┐                               │
│  │  Daemon      │  (Registers with Virginia)    │
│  │  :21999      │                               │
│  └──────────────┘                               │
└─────────────────────────────────────────────────┘
```

### Public Endpoints

**Virginia:**
- http://173.249.195.59:21999/serverinfo.json - Node info
- http://173.249.195.59:8080/ - Registry dashboard
- http://173.249.195.59:8080/nodes.json - All nodes
- http://173.249.195.59:8080/api/status - Quick status

**California:**
- http://172.93.160.131:21999/serverinfo.json - Node info

**Frankfurt:**
- http://FRANKFURT_IP:21999/serverinfo.json - Node info

### Capabilities Enabled

1. **GUI Wallets** can auto-discover nodes:
```javascript
const nodes = await fetch('http://173.249.195.59:8080/nodes.json');
const fastest = nodes.sort((a,b) => a.latency_ms - b.latency_ms)[0];
```

2. **Services** can monitor network health
3. **Community** can see transparent network status
4. **New nodes** can self-register automatically

## 📞 Support During Deployment

### If You Get Stuck

1. **Check logs**:
```bash
tail -f ~/.dinero/debug.log
sudo journalctl -u dinero-registry -f
```

2. **Review documentation**:
- `DEPLOY_TO_SERVERS.md` - Detailed steps
- `SERVER_QUICK_REFERENCE.md` - Quick commands
- `DEPLOYMENT_SUMMARY.md` - This file

3. **Common issues** documented in troubleshooting sections

4. **Rollback** if needed (5 minutes)

## 📝 Deployment Checklist

Use this during deployment:

### Virginia
- [ ] SSH connected
- [ ] Backed up current installation
- [ ] Pulled latest code
- [ ] Built successfully
- [ ] Updated dinero.conf
- [ ] Restarted daemon
- [ ] HTTP endpoint working
- [ ] serverinfo.json accessible
- [ ] Deployed registry server
- [ ] Registry service active
- [ ] Registry showing nodes
- [ ] Firewall configured
- [ ] External access verified

### California
- [ ] SSH connected
- [ ] Backed up current installation
- [ ] Pulled latest code
- [ ] Built successfully
- [ ] Updated dinero.conf
- [ ] Restarted daemon
- [ ] HTTP endpoint working
- [ ] serverinfo.json accessible
- [ ] Appears in Virginia registry
- [ ] Firewall configured
- [ ] External access verified

### Frankfurt
- [ ] (Same as California)

## 🎉 Success Indicators

When complete, you should see:

### In Your Browser
Visit: `http://173.249.195.59:8080/`

You'll see a dashboard showing:
```
Node Name    Network    Peers    WebSocket    Uptime    Latency
Virginia    mainnet    8        21000        24h       42ms
California  mainnet    12       21000        12h       38ms
```

### Via API
```bash
curl http://173.249.195.59:8080/api/status
```

Returns:
```json
{
  "status": "ok",
  "total_nodes_alive": 2,
  "total_nodes_configured": 3,
  "last_update": "2025-11-03T12:00:00Z"
}
```

### Via CLI
```bash
curl http://173.249.195.59:21999/serverinfo.json
curl http://172.93.160.131:21999/serverinfo.json
```

Both return valid JSON with node information.

## 🚀 Ready to Deploy?

1. **Read**: Review this summary (you are here ✓)
2. **Prepare**: Open SSH to Virginia server
3. **Execute**: Run deployment script
4. **Verify**: Check all endpoints
5. **Repeat**: Deploy to California, then Frankfurt
6. **Celebrate**: You now have a monitored Dinero network! 🎉

---

**Start with**: `./deploy_updates.sh Virginia 173.249.195.59`

**Total Time**: ~70 minutes for all servers

**Risk Level**: Low (automatic backups, easy rollback)

**Benefit**: High (network monitoring, auto-discovery, transparency)

---

**Questions?** Check `SERVER_QUICK_REFERENCE.md` for quick commands and troubleshooting.
