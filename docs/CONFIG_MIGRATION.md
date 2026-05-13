# Configuration Migration Guide

**Status:** Phase Z.2 - Configuration Guarantees
**Date:** 2025-12-31
**Objective:** Version-specific configuration migration paths

---

## Philosophy

**"Upgrade should never break your node."**

Every configuration version change includes:
- Automatic migration (where possible)
- Manual migration steps (when required)
- Rollback procedure
- Breaking change announcement

---

## Version History

### config_version=1 (v1.0.0 - Current)

**Released:** v1.0.0
**Status:** STABLE (frozen for v1.0.x series)

**Initial configuration format.**

**Key Structure:**
- Dotted keys: `wallet.datadir`, `rpc.port`, `p2p.port`
- Flat key aliases: `datadir`, `rpcport`, `port` (backward compatible)

**No Migration Required:** This is the initial version.

---

## Future Migrations (Preview)

### config_version=2 (v1.1.0 - Future)

**Status:** NOT RELEASED

**Planned Changes:**
- None currently planned
- This section will be populated if v1.1 introduces breaking config changes

**Migration Path:**
- Automatic: Daemon will migrate config_version=1 → 2 on first start
- Manual: None required (backward compatible)

---

## Migration Procedures

### Automatic Migration

**When:** Daemon detects old config_version

**Process:**
1. Daemon loads old config
2. Daemon applies transformations
3. Daemon writes new config_version
4. Daemon logs migration success

**Example:**
```
[ConfigService] Detected config_version=1
[ConfigService] Migrating to config_version=2
[ConfigService] Migration successful
[ConfigService] Restart not required
```

**Rollback:**
- Automatic migration does NOT modify original config
- Safe to downgrade (daemon will ignore newer fields)

---

### Manual Migration

**When:** Breaking changes require user intervention

**Example Scenario:** (Hypothetical - not in v1.0 → v1.1)

**Old Config (v1.0):**
```conf
rpcallowip=*  # Wildcard
```

**New Config (v1.1):**
```conf
rpcallowip=0.0.0.0/0  # CIDR notation required
```

**Migration Steps:**
1. Stop daemon
2. Edit `dinero.conf`
3. Replace wildcards with CIDR
4. Update `config_version=2`
5. Start daemon

**Validation:**
- Daemon rejects invalid config with clear error
- Error message includes migration instructions

---

## Backward Compatibility

### Flat Key Aliases (Permanent)

**Policy:** Flat keys are NEVER removed.

**Example:**
```conf
# v1.0 (both valid)
datadir=/var/lib/dinerod
wallet.datadir=/var/lib/dinerod

# v1.1 (both still valid)
datadir=/var/lib/dinerod
wallet.datadir=/var/lib/dinerod
```

**Guarantee:**
- `datadir` alias maintained forever
- `wallet.datadir` is canonical
- Both map to same internal key

---

### Deprecated Options

**Policy:** Deprecation cycle = 1 major version

**Example Deprecation Timeline:**

**v1.0:**
- Option exists: `old_option=value`
- Status: STABLE

**v1.1:**
- Option deprecated: `old_option=value` (with warning)
- New option: `new_option=value`
- Status: DEPRECATED

**v2.0:**
- Option removed: `old_option` no longer recognized
- Only new option: `new_option=value`
- Status: REMOVED

**Warning Message:**
```
[ConfigService] WARNING: 'old_option' is deprecated
[ConfigService] Use 'new_option' instead
[ConfigService] 'old_option' will be removed in v2.0
```

---

### Default Value Changes

**Policy:** Default changes announced in release notes

**Example:**

**v1.0:**
```conf
# Default: stratum=true
```

**v1.1:**
```conf
# Default: stratum=false (changed for security)
```

**Migration:**
- Explicit configs unaffected: `stratum=true` still works
- Implicit defaults changed: Users relying on default must update config
- Announced in release notes

**Best Practice:**
- Always set critical options explicitly
- Don't rely on defaults for production

---

## Validation & Errors

### Invalid Config Detection

**Daemon validates config on startup:**

1. **Unknown Keys** (Warning)
   ```
   [ConfigService] WARNING: Unknown config key 'typo_option'
   [ConfigService] Ignoring unknown key
   ```

2. **Invalid Values** (Error - daemon refuses to start)
   ```
   [ConfigService] ERROR: Invalid value for 'rpcport': 'abc'
   [ConfigService] Expected: integer 1024-65535
   [ConfigService] Daemon will not start
   ```

3. **Mutually Exclusive** (Error - daemon refuses to start)
   ```
   [ConfigService] ERROR: Cannot enable both 'testnet' and 'regtest'
   [ConfigService] Choose one network mode
   [ConfigService] Daemon will not start
   ```

4. **Missing Required** (Error - daemon refuses to start)
   ```
   [ConfigService] ERROR: 'rpcuser' is required for RPC access
   [ConfigService] Set 'rpcuser' and 'rpcpassword' in dinero.conf
   [ConfigService] Daemon will not start
   ```

---

## Rollback Procedure

### Downgrading from v1.1 to v1.0

**Scenario:** You upgraded to v1.1 but want to rollback to v1.0

**Steps:**

1. **Stop v1.1 daemon:**
   ```bash
   dinero-cli stop
   ```

2. **Check config compatibility:**
   - If config uses v1.1-only features → Manual edit required
   - If config is v1.0-compatible → Safe to rollback

3. **Install v1.0 binaries:**
   ```bash
   # Restore v1.0 binaries
   cp dinerod-v1.0.0 /usr/local/bin/dinerod
   ```

4. **Optional: Revert config_version:**
   ```conf
   # dinero.conf
   config_version=1
   ```

5. **Start v1.0 daemon:**
   ```bash
   dinerod
   ```

**Safe:** v1.0 ignores v1.1-specific config keys (unknown keys are warnings, not errors).

---

## Testing Migrations

### Pre-Upgrade Checklist

Before upgrading to new version:

- [ ] Backup `dinero.conf`
- [ ] Backup `wallet.dat`
- [ ] Read release notes (breaking changes)
- [ ] Test on testnet/regtest first
- [ ] Verify disk space (for reindex if needed)

### Testnet Testing

**Recommended:** Always test upgrades on testnet first.

**Steps:**
```bash
# 1. Stop mainnet daemon
dinero-cli stop

# 2. Copy config to testnet directory
cp ~/.dinero/dinero.conf ~/.dinero_testnet/dinero.conf

# 3. Edit testnet config
nano ~/.dinero_testnet/dinero.conf
# Add: testnet=true

# 4. Start testnet daemon with new version
dinerod-v1.1.0 --datadir=~/.dinero_testnet

# 5. Verify migration success
dinero-cli --datadir=~/.dinero_testnet getblockchaininfo

# 6. If successful, upgrade mainnet
```

---

## Common Migration Scenarios

### Scenario 1: Fresh Install (v1.0)

**No migration needed.**

**Steps:**
1. Install v1.0 binaries
2. Create `dinero.conf` (see CONFIGURATION.md)
3. Start daemon

---

### Scenario 2: Upgrade v0.x → v1.0

**Status:** NOT APPLICABLE (v1.0 is initial release)

If v0.x existed:
- Full migration guide would be provided
- Likely requires database migration
- Reindex may be required

---

### Scenario 3: Upgrade v1.0 → v1.1 (Future)

**Status:** FUTURE (not yet released)

**Planned Migration:**
- Automatic config migration
- No reindex required (unless noted in release notes)
- Backward compatible

**Steps:**
1. Backup config
2. Install v1.1 binaries
3. Start daemon
4. Daemon auto-migrates config_version=1 → 2
5. Verify with `dinero-cli getblockchaininfo`

---

## Emergency Rollback

### Daemon Won't Start After Upgrade

**Symptom:** Daemon crashes or refuses to start after upgrade

**Emergency Procedure:**

1. **Check logs:**
   ```bash
   tail -n 100 ~/.dinero/debug.log
   ```

2. **Identify error:**
   - Config error? → Fix config
   - Database error? → Restore backup
   - Binary error? → Rollback binaries

3. **Rollback binaries:**
   ```bash
   # Restore previous version
   cp dinerod-v1.0.0 /usr/local/bin/dinerod
   dinerod
   ```

4. **Restore config backup:**
   ```bash
   cp ~/.dinero/dinero.conf.bak ~/.dinero/dinero.conf
   dinerod
   ```

5. **Restore database backup (last resort):**
   ```bash
   # Stop daemon
   dinero-cli stop

   # Restore backup
   rm -rf ~/.dinero/blocks
   rm -rf ~/.dinero/chainstate
   cp -r ~/.dinero_backup/blocks ~/.dinero/
   cp -r ~/.dinero_backup/chainstate ~/.dinero/

   # Start daemon
   dinerod
   ```

---

## Deprecation Warnings

### v1.0 Deprecations

**None.** (Initial release)

---

### v1.1 Deprecations (Future)

**Status:** NOT YET DETERMINED

Potential deprecations will be announced:
- 6 months before v1.1 release
- In release notes
- In this document

---

## Configuration Freeze Policy

### Frozen Periods

**v1.0.x series:**
- config_version=1 (FROZEN)
- No breaking changes allowed
- Only additive changes (new keys with defaults)

**v1.1.x series:**
- config_version=2 (future)
- Breaking changes allowed (with migration)

---

## Audit Trail

Phase Z.2 establishes **configuration migration guarantees**:

1. **Phase D** - Consensus frozen ✅
2. **Phase E** - Safety infrastructure ✅
3. **Phase Z.1** - Reproducible builds ✅
4. **Phase Z.2** - Configuration guarantees ← **YOU ARE HERE** 🔨

**Next:** Phase Z.3 (RPC Compatibility Contract)

---

**Phase Z.2: Configuration Migration - Complete**

**Migration guarantee:**
- ✅ Automatic migration (where possible)
- ✅ Manual migration documented
- ✅ Rollback procedure documented
- ✅ Deprecation cycle: 1 major version
- ✅ Backward compatibility maintained

**Operators can upgrade safely.**
