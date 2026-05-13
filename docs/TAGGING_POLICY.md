# DineroCoin Git Tag Policy

**Last Updated:** 2025-12-31
**Status:** CANONICAL - All contributors must follow this policy

---

## Immutability Anchor

**DineroCoin consensus v1.0.0 is defined at tag `consensus-v1.0.0`.**

This tag:
- **MUST NEVER MOVE** - It is the permanent reference point for consensus rules
- **MUST be cited** in all consensus-related documentation
- **MUST be referenced** by miners, exchanges, auditors, and node operators
- Contains the canonical consensus parameters frozen at mainnet launch

**Consensus Parameters at `consensus-v1.0.0`:**
```
Genesis Time: 1772496000 (2026-03-03 00:00:00 UTC)
Genesis Hash: 00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74
Block Time: 120 seconds (2 minutes)
Initial Reward: 100 DIN per block
Halving Interval: 1,314,000 blocks (~5 years)
Max Supply: 265,428,000 DIN
Premine: 2,627,900 DIN (~1%)
Network Magic: 0xd9b4bef9
```

---

## Tag Types

### 1. Consensus Tags (`consensus-vX.Y.Z`)

**Purpose:** Define immutable consensus rules for hard fork coordination

**Rules:**
- ✅ **IMMUTABLE** - Once published, NEVER delete or move
- ✅ Tag the exact commit where consensus rules are frozen
- ✅ Must include full consensus parameters in tag message
- ✅ Must be GPG-signed by release manager
- ✅ Referenced by all nodes for hard fork activation

**Format:**
```bash
git tag -a consensus-v1.0.0 -m "Consensus rules for DineroCoin v1.0.0

Genesis: 1772496000 (2026-03-03 00:00:00 UTC)
Genesis Hash: 00000018a388c95d48c7141bb86f131c3538954d57acd07806d1f62bcfa9fd74
Block Time: 120 seconds
Initial Reward: 100 DIN
Halving Interval: 1,314,000 blocks
Max Supply: 265,428,000 DIN
Premine: 2,627,900 DIN
Network Magic: 0xd9b4bef9"
```

### 2. Semantic Version Tags (`vX.Y.Z`)

**Purpose:** Mark software releases (binaries, documentation, features)

**Rules:**
- ✅ **IMMUTABLE ONCE PUBLISHED** - Do not delete or force-push
- ✅ For documentation-only fixes, use `-doc` or `-r` suffix
- ❌ Do NOT retag if parameters are wrong - use errata document instead
- ❌ Do NOT force-push to fix mistakes - transparency over perfection

**Format:**
```bash
git tag -a v1.0.0 -m "DineroCoin v1.0.0 - Mainnet Launch

[Software release notes here]

Consensus: See consensus-v1.0.0 tag for canonical parameters"
```

**Documentation Updates:**
```bash
# Correct way to update docs for v1.0.0
git tag -a v1.0.0-doc1 -m "Documentation clarification for v1.0.0

- Fixed incorrect consensus parameters in release notes
- No code changes
- Consensus rules unchanged (see consensus-v1.0.0)"
```

### 3. Phase Tags (`phase-X.Y`)

**Purpose:** Mark completion of development phases

**Rules:**
- ✅ Can be moved during development (pre-release only)
- ✅ Become immutable once referenced in mainnet release
- ✅ Used for milestone tracking and progress reporting

**Format:**
```bash
git tag -a phase-z.4 -m "Phase Z.4: Release Readiness Complete"
```

---

## Tag History & Errata

### v1.0.0 Tag History

**Original `v1.0.0` (DEPRECATED):**
- Commit: `621d127c` (older production readiness work)
- Contained incorrect consensus parameters (copied from Bitcoin)
- **Status:** This tag was force-pushed on 2025-12-31

**Current `v1.0.0`:**
- Commit: `a03664eb` (mainnet launch release notes)
- Contains correct DineroCoin consensus parameters
- **Status:** Active, but prefer `consensus-v1.0.0` for consensus verification

**Lesson Learned:**
- Force-pushing version tags breaks immutability guarantees
- Created this policy to prevent future occurrences
- `consensus-v1.0.0` is the true immutability anchor

**Going Forward:**
- All consensus verification MUST reference `consensus-v1.0.0`
- Miners, exchanges, auditors: Use `consensus-v1.0.0` as canonical reference
- Any discrepancies in `v1.0.0` tag are documentation issues only

---

## Best Practices

### Creating a New Release

1. **Tag the consensus first** (if consensus changes):
   ```bash
   git tag -a consensus-v2.0.0 -m "Consensus rules for hard fork v2.0.0

   [Full consensus parameters]"
   ```

2. **Tag the software release**:
   ```bash
   git tag -a v2.0.0 -m "DineroCoin v2.0.0 Release

   [Release notes]

   Consensus: See consensus-v2.0.0 tag"
   ```

3. **Push both tags**:
   ```bash
   git push origin consensus-v2.0.0 v2.0.0
   ```

### Fixing Documentation Mistakes

**WRONG ❌:**
```bash
git tag -d v1.0.0
git tag -a v1.0.0 -m "Fixed version"
git push origin v1.0.0 --force  # NEVER DO THIS
```

**CORRECT ✅:**
```bash
# Option 1: Create errata document
echo "v1.0.0 tag contains incorrect parameters in message.
Canonical consensus: See consensus-v1.0.0 tag" > ERRATA.md
git commit -m "Add errata for v1.0.0 tag"

# Option 2: Create documentation update tag
git tag -a v1.0.0-doc1 -m "Clarification: Use consensus-v1.0.0 for parameters"
```

### Verifying Consensus

**Always verify against the consensus tag:**
```bash
# Check current consensus version
git show consensus-v1.0.0

# Verify genesis hash matches
dinerod --version  # Should reference consensus-v1.0.0

# Audit consensus parameters
grep -r "GENESIS_TIME\|HALVING_INTERVAL\|MAX_SUPPLY" include/consensus/
```

---

## Enforcement

### Pre-Push Hook

Consider adding to `.git/hooks/pre-push`:
```bash
#!/bin/bash
# Prevent force-pushing version tags

while read local_ref local_sha remote_ref remote_sha; do
    if [[ "$remote_ref" =~ refs/tags/v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        if [ "$remote_sha" != "0000000000000000000000000000000000000000" ]; then
            echo "❌ ERROR: Cannot force-push version tag $remote_ref"
            echo "Version tags are immutable once published"
            echo "See docs/TAGGING_POLICY.md for documentation updates"
            exit 1
        fi
    fi
done

exit 0
```

### CI Validation

Add to CI pipeline:
```yaml
- name: Validate tag immutability
  run: |
    # Ensure consensus-v1.0.0 still points to same commit
    EXPECTED="8d809d3f2d26b0da80b71cba6509eb9aa217f681"
    ACTUAL=$(git rev-list -n 1 consensus-v1.0.0)
    if [ "$ACTUAL" != "$EXPECTED" ]; then
      echo "❌ CRITICAL: consensus-v1.0.0 tag moved!"
      exit 1
    fi
```

---

## Summary

**Golden Rules:**
1. `consensus-vX.Y.Z` tags are the source of truth for network rules
2. `vX.Y.Z` tags are immutable once pushed to origin
3. Documentation fixes use `-doc` or `-r` suffixes, never force-push
4. Transparency beats perfection - document mistakes openly
5. When in doubt, create a new tag, never modify an existing one

**For v1.0.0 Mainnet:**
- **Consensus Reference:** `consensus-v1.0.0` (immutable anchor)
- **Software Release:** `v1.0.0` (current, with errata noted above)
- **Miners/Exchanges/Auditors:** Always reference `consensus-v1.0.0`

---

**This policy prevents the v1.0.0 force-push mistake from ever happening again.**
