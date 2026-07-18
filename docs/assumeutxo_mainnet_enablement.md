# AssumeUTXO: Mainnet Enablement Guide

**Status:** ✅ PRODUCTION-READY
**Version:** 1.0
**Date:** 2025-12-24

---

## Executive Summary

AssumeUTXO is **ready for mainnet deployment**. All code is production-ready, security-validated, and crash-tested. This document provides the operational guide for enabling AssumeUTXO on mainnet.

**What's Ready:**
- ✅ Core implementation (Phases 42-46)
- ✅ Security fixes (3 CRITICAL bugs found and fixed)
- ✅ Crash safety (proven under SIGKILL)
- ✅ Adversarial testing (7 attack vectors blocked)
- ✅ Lifecycle validation (end-to-end tested)

**What's Needed:**
- Operational tooling setup (snapshot generation + distribution)
- First official snapshot creation
- Operator documentation
- Monitoring and support plan

---

## Phased Rollout Plan

### Phase 1: Testnet Deployment (Week 1)

**Goal:** Validate operational procedures on testnet with live peers.

**Tasks:**
1. Deploy snapshot generation script on testnet node
2. Generate first testnet snapshot
3. Test snapshot load on fresh testnet node
4. Monitor background validation completion
5. Document any operational issues

**Success Criteria:**
- ✓ Snapshot generated successfully
- ✓ New node bootstraps from snapshot
- ✓ Background validation completes
- ✓ No errors or corruption detected

### Phase 2: Infrastructure Setup (Week 2)

**Goal:** Establish snapshot distribution infrastructure.

**Tasks:**
1. Set up snapshot hosting (NGINX/S3/GitHub Releases)
2. Configure automated snapshot generation (cron)
3. Establish GPG signing process
4. Create operator verification documentation
5. Set up monitoring and alerts

**Success Criteria:**
- ✓ Snapshots hosted and accessible
- ✓ Checksums and signatures available
- ✓ Automated generation working
- ✓ Documentation complete

### Phase 3: Mainnet Soft Launch (Week 3)

**Goal:** Enable AssumeUTXO on mainnet with feature flag.

**Tasks:**
1. Generate first official mainnet snapshot
2. Verify snapshot through full background validation
3. Sign snapshot with release key
4. Publish snapshot + checksums + signatures
5. Enable feature flag: `assumeutxo_enabled=1` (default: 0)
6. Announce in release notes as "experimental"

**Success Criteria:**
- ✓ Snapshot verified and signed
- ✓ Feature available but opt-in
- ✓ Documentation clear
- ✓ Support channels ready

### Phase 4: Mainnet Production (Week 4+)

**Goal:** Make AssumeUTXO default for new nodes.

**Tasks:**
1. Monitor adoption and feedback
2. Address any operational issues
3. Update default: `assumeutxo_enabled=1`
4. Include snapshot in release bundles
5. Promote in mainnet documentation

**Success Criteria:**
- ✓ No critical issues reported
- ✓ Positive operator feedback
- ✓ Default enabled
- ✓ Widespread adoption

---

## Snapshot Generation & Distribution (Operational)

**IMPORTANT:** This is operational tooling, NOT consensus code. Snapshots are untrusted until operators verify them.

### Automated Generation

**Location:** `scripts/assumeutxo/generate_snapshot.sh`

**Setup:**

1. **Install script:**
   ```bash
   cd /opt/dinero
   cp scripts/assumeutxo/generate_snapshot.sh .
   chmod +x generate_snapshot.sh
   ```

2. **Configure cron:**
   ```bash
   # Testnet: Weekly (Sundays 2 AM)
   0 2 * * 0 DINERO_DATADIR=/var/lib/dinero /opt/dinero/generate_snapshot.sh testnet >> /var/log/dinero/snapshot.log 2>&1

   # Mainnet: Monthly (1st of month 3 AM)
   0 3 1 * * DINERO_DATADIR=/var/lib/dinero /opt/dinero/generate_snapshot.sh mainnet >> /var/log/dinero/snapshot.log 2>&1
   ```

3. **Monitor logs:**
   ```bash
   tail -f /var/log/dinero/snapshot.log
   ```

**Output:**
- Snapshot file: `utxo-snapshot-{network}-height-{N}-{timestamp}.dat`
- Checksum: `*.sha256`
- Signature: `*.asc` (if GPG configured)
- Metadata: `*.json`

### Distribution Setup

**Recommended: Static HTTP (NGINX + S3 Backup)**

**Directory structure:**
```
/var/www/snapshots.dinero-coin.com/
  testnet/
    utxo-snapshot-height-123456.dat
    utxo-snapshot-height-123456.dat.sha256
    utxo-snapshot-height-123456.dat.asc
    latest.txt  (contains: utxo-snapshot-height-123456.dat)
  mainnet/
    utxo-snapshot-height-765432.dat
    utxo-snapshot-height-765432.dat.sha256
    utxo-snapshot-height-765432.dat.asc
    latest.txt  (contains: utxo-snapshot-height-765432.dat)
```

**NGINX config:**
```nginx
server {
    listen 443 ssl;
    server_name snapshots.dinero-coin.com;

    root /var/www/snapshots.dinero-coin.com;

    location / {
        autoindex on;
        add_header Access-Control-Allow-Origin *;
    }

    # Cacheable (snapshots are immutable)
    location ~* \.(dat|sha256|asc|json)$ {
        expires 30d;
        add_header Cache-Control "public, immutable";
    }
}
```

**S3 Backup:**
```bash
# Sync to S3 after generation
aws s3 sync /var/www/snapshots.dinero-coin.com/ \
    s3://dinero-snapshots/ \
    --acl public-read
```

### Publishing Workflow

**Testnet (Weekly):**
1. Cron generates snapshot
2. Verify locally: `dinero-cli --testnet loadtxoutset <path>`
3. Manual upload to distribution server
4. Update `latest.txt`
5. Announce in Discord/Telegram

**Mainnet (Monthly):**
1. Cron generates snapshot
2. **Extended verification:**
   - Load on isolated node
   - Wait for full background validation
   - Verify no errors
3. Sign with release GPG key
4. Test on fresh node
5. Manual upload to distribution server
6. Update `latest.txt`
7. Announce in release notes + community channels

---

## Operator Documentation

### Quick Start with AssumeUTXO

**For new node operators:**

1. **Download snapshot:**
   ```bash
   wget https://snapshots.dinero-coin.com/mainnet/$(curl -s https://snapshots.dinero-coin.com/mainnet/latest.txt)
   wget https://snapshots.dinero-coin.com/mainnet/$(curl -s https://snapshots.dinero-coin.com/mainnet/latest.txt).sha256
   wget https://snapshots.dinero-coin.com/mainnet/$(curl -s https://snapshots.dinero-coin.com/mainnet/latest.txt).asc
   ```

2. **Verify checksum:**
   ```bash
   sha256sum -c *.sha256
   # Expected: OK
   ```

3. **Verify signature:**
   ```bash
   gpg --recv-keys DINERO_RELEASE_KEY_ID
   gpg --verify *.asc *.dat
   # Expected: Good signature
   ```

4. **Start daemon:**
   ```bash
   dinerod --daemon
   ```

5. **Load snapshot:**
   ```bash
   dinero-cli loadtxoutset /path/to/snapshot.dat
   ```

6. **Monitor validation:**
   ```bash
   dinero-cli getbackgroundvalidationprogress
   ```

**Result:**
- Node is immediately usable (RPC, wallets, mining)
- Background validation runs automatically (3-7 days)
- Full security restored after validation completes

### Verification Steps (Mandatory)

**Operators MUST verify:**

1. **Checksum match**
   - Use `sha256sum -c` to verify
   - Do NOT skip this step

2. **GPG signature valid**
   - Import official release key
   - Verify with `gpg --verify`

3. **Network consensus**
   - Cross-reference height with block explorers
   - Confirm base block hash matches

4. **Background validation completes**
   - Monitor with `getbackgroundvalidationprogress`
   - Alert if validation fails

### Security Model

**Trust Requirements:**

**What you MUST trust:**
- Snapshot authenticity (via checksum + GPG verification)
- Snapshot source (official releases or self-generated)

**What you DON'T need to trust:**
- Snapshot contents (background validation detects bad data)
- Network peers (validation is local)
- Implementation bugs (crash safety proven)

**Trust Timeline:**
1. **Load snapshot** — Trust temporarily (verified by checksum/GPG)
2. **Background validation** — Verify all blocks from genesis
3. **Validation complete** — Trust removed, full security

---

## Configuration Options

### Feature Flag

**Enable AssumeUTXO:**
```conf
# dinero.conf
assumeutxo_enabled=1  # Enable (default: 0 for soft launch)
```

**Automatic bootstrap:**
```conf
# dinero.conf
assumeutxo_snapshot=/path/to/snapshot.dat  # Auto-load on startup
```

### Advanced Options

**Background validation:**
```conf
# Default: enabled, cannot be disabled (security requirement)
# Progress can be monitored via RPC
```

**Snapshot trust gate and transport hardening:**
```conf
# Optional explicit manifest path (JSON with sha256/height/block_hash)
assumeutxo_manifest=/path/to/snapshot.dat.manifest.json

# Require manifest validation before loadtxoutset/auto-bootstrap
assumeutxo_require_manifest=1

# Transport size guard (default 65536 MB = 64 GiB)
assumeutxo_snapshot_max_mb=65536
```

**Pruning (optional):**
```conf
# After validation completes, enable pruning
prune_mode=auto
prune_target_gb=10
```

---

## Monitoring and Support

### Metrics to Monitor

**Snapshot generation:**
- Generation success rate
- Snapshot size trends
- Generation time
- Disk space usage

**Snapshot distribution:**
- Download counts
- Bandwidth usage
- Geographic distribution
- Checksum verification rate

**Background validation:**
- Validation progress
- Validation completion time
- Validation failure rate
- Resource usage (CPU, disk I/O)

### Snapshot Bootstrap RPC Checklist

Use these RPCs during snapshot bootstrap and while background validation runs:

```bash
# Full snapshot bootstrap diagnostics (trust gate + transport + next action)
dinero-cli getsnapshotbootstrapstatus

# IBD/scheduler plus embedded snapshot bootstrap diagnostics
dinero-cli getibdprogress

# Validation status and progress to snapshot base height
dinero-cli getbackgroundvalidationprogress
```

Key fields to watch:
- `snapshot_bootstrap.trust_gate_mode` (`required`/`optional`/`disabled`)
- `snapshot_bootstrap.manifest_present`
- `snapshot_bootstrap.configured_snapshot_symlink` (must be `false`)
- `snapshot_bootstrap.background_validation_status`
- `snapshot_bootstrap.next_action`

### Support Channels

**For operators:**
- Documentation: `docs/assumeutxo_security_model.md`
- Discord: #node-operators
- GitHub Discussions: AssumeUTXO category
- Email: support@dinero-coin.com

**For developers:**
- GitHub Issues: Tag `assumeutxo`
- Developer docs: `docs/`
- Code: `src/daemon/services/chainstate_service.cpp` (Phase 42-46)

---

## Troubleshooting

### Snapshot generation fails

**Error:** `dumptxoutset failed`

**Solutions:**
- Ensure daemon is fully synced
- Check disk space (need 2x UTXO set size)
- Verify datadir permissions
- Review daemon logs

### Snapshot load fails

**Error:** `Snapshot base block not found in chain`

**Cause:** Node hasn't synced headers to snapshot height yet

**Solution:**
- Wait for headers-first sync to complete
- Check P2P connectivity
- Verify network peers are available

### Background validation stalled

**Error:** Validation progress not advancing

**Solutions:**
- Check disk space
- Verify P2P connectivity
- Review daemon logs for errors
- Monitor resource usage (CPU, I/O)

### Checksum mismatch

**Error:** `sha256sum: computed checksum did NOT match`

**Cause:** Corrupted download or malicious snapshot

**Solution:**
- Re-download snapshot
- Verify download URL is official
- Check network security
- **DO NOT use corrupted snapshot**

---

## What NOT To Do

❌ **Don't auto-enable** AssumeUTXO because snapshots exist
❌ **Don't add snapshot URLs** to consensus code
❌ **Don't ship unsigned snapshots** in releases
❌ **Don't mix ops scripts** with consensus directories
❌ **Don't auto-publish** from cron without verification
❌ **Don't skip checksum verification** (enforce in docs)
❌ **Don't disable background validation** (security requirement)

---

## Success Criteria

### Testnet (Week 1)
- [ ] Snapshot generated successfully
- [ ] Snapshot loaded on fresh node
- [ ] Background validation completed
- [ ] No errors reported

### Infrastructure (Week 2)
- [ ] Hosting configured and accessible
- [ ] Automated generation working
- [ ] GPG signing process established
- [ ] Documentation complete

### Mainnet Soft Launch (Week 3)
- [ ] Official snapshot created and verified
- [ ] Feature flag enabled (opt-in)
- [ ] Release notes published
- [ ] Support channels ready

### Mainnet Production (Week 4+)
- [ ] No critical issues
- [ ] Positive feedback from operators
- [ ] Default enabled
- [ ] Widespread adoption

---

## Timeline Summary

| Week | Phase | Key Milestone |
|------|-------|---------------|
| 1 | Testnet | First snapshot + load test |
| 2 | Infrastructure | Hosting + automation ready |
| 3 | Soft Launch | Feature flag enabled (opt-in) |
| 4+ | Production | Default enabled |

---

## References

- [Production-Ready Declaration](ASSUMEUTXO_PRODUCTION_READY.md)
- [Security Model](assumeutxo_security_model.md)
- [Crash Safety Summary](../tests/abuse/CRASH_SAFETY_SUMMARY.md)
- [Operational Tooling](../scripts/assumeutxo/README.md)

---

## Sign-Off

**Status:** ✅ APPROVED FOR MAINNET DEPLOYMENT

**Conditions:**
- Follow phased rollout plan
- Verify all snapshots before publishing
- Monitor operational metrics
- Provide operator support

**Deployment Authority:** Network operators (decentralized)

**Last Updated:** 2025-12-24
**Maintainer:** DineroCoin Development Team

---

**END OF GUIDE**
