# ✅ Pre-Deployment Checklist

## Before You Deploy to Production

### Step 1: Verify Local Build

```bash
cd ~/Documents/DineroCoin

# Check you're on the right branch/commit
git status
git log --oneline -5

# Verify all new files are present
ls -la include/daemon/node_identity.h
ls -la src/daemon/node_identity.cpp

# Clean build
rm -rf build
mkdir build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dinerod dinero-cli

# Verify binaries exist and are recent
ls -lh build/src/dinerod build/src/dinero-cli
```

**Expected output:**
```
-rwxr-xr-x  1 user  staff   25M Nov  4 04:00 build/src/dinerod
-rwxr-xr-x  1 user  staff   12M Nov  4 04:00 build/src/dinero-cli
```

✅ Binaries built successfully
✅ Timestamps are recent (< 5 minutes old)

---

### Step 2: Local Testing

```bash
# Start test daemon
rm -rf /tmp/deploy-test
mkdir -p /tmp/deploy-test
./build/src/dinerod --regtest --rpcport=22999 --datadir=/tmp/deploy-test --daemon

# Wait for startup
sleep 10

# Test 1: Check node_identity.dat created
ls -la /tmp/deploy-test/node_identity.dat
# Expected: -rw------- 1 user staff 32 Nov 4 04:00 node_identity.dat

# Test 2: Check serverinfo.json has signature
cat /tmp/deploy-test/serverinfo.json | python3 -m json.tool | grep -E "node_id|signature"
# Expected: "node_id": "..." and "signature": "..."

# Test 3: Check /serverinfo endpoint
curl -s http://127.0.0.1:22999/serverinfo | python3 -m json.tool | head -10
# Expected: JSON with node_id and signature

# Test 4: Check logs
tail -20 /tmp/deploy-test/debug.log | grep -E "NodeIdentity|ServerInfo"
# Expected: "[NodeIdentity] Loaded existing node identity"
# Expected: "[ServerInfo] Auto-refresh thread started"

# Cleanup
./build/src/dinero-cli -rpcport=22999 -datadir=/tmp/deploy-test stop
```

✅ node_identity.dat created with correct permissions
✅ serverinfo.json contains signature
✅ /serverinfo endpoint returns valid JSON
✅ Logs show successful initialization

---

### Step 3: Configure Deployment Script

```bash
# Edit deployment script
nano deploy_to_production.sh

# Update NODES array (lines 13-18)
NODES=(
    "root@173.249.195.59:/opt/dinero"          # Virginia
    # "root@172.93.160.131:/opt/dinero"        # California (comment out for first deployment)
)

# Save and make executable
chmod +x deploy_to_production.sh
```

✅ Server addresses configured
✅ SSH paths verified
✅ Script is executable

---

### Step 4: Verify SSH Access

```bash
# Test SSH to each server
ssh root@173.249.195.59 "hostname && uptime"
# Expected: Server hostname and uptime

# Test sudo access (if using systemd)
ssh root@173.249.195.59 "sudo systemctl status dinerod"
# Expected: Service status (may be active or inactive)

# Verify target directory exists
ssh root@173.249.195.59 "ls -la /opt/dinero/"
# Expected: Current dinerod and dinero-cli binaries
```

✅ SSH access working
✅ Sudo access confirmed (if needed)
✅ Target directory exists

---

### Step 5: Backup Current Production

```bash
# Backup current binaries
ssh root@173.249.195.59 "cd /opt/dinero && \
  cp dinerod dinerod.pre-identity-$(date +%Y%m%d) && \
  cp dinero-cli dinero-cli.pre-identity-$(date +%Y%m%d)"

# Verify backups
ssh root@173.249.195.59 "ls -lah /opt/dinero/*.pre-identity*"
```

✅ Current binaries backed up
✅ Backup files verified

---

### Step 6: Check Production dinero.conf

```bash
# View current config
ssh root@173.249.195.59 "cat ~/.dinero/dinero.conf"

# Verify RPC is enabled
ssh root@173.249.195.59 "grep -E 'server=|rpcuser=|rpcpassword=' ~/.dinero/dinero.conf"

# Expected output:
# server=1
# rpcuser=...
# rpcpassword=...
```

✅ RPC server enabled
✅ Credentials configured
✅ No changes needed to config

---

### Step 7: Document Current State

```bash
# Record current block height
ssh root@173.249.195.59 "./dinero-cli getblockcount" > /tmp/pre-deploy-blockheight.txt

# Record current peers
ssh root@173.249.195.59 "./dinero-cli getpeerinfo | wc -l" > /tmp/pre-deploy-peers.txt

# Check existing serverinfo (if any)
ssh root@173.249.195.59 "cat ~/.dinero/serverinfo.json 2>/dev/null" > /tmp/pre-deploy-serverinfo.txt || echo "No existing serverinfo"
```

✅ Current state documented
✅ Baseline metrics recorded

---

### Step 8: Final Sanity Checks

```bash
# Verify binaries are for the correct architecture
file build/src/dinerod
# Expected: Mach-O 64-bit executable arm64 (or x86_64 depending on your Mac)

# Check binary sizes are reasonable
du -h build/src/dinerod
# Expected: ~20-30MB

# Verify no uncommitted changes
git status
# Expected: "working tree clean" or only documentation files changed
```

✅ Binary architecture correct
✅ Binary sizes reasonable
✅ No uncommitted critical changes

---

## Deployment Execution

Once all checks pass:

```bash
./deploy_to_production.sh
```

**Estimated time:** 2-3 minutes per server

**Watch for:**
- ✅ Green checkmarks at each step
- ❌ Red errors (investigate immediately)
- ⚠️ Yellow warnings (note but may be okay)

---

## Post-Deployment Verification

Within 60 seconds of deployment:

```bash
# 1. Check process is running
ssh root@173.249.195.59 "ps aux | grep dinerod | grep -v grep"

# 2. Check node_identity.dat created
ssh root@173.249.195.59 "ls -la ~/.dinero/node_identity.dat"

# 3. Wait for serverinfo.json
sleep 30
ssh root@173.249.195.59 "cat ~/.dinero/serverinfo.json | python3 -m json.tool"

# 4. Test /serverinfo endpoint
curl http://173.249.195.59:21999/serverinfo | python3 -m json.tool

# 5. Check logs for errors
ssh root@173.249.195.59 "tail -50 ~/.dinero/debug.log | grep -i error"

# 6. Verify block sync
ssh root@173.249.195.59 "./dinero-cli getblockcount"
# Should match pre-deployment height (or be slightly higher)
```

✅ Process running
✅ node_identity.dat exists
✅ serverinfo.json has signature
✅ /serverinfo endpoint accessible
✅ No errors in logs
✅ Blockchain syncing

---

## Rollback Criteria

**Roll back immediately if:**
- ❌ Daemon won't start after 30 seconds
- ❌ Blockchain stops syncing
- ❌ RPC commands fail
- ❌ Peer count drops to zero
- ❌ Critical errors in debug.log

**Rollback command:**
```bash
ssh root@173.249.195.59 "sudo systemctl stop dinerod && \
  cd /opt/dinero && \
  cp dinerod.pre-identity-$(date +%Y%m%d) dinerod && \
  sudo systemctl start dinerod"
```

---

## Success Criteria

Deployment is successful when:

✅ Daemon running for 5+ minutes
✅ Block height increasing
✅ Peer count stable (>= 3)
✅ node_identity.dat exists (32 bytes, 0600)
✅ serverinfo.json has valid signature
✅ GET /serverinfo returns 200 OK
✅ No critical errors in logs
✅ Auto-refresh thread running (check logs)

---

## Next Actions After Successful Deployment

1. **Monitor for 1 hour**
   ```bash
   watch -n 60 'curl -s http://173.249.195.59:21999/serverinfo | python3 -m json.tool'
   ```

2. **Register with global registry**
   ```bash
   cd registry
   python3 dinero_registry_extended.py \
     --port 8080 \
     -i 30 \
     -n "http://173.249.195.59:21999/serverinfo"
   ```

3. **Verify in registry dashboard**
   ```bash
   open http://localhost:8080/
   ```

4. **Deploy to additional servers**
   - Uncomment California node in deploy_to_production.sh
   - Run ./deploy_to_production.sh again

---

## Emergency Contacts

If deployment fails:
1. Check `~/.dinero/debug.log` on affected server
2. Review this checklist for missed steps
3. Consult `PRODUCTION_UPDATE_GUIDE.md` troubleshooting section
4. Roll back to backup binaries if needed

---

**Date:** _____________
**Performed by:** _____________
**Servers deployed:** _____________
**Deployment duration:** _____________
**Issues encountered:** _____________
**Resolution:** _____________

---

**Last Updated:** November 4, 2025
