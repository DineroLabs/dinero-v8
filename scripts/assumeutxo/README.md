# AssumeUTXO Operational Tooling

**Purpose:** Generate and distribute UTXO snapshots for AssumeUTXO bootstrap.

**Location:** `scripts/assumeutxo/` (non-consensus operational tools)

---

## ⚠️ Important Principles

1. **Snapshots are UNTRUSTED** until operators verify them
2. **This is operational tooling**, NOT consensus code
3. **No auto-publishing** — all distribution is manual
4. **Operators may generate their own** snapshots
5. **Trust must be explicit** — verify checksums + signatures

---

## Scripts

### `generate_snapshot.sh`

Generates UTXO snapshots with checksums and GPG signatures.

**Usage:**
```bash
# Testnet (default)
./generate_snapshot.sh testnet

# Mainnet
./generate_snapshot.sh mainnet

# Regtest
./generate_snapshot.sh regtest
```

**What it does:**
1. ✓ Checks daemon is synced
2. ✓ Generates snapshot via `dumptxoutset`
3. ✓ Computes SHA256 checksum
4. ✓ Writes a loader-compatible `*.manifest.json`
5. ✓ Creates GPG signature (if available)
6. ✓ Stages output in `/var/lib/dinero/snapshots/`
7. ✗ Does NOT auto-publish

**Output files:**
```
/var/lib/dinero/snapshots/testnet/
  utxo-snapshot-testnet-height-123456-20251224-120000.dat
  utxo-snapshot-testnet-height-123456-20251224-120000.dat.sha256
  utxo-snapshot-testnet-height-123456-20251224-120000.dat.manifest.json
  utxo-snapshot-testnet-height-123456-20251224-120000.dat.asc
  utxo-snapshot-testnet-height-123456-20251224-120000.dat.json
```

### `stage_ios_snapshot.sh`

Stages a generated snapshot into `DineroDPI/Bootstrap/`
with stable bundle filenames so the iOS app can discover it automatically.

**Usage:**
```bash
./stage_ios_snapshot.sh /path/to/utxo-snapshot-mainnet-height-765432.dat
```

**Environment variables:**
- `DINERO_DATADIR` — Override datadir (default: `~/.dinero`)
- `SNAPSHOT_OUTDIR` — Override output dir (default: `/var/lib/dinero/snapshots/$NETWORK`)
- `DINERO_CLI` — Override CLI path (default: `dinero-cli`)

---

## Automated Generation (Cron)

**Safe cron setup:**

```bash
# Generate weekly testnet snapshot (Sundays at 2 AM)
0 2 * * 0 /path/to/scripts/assumeutxo/generate_snapshot.sh testnet >> /var/log/dinero/snapshot.log 2>&1

# Generate monthly mainnet snapshot (1st of month at 3 AM)
0 3 1 * * /path/to/scripts/assumeutxo/generate_snapshot.sh mainnet >> /var/log/dinero/snapshot.log 2>&1
```

**What cron does:**
- ✓ Generate snapshot
- ✓ Stage in output directory
- ✓ Log metadata
- ✗ Does NOT publish

**Publishing must be manual** or CI-mediated (with approval).

---

## Snapshot Distribution

### Recommended Hosting

**Option 1: Static HTTP (NGINX/S3/GitHub Releases/CDN)**

Directory structure:
```
snapshots/
  testnet/
    utxo-snapshot-height-123456.dat
    utxo-snapshot-height-123456.dat.sha256
    utxo-snapshot-height-123456.dat.asc
  mainnet/
    utxo-snapshot-height-765432.dat
    utxo-snapshot-height-765432.dat.sha256
    utxo-snapshot-height-765432.dat.asc
```

**Option 2: Torrent**

For large snapshots (>1GB):
- Create `.torrent` file
- Seed from multiple locations
- Reduces bandwidth costs

**Option 3: IPFS**

For decentralized distribution:
- Pin snapshot to IPFS
- Distribute CID + checksum
- Operators verify before use

---

## Operator Verification

**Operators MUST verify snapshots before use:**

### 1. Download snapshot + checksum
```bash
wget https://snapshots.dinero-coin.com/mainnet/utxo-snapshot-height-765432.dat
wget https://snapshots.dinero-coin.com/mainnet/utxo-snapshot-height-765432.dat.sha256
```

### 2. Verify checksum
```bash
sha256sum -c utxo-snapshot-height-765432.dat.sha256
# Expected: utxo-snapshot-height-765432.dat: OK
```

### 3. Verify GPG signature (if available)
```bash
# Import signing key first
gpg --recv-keys DINERO_RELEASE_KEY

# Verify signature
gpg --verify utxo-snapshot-height-765432.dat.asc utxo-snapshot-height-765432.dat
# Expected: Good signature from "DineroCoin Release Team"
```

### 4. Load snapshot
```bash
dinero-cli loadtxoutset /path/to/utxo-snapshot-height-765432.dat
```

### 5. Monitor background validation
```bash
dinero-cli getbackgroundvalidationprogress
```

---

## Security Model

### Trust Requirements

**What you MUST trust:**
- Snapshot authenticity (via checksum + GPG)
- Snapshot source (official releases or self-generated)

**What you DON'T need to trust:**
- Snapshot content (background validation detects bad snapshots)
- Network peers (validation is local)
- Implementation bugs (crash safety proven)

### Trust Timeline

1. **Initial load** — Trust snapshot temporarily
2. **Background validation** — Verify all blocks from genesis
3. **Validation complete** — Trust removed, full security restored

---

## Publishing Workflow (Manual)

### Testnet (Weekly)

1. Cron generates snapshot on Sunday 2 AM
2. Monitor logs: `tail -f /var/log/dinero/snapshot.log`
3. Verify output in `/var/lib/dinero/snapshots/testnet/`
4. Test locally: `dinero-cli --testnet loadtxoutset <snapshot>`
5. Manual upload to distribution server
6. Update release notes with checksum

### Mainnet (Monthly)

1. Cron generates snapshot on 1st of month 3 AM
2. **Extended verification:**
   - Load on isolated node
   - Wait for background validation to complete
   - Verify no errors
3. GPG sign with release key
4. Manual upload to distribution server
5. Announce with checksums + signatures
6. Update documentation

---

## What NOT To Do

❌ **Don't auto-enable AssumeUTXO** because snapshots exist
❌ **Don't add snapshot URLs** to consensus code
❌ **Don't ship unsigned snapshots** in releases
❌ **Don't mix ops scripts** with consensus directories
❌ **Don't auto-publish** from cron without approval
❌ **Don't assume operators** will verify checksums (enforce it in docs)

---

## Troubleshooting

### Snapshot generation fails

**Error:** `dumptxoutset failed`

**Solutions:**
- Check daemon is fully synced
- Ensure sufficient disk space
- Check datadir permissions
- Review daemon logs

### Checksum mismatch

**Error:** `sha256sum: WARNING: 1 computed checksum did NOT match`

**Solutions:**
- Re-download snapshot (corruption during transfer)
- Verify download URL is official
- Check network connection
- Do NOT use corrupted snapshot

### GPG signature invalid

**Error:** `BAD signature`

**Solutions:**
- Verify signing key is correct
- Re-download snapshot + signature
- Check for man-in-the-middle attacks
- Do NOT use snapshot with bad signature

---

## References

- [AssumeUTXO Security Model](../../docs/assumeutxo_security_model.md)
- [Mainnet Enablement Guide](../../docs/assumeutxo_mainnet_enablement.md)
- [Production-Ready Declaration](../../docs/ASSUMEUTXO_PRODUCTION_READY.md)

---

**Last Updated:** 2025-12-24
**Maintainer:** DineroCoin Development Team
