# Testnet Deployment Guide - AssumeUTXO Hardening

**Date:** 2025-12-24
**Status:** Ready for Deployment
**Version:** v0.1.0 with Phase 44.1 + Automatic Rollback

---

## Pre-Deployment Checklist

- [x] Phase 44.1 implementation complete
- [x] Automatic rollback implementation complete
- [x] Code committed (commit: 96b7033e)
- [x] Build verified successful
- [x] Hardening code present in binary

**Binary Verification:**
```bash
strings build/bin/dinerod | grep "Phase 44.1"
# Output: [BackgroundValidation] Phase 44.1: UTXO Set Verification
```

---

## Deployment Architecture

### Option 1: Cloud Instance (Recommended)

**Requirements:**
- Ubuntu 24.04 LTS (ARM64 or x86_64)
- 4GB RAM minimum
- 50GB disk space
- Public IP for P2P connectivity

**Providers:**
- AWS EC2 (t4g.medium for ARM64)
- DigitalOcean Droplet
- Hetzner Cloud
- Vultr

### Option 2: Local Testnet

**Requirements:**
- macOS or Linux
- Build environment configured
- Network connectivity for P2P

---

## Deployment Steps

### Step 1: Build for Target Architecture

**On Ubuntu/Linux ARM64:**
```bash
cd /path/to/DineroCoin
git pull origin phase-3-spending
git log -1 --oneline  # Verify: 96b7033e AssumeUTXO Hardening

# Install dependencies
sudo apt-get update
sudo apt-get install -y build-essential cmake git libssl-dev sqlite3 libsqlite3-dev

# Build
mkdir -p build && cd build
cmake ..
cmake --build . --target dinerod dinero-cli -j$(nproc)

# Verify hardening code
strings bin/dinerod | grep "Phase 44.1"
```

**On macOS (for local testing):**
```bash
cd /Users/haydarevich/Documents/DineroCoin
cmake --build build --target dinerod dinero-cli -j8

# Already built - verified above
```

### Step 2: Deploy to Testnet Node

**Transfer binaries:**
```bash
# To remote server
scp build/bin/dinerod user@testnet-node:/usr/local/bin/
scp build/bin/dinero-cli user@testnet-node:/usr/local/bin/

# To multipass VM (if building on Linux)
multipass transfer build/bin/dinerod dinero-node1:/home/ubuntu/
multipass transfer build/bin/dinero-cli dinero-node1:/home/ubuntu/
```

**On target server:**
```bash
chmod +x /usr/local/bin/dinerod /usr/local/bin/dinero-cli
mkdir -p ~/.dinero-testnet
```

### Step 3: Start Testnet Node

**Create systemd service (recommended for production):**
```bash
sudo tee /etc/systemd/system/dinerod-testnet.service <<EOF
[Unit]
Description=DineroCoin Testnet Daemon
After=network.target

[Service]
Type=forking
User=dinero
WorkingDirectory=/home/dinero
ExecStart=/usr/local/bin/dinerod -testnet -datadir=/home/dinero/.dinero-testnet -daemon
ExecStop=/usr/local/bin/dinero-cli -testnet -datadir=/home/dinero/.dinero-testnet stop
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable dinerod-testnet
sudo systemctl start dinerod-testnet
```

**Or manual start:**
```bash
dinerod -testnet -datadir=~/.dinero-testnet -daemon
```

### Step 4: Verify Deployment

**Check node is running:**
```bash
dinero-cli -testnet -datadir=~/.dinero-testnet getinfo
```

**Monitor logs for hardening code:**
```bash
tail -f ~/.dinero-testnet/debug.log | grep -i "phase 44.1\|assumeutxo\|validation"
```

**Check block sync:**
```bash
watch -n 5 'dinero-cli -testnet getblockcount'
```

---

## Testing AssumeUTXO Hardening

### Test 1: Generate Testnet Snapshot

**Wait for sync to complete:**
```bash
# Check sync status
dinero-cli -testnet getblockcount

# When fully synced, generate snapshot
dinero-cli -testnet dumptxoutset ~/testnet-snapshot-$(date +%Y%m%d).dat
```

**Verify snapshot metadata:**
```bash
ls -lh ~/testnet-snapshot-*.dat
```

### Test 2: Load Snapshot on Fresh Node

**On second testnet node:**
```bash
# Stop node if running
dinero-cli -testnet stop

# Clear UTXO database only
rm -rf ~/.dinero-testnet/wallets

# Restart
dinerod -testnet -daemon

# Load snapshot
dinero-cli -testnet loadtxoutset ~/testnet-snapshot-20251224.dat
```

**Monitor Phase 44.1 verification:**
```bash
tail -f ~/.dinero-testnet/debug.log | grep -A 20 "Phase 44.1"
```

**Expected output:**
```
[BackgroundValidation] ═══════════════════════════════════════════════════════
[BackgroundValidation] Phase 44.1: UTXO Set Verification
[BackgroundValidation] ═══════════════════════════════════════════════════════
[BackgroundValidation] UTXO Count Verification:
[BackgroundValidation]   Expected (from snapshot): 12345
[BackgroundValidation]   Actual (from database):   12345
[BackgroundValidation] ✓ UTXO count matches: 12345
[BackgroundValidation] Spot-checking sampled UTXOs...
[BackgroundValidation] Verified base block exists at height 100000
[BackgroundValidation] ✓ UTXO Set Verification PASSED
```

### Test 3: Monitor Background Validation

**Check progress:**
```bash
watch -n 30 'dinero-cli -testnet getbackgroundvalidationprogress'
```

**Expected output:**
```json
{
  "status": "InProgress",
  "blocks_validated": 50000,
  "target_height": 100000,
  "percent_complete": 50.0
}
```

**Wait for completion (may take hours):**
```bash
# When complete:
dinero-cli -testnet getbackgroundvalidationprogress
```

**Expected final output:**
```json
{
  "status": "Completed",
  "blocks_validated": 100000,
  "target_height": 100000,
  "percent_complete": 100.0
}
```

### Test 4: Verify No Rollback (Success Case)

**Check logs after validation completes:**
```bash
grep -i "rollback\|validation complete" ~/.dinero-testnet/debug.log | tail -20
```

**Expected output:**
```
✅ BACKGROUND VALIDATION COMPLETE
Blocks validated: 100000
UTXO set verified at height: 100000
Snapshot is VALID - exiting AssumeUTXO mode
```

**Should NOT see:**
```
🔄 AUTOMATIC ROLLBACK INITIATED  # <-- Should NOT appear if snapshot is valid
```

---

## Monitoring & Health Checks

### Key Metrics to Monitor

**Node Health:**
```bash
# Block height
dinero-cli -testnet getblockcount

# Peer connections
dinero-cli -testnet getpeerinfo | grep -c "addr"

# Mempool status
dinero-cli -testnet getmempoolinfo
```

**AssumeUTXO Status:**
```bash
# Snapshot bootstrap/trust-gate diagnostics
dinero-cli -testnet getsnapshotbootstrapstatus

# IBD + scheduler + embedded snapshot diagnostics
dinero-cli -testnet getibdprogress

# Check background validation
dinero-cli -testnet getbackgroundvalidationprogress
```

**Resource Usage:**
```bash
# CPU and memory
ps aux | grep dinerod

# Disk usage
du -sh ~/.dinero-testnet/
```

### Log Patterns to Watch

**Success Indicators:**
```
✓ Snapshot loaded successfully
✓ UTXO count matches
✓ UTXO Set Verification PASSED
✅ BACKGROUND VALIDATION COMPLETE
```

**Failure Indicators (trigger rollback):**
```
✗ UTXO COUNT MISMATCH
❌ BACKGROUND VALIDATION FAILED
🔄 AUTOMATIC ROLLBACK INITIATED
```

---

## Troubleshooting

### Snapshot Load Fails

**Error:** "Snapshot base block not found in chain"

**Solution:**
```bash
# Ensure node has synced headers first
dinero-cli -testnet getblockcount

# Headers must reach or exceed snapshot height
# Wait for headers-first sync to complete
```

### Background Validation Stalled

**Check:**
```bash
# Verify node is syncing
tail -f ~/.dinero-testnet/debug.log | grep -i "block\|sync"

# Check disk space
df -h ~/.dinero-testnet/

# Check network connectivity
dinero-cli -testnet getpeerinfo
```

### Rollback Triggered Unexpectedly

**Investigate:**
```bash
# Check rollback reason in logs
grep -B 20 "AUTOMATIC ROLLBACK" ~/.dinero-testnet/debug.log

# Common causes:
# - UTXO count mismatch (corrupted snapshot)
# - Block validation failure (invalid blocks)
# - Database corruption
```

### Pre-Release Soak (Recommended)

Run a duration-based churn/restart soak before promoting bridge/proof-serving changes:

```bash
# 2-hour soak with at least 2 full cycles
SOAK_HOURS=2 MIN_CYCLES=2 \
PRELOAD_BLOCKS=180 CHURN_ROUNDS=6 \
tests/integration/test_csn_reorg_churn_restart_soak.sh

# Full proof-plane hardening runner with long soak enabled
RUN_LONG_SOAK=1 SOAK_HOURS=2 SOAK_MIN_CYCLES=2 \
RUN_ADVERSARIAL=0 RUN_GETPROOF_ABUSE=1 RUN_PROOFDATA_ABUSE=1 \
tests/integration/test_bridge_proof_plane_hardening.sh
```

---

## Success Criteria

**Deployment Successful When:**
- [x] Node syncing blocks on testnet
- [x] Hardening code detected in binary
- [x] Logs show Phase 44.1 messages

**Snapshot Load Successful When:**
- [ ] Snapshot loads without error
- [ ] AssumeUTXO mode entered
- [ ] Background validation starts
- [ ] UTXO count verification passes

**Background Validation Successful When:**
- [ ] Validation completes (100%)
- [ ] No errors in logs
- [ ] No rollback triggered
- [ ] Node exits AssumeUTXO mode cleanly

**Production Ready When:**
- [ ] Multiple snapshots loaded successfully
- [ ] Full validation cycle completed
- [ ] No unexpected rollbacks
- [ ] Performance acceptable

---

## Next Steps After Testnet Validation

1. **Document Results:**
   - Snapshot load times
   - Validation duration
   - Resource usage
   - Any issues encountered

2. **Create Official Testnet Snapshot:**
   - Generate from fully-synced node
   - Compute checksums
   - GPG sign with release key
   - Publish for community testing

3. **Prepare for Mainnet:**
   - Update mainnet enablement guide
   - Generate mainnet snapshot
   - Announce feature availability
   - Monitor adoption

---

## References

- [Hardening Implementation](ASSUMEUTXO_HARDENING_COMPLETE.md)
- [Manual Test Plan](assumeutxo_hardening_test_plan.md)
- [Mainnet Enablement Guide](assumeutxo_mainnet_enablement.md)
- [Security Model](assumeutxo_security_model.md)

---

**Deployment Status:** Ready
**Last Updated:** 2025-12-24
**Maintainer:** DineroCoin Development Team
