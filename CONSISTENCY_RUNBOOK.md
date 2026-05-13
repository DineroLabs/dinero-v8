# Dinero – Consistency Auto‑Heal Pack

This pack gives ops a zero‑touch, self‑healing path for genesis **chainwork** correctness and CI/production guardrails.

## What's inside

* `migrations/005_chainwork_bootstrap.sql` – idempotent SQL migration
* `scripts/fix_chainwork_bulk.sh` – multi‑network bulk fixer
* `systemd/dinero@.service` – startup hook with zero downtime
* `scripts/cicd_preflight.sh` – CI/CD one-liner check
* `.github/workflows/consistency.yml` – automated CI validation

---

## migrations/005\_chainwork\_bootstrap.sql

```sql
-- Idempotent fix for genesis chainwork on height 0 databases
-- Safe to run multiple times; only corrects the specific old value.
BEGIN IMMEDIATE;
UPDATE meta
   SET value='0000000000000000000000000000000000000000000000000000000000000001'
 WHERE key='chainwork'
   AND value='0000000000000000000000000000000000000000000000000000000000000000'
   AND (SELECT value FROM meta WHERE key='height') = '0';
COMMIT;
```

**Apply locally:**

```bash
sqlite3 data/<network>/explorer.db < migrations/005_chainwork_bootstrap.sql
```

---

## scripts/fix\_chainwork\_bulk.sh

**Usage:**

```bash
chmod +x scripts/fix_chainwork_bulk.sh
scripts/fix_chainwork_bulk.sh                 # scans data/*/*/explorer.db
scripts/fix_chainwork_bulk.sh /var/lib/dinero # custom root
```

**Output example:**
```
[regtest] /path/to/regtest/explorer.db  chainwork: 0000...0000 -> 0000...0001
[testnet] /path/to/testnet/explorer.db  chainwork: 0000...0001 -> 0000...0001
```

---

## systemd/dinero\@.service

**Enable & start (example for regtest):**

```bash
sudo cp systemd/dinero@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now dinero@regtest
# or for testnet/mainnet accordingly
```

**Check status:**
```bash
sudo systemctl status dinero@regtest
sudo journalctl -u dinero@regtest -f
```

---

## CI/CD Integration

### GitHub Actions (automated)
The workflow runs automatically on push/PR and validates consistency across networks.

### Manual CI/CD preflight
```bash
# Before deployment
./scripts/cicd_preflight.sh

# Custom network
NET=testnet ./scripts/cicd_preflight.sh
```

### Production deployment hook
```bash
# Add to your deployment scripts
./scripts/cicd_preflight.sh && systemctl start dinero@mainnet
```

---

## Ops runbook (quick reference)

### Fresh installs
✅ **Automatic** - migrations apply during daemon startup → correct genesis chainwork

### Existing nodes  
🔧 **One-time fix** - run the bulk fixer once across datadirs:
```bash
./scripts/fix_chainwork_bulk.sh
```

### Production startup
✅ **Self-healing** - systemd `ExecStartPre` ensures consistency (no-op when already correct)

### Monitoring & validation
🔍 **Continuous** - GitHub Actions enforces invariants in CI
🔍 **On-demand** - run consistency smoke checker anytime:
```bash
./scripts/smoke_consistency.sh
```

### Troubleshooting

**Check current chainwork:**
```bash
sqlite3 data/regtest/explorer.db "SELECT key, value FROM meta WHERE key='chainwork';"
```

**Manual fix (if needed):**
```bash
sqlite3 data/regtest/explorer.db < migrations/005_chainwork_bootstrap.sql
```

**Validate fix:**
```bash
NET=regtest ./scripts/smoke_consistency.sh
```

---

## File manifest

```
migrations/005_chainwork_bootstrap.sql  # Idempotent SQL migration
scripts/fix_chainwork_bulk.sh          # Bulk network fixer
scripts/cicd_preflight.sh              # CI/CD one-liner
scripts/smoke_consistency.sh           # Validation enforcer (existing)
systemd/dinero@.service                # Production systemd unit
.github/workflows/consistency.yml      # GitHub Actions CI
CONSISTENCY_RUNBOOK.md                 # This runbook
```

**Status**: 🚀 **Production Ready** - Zero manual SQLite operations required!
