# 🚀 Deploy to Production - Quick Start

## TL;DR - 3 Commands

```bash
# 1. Build fresh binaries
cd ~/Documents/DineroCoin
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target dinerod dinero-cli

# 2. Edit server list (if needed)
nano deploy_to_production.sh  # Update NODES array

# 3. Deploy
./deploy_to_production.sh
```

## What Gets Deployed

### New Features (Last 48 Hours)
1. **Node Identity System** - Cryptographic node signatures
2. **HTTP /serverinfo Endpoint** - Public autodiscovery endpoint
3. **Auto-Refresh** - ServerInfo updates every 10 minutes
4. **Multi-Asset Escrows** - Cross-asset trading support

### Files Deployed
- `dinerod` (daemon binary)
- `dinero-cli` (CLI tool)

### Files Created on Remote Servers
- `~/.dinero/node_identity.dat` (32 bytes, persistent keypair)
- `~/.dinero/serverinfo.json` (updated with signature)

## Pre-Flight Check

```bash
# Verify you have the latest code
cd ~/Documents/DineroCoin
git log --oneline -5

# Check binaries exist
ls -lh build/src/dinerod build/src/dinero-cli
```

## Deployment Steps

### 1. Configure Server List

Edit `deploy_to_production.sh` line 13-18:

```bash
NODES=(
    "root@173.249.195.59:/opt/dinero"          # Virginia
    "root@172.93.160.131:/opt/dinero"          # California
    # "admin@192.168.1.100:/home/admin/dinero" # Mac Mini
)
```

### 2. Run Deployment

```bash
./deploy_to_production.sh
```

**What it does:**
1. Verifies local binaries
2. Generates checksums
3. Backs up existing binaries on remote servers
4. Uploads new binaries
5. Stops daemons
6. Replaces binaries
7. Starts daemons
8. Verifies deployment

### 3. Post-Deployment Verification

```bash
# Test Virginia node
curl http://173.249.195.59:21999/serverinfo | python3 -m json.tool

# Should show:
# - "node_id": "..." (new!)
# - "node_pubkey": "..." (new!)
# - "signature": "..." (new!)
```

## Expected Output

```json
{
  "node_id": "3cb7355b75d14eb772e49d382c13407c7776836c",
  "node_pubkey": "02565ec96d34f2b5d1eb0ce15b6c2fac252c0a8046b84a3a7ed282ebfba81de1f0",
  "signature": "3045022100e41bd43141c89426fb985405b22feec9771c4de06a98a36d85df52ba019691c3...",
  "network": "mainnet",
  "rpc": {
    "host": "173.249.195.59",
    "port": 21999
  },
  "features": ["websocket", "multiasset", "bridge", "contracts"]
}
```

## Troubleshooting

### Issue: "Permission denied (publickey)"
**Fix:** Ensure SSH keys are set up for root access:
```bash
ssh-copy-id root@173.249.195.59
```

### Issue: "node_identity.dat not created"
**Fix:** Check daemon logs:
```bash
ssh root@173.249.195.59 "tail -100 ~/.dinero/debug.log | grep NodeIdentity"
```

### Issue: "/serverinfo returns 404"
**Fix:** Verify RPC is enabled in `dinero.conf`:
```bash
ssh root@173.249.195.59 "cat ~/.dinero/dinero.conf"
```
Must have:
```
server=1
rpcuser=...
rpcpassword=...
```

### Issue: "Signature missing from serverinfo.json"
**Fix:** Restart daemon and wait 30 seconds:
```bash
ssh root@173.249.195.59 "sudo systemctl restart dinerod"
sleep 30
ssh root@173.249.195.59 "cat ~/.dinero/serverinfo.json | grep signature"
```

## Rollback

If deployment fails:

```bash
# For each server:
ssh root@173.249.195.59 "sudo systemctl stop dinerod"
ssh root@173.249.195.59 "cd /opt/dinero && ls -lt dinerod.backup.* | head -1"
ssh root@173.249.195.59 "cd /opt/dinero && cp dinerod.backup.20251104_040000 dinerod"
ssh root@173.249.195.59 "sudo systemctl start dinerod"
```

## Next Steps After Deployment

1. **Register nodes with global registry:**
   ```bash
   cd ~/Documents/DineroCoin/registry
   python3 dinero_registry_extended.py \
     --port 8080 \
     -i 30 \
     -n "http://173.249.195.59:21999/serverinfo" \
     -n "http://172.93.160.131:21999/serverinfo"
   ```

2. **Monitor registry dashboard:**
   ```bash
   open http://localhost:8080/
   ```

3. **Check node health:**
   ```bash
   curl http://localhost:8080/api/status | python3 -m json.tool
   ```

## Complete Documentation

For detailed information, see:
- `PRODUCTION_UPDATE_GUIDE.md` - Full deployment guide
- `registry/NODE_REGISTRATION_GUIDE.md` - Registry setup
- `registry/IMPLEMENTATION_SUMMARY.md` - Registry features

## Support Commands

```bash
# View remote daemon logs
ssh root@173.249.195.59 "tail -50 ~/.dinero/debug.log"

# Check daemon process
ssh root@173.249.195.59 "ps aux | grep dinerod"

# View serverinfo.json
ssh root@173.249.195.59 "cat ~/.dinero/serverinfo.json | python3 -m json.tool"

# Check node_identity.dat
ssh root@173.249.195.59 "ls -la ~/.dinero/node_identity.dat"
ssh root@173.249.195.59 "hexdump -C ~/.dinero/node_identity.dat | head -3"
```

---

**Build once. Deploy everywhere. Monitor everything.**
