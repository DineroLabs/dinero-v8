# dinerod doctor - Operator Runbook

Production diagnostics for Dinero node operators, exchanges, and mining pools.

## Quick Reference

```bash
# Quick health check (default, <30s)
dinerod doctor

# Deep check with full SST verification (<10 min)
dinerod doctor --deep

# Machine-readable output for monitoring
dinerod doctor --json

# Save report to file
dinerod doctor --json --output /tmp/health.json

# Check specific domains
dinerod doctor --checks "storage.*"
dinerod doctor --checks "db.*,inv.*"

# Auto-fix safe issues (missing dirs, corrupt mempool.dat)
dinerod doctor --apply-safe-fixes

# List all checks
dinerod doctor --list-checks

# Explain a specific check
dinerod doctor --explain db.tip_consistency
```

## Exit Codes (Stable Contract)

| Code | Meaning | Action |
|------|---------|--------|
| 0 | Healthy | No action needed |
| 1 | Warnings only | Review findings, non-urgent |
| 2 | Critical findings | Operator action required |
| 3 | Internal error | Doctor itself failed, report bug |

## v1 Check Set

| Check ID | Mode | What It Does |
|----------|------|-------------|
| `storage.disk_space` | BOTH | Warn <1GB, crit <256MB available |
| `storage.permissions` | BOTH | Verify R/W access to data directories |
| `storage.fsync_latency.sample` | BOTH | Measure fsync latency (1 or 5 samples) |
| `db.sqlite.quick_check` | BOTH | Verify wallet SQLite headers (deep: page validation) |
| `db.tip_consistency` | BOTH | Verify RocksDB CURRENT/MANIFEST integrity |
| `db.rocksdb.checksum_sample` | DEEP | Verify SST files (sample in BOTH, full scan in DEEP) |
| `mempool.snapshot_sanity` | QUICK | Verify mempool.dat magic header and version |
| `p2p.bind_listen` | QUICK | Check if P2P port is available |
| `p2p.dns_seeds.resolve` | QUICK | Resolve DNS seed hostnames |
| `inv.supply_bounds` | QUICK | Verify consensus supply schedule invariants |

## Safe Auto-Fixes (v1)

Only two fixes are auto-applied with `--apply-safe-fixes`:

| Fix ID | Trigger | Action | Risk |
|--------|---------|--------|------|
| `storage.permissions.create_dirs` | Missing blockchain/chaindb/wallets subdirs | `mkdir` with 0700 | LOW |
| `mempool.snapshot_sanity.remove` | Corrupt mempool.dat header | Remove file (rebuilt from peers) | LOW |

Destructive operations (reindex, DB repair) are **never** auto-applied. Doctor suggests them in the fix plan but requires manual execution.

## Common Scenarios

### Fresh Node Setup
```bash
$ dinerod doctor
  [WARN]  storage.permissions
         Optional subdirectories missing (safe to create)

$ dinerod doctor --apply-safe-fixes
  [ OK ] storage.permissions.create_dirs
         Created 2 missing directory(ies)
```

### Corrupt Mempool
```bash
$ dinerod doctor
  [CRIT]  mempool.snapshot_sanity
         mempool.dat has invalid magic header

$ dinerod doctor --apply-safe-fixes
  [ OK ] mempool.snapshot_sanity.remove
         Removed /home/node/.dinero/mempool.dat
# Restart daemon - mempool rebuilds from peers
```

### Corrupted ChainDB
```bash
$ dinerod doctor
  [CRIT]  db.tip_consistency
         RocksDB CURRENT file missing
         Fix: db.tip_consistency.reindex (risk: HIGH)
           -> dinerod --reindex

# Manual action required:
$ dinerod --reindex  # Takes 30+ minutes
```

### Low Disk Space
```bash
$ dinerod doctor
  [CRIT]  storage.disk_space
         Critically low disk space: 128 MB remaining
# Expand volume or clean up before node writes fail
```

### Pre-Upgrade Verification
```bash
# Run before upgrading daemon binary
dinerod doctor --deep --json --output /tmp/pre-upgrade.json

# Upgrade daemon...

# Run after upgrade
dinerod doctor --deep --json --output /tmp/post-upgrade.json

# Compare reports
diff <(jq '.checks[].status' /tmp/pre-upgrade.json) \
     <(jq '.checks[].status' /tmp/post-upgrade.json)
```

## CI / Monitoring Integration

### Cron Health Check
```bash
# /etc/cron.d/dinero-health
*/15 * * * * dinero dinerod doctor --json --output /var/log/dinero/health.json 2>/dev/null
```

### Nagios/Icinga Plugin
```bash
#!/bin/bash
EXIT=$(dinerod doctor --json 2>/dev/null | jq -r '.exit_code')
case "$EXIT" in
  0) echo "DINERO OK"; exit 0;;
  1) echo "DINERO WARNING"; exit 1;;
  2) echo "DINERO CRITICAL"; exit 2;;
  *) echo "DINERO UNKNOWN"; exit 3;;
esac
```

### Prometheus Textfile Collector
```bash
#!/bin/bash
OUT=/var/lib/prometheus/node-exporter/dinero.prom
JSON=$(dinerod doctor --json 2>/dev/null)
echo "# HELP dinero_doctor_exit Doctor exit code"
echo "# TYPE dinero_doctor_exit gauge"
echo "dinero_doctor_exit $(echo $JSON | jq '.exit_code')"
echo "# HELP dinero_doctor_critical Critical findings count"
echo "# TYPE dinero_doctor_critical gauge"
echo "dinero_doctor_critical $(echo $JSON | jq '.summary.critical')"
echo "# HELP dinero_doctor_warnings Warning findings count"
echo "# TYPE dinero_doctor_warnings gauge"
echo "dinero_doctor_warnings $(echo $JSON | jq '.summary.warnings')"
```

## JSON Schema (v1.0)

Top-level fields (stable contract, additive-only evolution):

```json
{
  "schema_version": "1.0",
  "node_version": "0.1.0",
  "network": "mainnet",
  "timestamp": "2026-02-13T18:03:49.072Z",
  "mode": "quick",
  "exit_code": 0,
  "summary": {
    "critical": 0, "warnings": 0, "errors": 0, "skipped": 0, "passed": 10
  },
  "checks": [
    {
      "id": "storage.disk_space",
      "status": "PASS",
      "message": "450 GB available",
      "evidence": {"available": "450 GB", "total": "500 GB"},
      "fix_plan": [],
      "duration_ms": 0,
      "started_at": "2026-02-13T18:03:49.072Z",
      "finished_at": "2026-02-13T18:03:49.072Z"
    }
  ]
}
```

## Network Selection

```bash
dinerod doctor                    # mainnet (default)
dinerod doctor --testnet          # testnet data directory
dinerod doctor --regtest          # regtest data directory
dinerod doctor --datadir=/custom  # explicit data directory
```

## Deep Mode Budget

Deep mode has a 10-minute overall budget. If early checks consume too much time, later checks may be skipped with `"Skipped: insufficient run budget remaining"`. Each check also has its own per-check timeout (1-60 seconds depending on the check).
