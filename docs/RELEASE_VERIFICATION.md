# DineroCoin Release Verification

**Purpose:** Deterministic verification that a release tag contains exactly what was built and tested.

**Use this checklist:** Before announcing any release, after pushing tags to origin.

---

## Verification Checklist

Run these steps in order. **All checks must pass** before considering a release valid.

### 1️⃣ Verify Tag Points to Expected Commit

**Purpose:** Confirm the tag wasn't accidentally moved or points to wrong commit.

```bash
git fetch --tags --force
git show v1.0.0 --no-patch
```

**Expected output:**
- Commit hash matches your release commit
- Commit message matches expected release
- Date is today (for new releases)

**If hash is wrong:** STOP. Investigate why tag points to unexpected commit.

---

### 2️⃣ Verify main and Release Tag Relationship

**Purpose:** Ensure no unexpected divergence between branches and tags.

```bash
echo "main:    $(git rev-parse main)"
echo "v1.0.0:  $(git rev-parse v1.0.0)"
```

**Expected:**
- For initial release: Hashes match exactly
- For documentation updates: main may be 1 commit ahead (tagged as vX.Y.Z-doc1)

**If hashes differ unexpectedly:** Investigate the divergence.

---

### 3️⃣ Verify All Expected Files Exist in Tag

**Purpose:** Confirm release actually contains the deliverables you built.

```bash
git ls-tree -r --name-only v1.0.0 | grep -E \
"REPRODUCIBLE_BUILDS|DEPENDENCIES|CONFIGURATION|CONFIG_MIGRATION|RPC_COMPATIBILITY|RELEASE_CHECKLIST|build-deterministic|release-verify|rpc-examples"
```

**Expected output for v1.0.0 (Phase Z):**
```
contrib/build-deterministic.sh
contrib/dinero.conf.example
contrib/release-verify.sh
contrib/rpc-examples.sh
docs/CONFIGURATION.md
docs/CONFIG_MIGRATION.md
docs/DEPENDENCIES.md
docs/RELEASE_CHECKLIST_V1.md
docs/REPRODUCIBLE_BUILDS.md
docs/RPC_API.md
docs/RPC_COMPATIBILITY.md
```

**If files are missing:** Release is incomplete. DO NOT announce.

---

### 4️⃣ Verify Commit History is Correct

**Purpose:** Ensure release contains expected development history, not old code.

```bash
git log --oneline v1.0.0 --max-count=10
```

**Expected for v1.0.0:**
```
a03664eb DineroCoin v1.0.0 - Mainnet Launch Release Notes
ea163013 Phase Z.4: Release Readiness Checklist
f7a9e4cf Phase Z.3: RPC API Compatibility Contract
7e0a9ff8 Phase Z.2: Configuration Guarantees
908099d1 Phase Z.1: Reproducible Builds Foundation
```

**If commits are missing or unexpected:** Tag may point to wrong commit tree.

---

### 5️⃣ Verify Working Tree is Clean

**Purpose:** Ensure build was from tagged release state, not dirty tree.

```bash
git checkout v1.0.0
git status
```

**Expected:**
- "nothing to commit, working tree clean" OR
- Only build artifacts differ (CMake generated files, build directories)

**Verify source files specifically:**
```bash
git diff --name-only docs/ src/ include/ contrib/ | grep -E '\.(md|sh|cpp|h|conf)$'
```

**Expected:** No output (no source file differences)

**If source files differ:** Build was from dirty tree. Rebuild from clean checkout.

---

### 6️⃣ Verify Consensus Anchor is Unchanged

**Purpose:** Ensure consensus-critical parameters haven't drifted.

```bash
git show consensus-v1.0.0 --no-patch
```

**Expected:**
- Tag still exists
- Points to same commit: `8d809d3f2d26b0da80b71cba6509eb9aa217f681`
- Tag date hasn't changed

**Verify parameters in code:**
```bash
git show consensus-v1.0.0:include/consensus/subsidy.h | grep -E \
"GENESIS_TIME|MAX_SUPPLY|PREMINE_DIN|HALVING_INTERVAL|INITIAL_SUBSIDY"
```

**Expected output:**
```cpp
static constexpr uint32_t GENESIS_TIME = 1772496000;  // 2026-03-03 00:00:00 UTC
static constexpr uint64_t INITIAL_SUBSIDY = 100ULL * UNA_PER_DIN;  // 100 DIN per block
static constexpr uint32_t HALVING_INTERVAL = 1314000;  // 5.0 years @ 2 min blocks
static constexpr uint64_t MAX_SUPPLY_UNA = 265428000ULL * UNA_PER_DIN;
static constexpr uint64_t PREMINE_DIN  = 2627900ULL;
```

**If any value changed:** CRITICAL - consensus parameters were modified. This requires hard fork coordination.

---

### 7️⃣ (Optional) Rebuild from Tag for Bit-for-Bit Verification

**Purpose:** Ultimate proof the release is deterministically reproducible.

**⚠️  WARNING:** This will delete your build directory!

```bash
git clean -xfd
git checkout v1.0.0
./contrib/build-deterministic.sh
```

**Compare binary hashes:**
```bash
shasum -a 256 build/bin/dinerod
```

Compare this hash to:
- Hash from your original build
- Hash in release notes
- Hash published for users

**If hashes match:** Absolute proof of reproducibility ✅

**If hashes differ:** Build is not deterministic. Investigate before releasing.

---

## Verification Results Template

Copy this template and fill in results:

```markdown
# Release Verification: vX.Y.Z

**Date:** YYYY-MM-DD
**Verifier:** [Your name]
**Commit:** [git rev-parse vX.Y.Z]

## Checklist

- [ ] 1. Tag points to expected commit
- [ ] 2. main/tag relationship verified
- [ ] 3. All expected files present in tag
- [ ] 4. Commit history correct
- [ ] 5. Working tree clean
- [ ] 6. Consensus anchor unchanged
- [ ] 7. (Optional) Rebuild verification passed

## Notes

[Any issues, warnings, or observations]

## Result

✅ PASS - Release verified and ready for announcement
❌ FAIL - Do not announce, investigate issues above
```

---

## Common Issues and Solutions

### Issue: "Tag points to unexpected commit"

**Cause:** Tag was moved or created from wrong branch

**Solution:**
1. Delete local tag: `git tag -d vX.Y.Z`
2. Fetch from origin: `git fetch --tags --force`
3. If still wrong, check with team before proceeding

### Issue: "Files missing from tag"

**Cause:** Files weren't committed before tagging

**Solution:**
1. DO NOT force-push or move tag
2. Create vX.Y.Z-r1 tag with fixes (see TAGGING_POLICY.md)
3. Document the issue in release notes

### Issue: "Working tree has source changes"

**Cause:** Built from modified files instead of clean checkout

**Solution:**
1. Discard changes: `git checkout -- .`
2. Rebuild from clean state
3. Regenerate release binaries
4. Update published hashes if already announced

### Issue: "Consensus parameters changed"

**Cause:** Critical - consensus code was modified

**Solution:**
1. STOP ALL RELEASES IMMEDIATELY
2. Notify all team members
3. Determine if change was intentional
4. If unintentional: Revert and rebuild
5. If intentional: Requires coordinated hard fork process

---

## Automation

This verification can be partially automated. Example CI check:

```yaml
name: Release Verification

on:
  push:
    tags:
      - 'v*.*.*'

jobs:
  verify-release:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
        with:
          fetch-depth: 0

      - name: Verify tag points to main
        run: |
          TAG_COMMIT=$(git rev-parse ${{ github.ref_name }})
          MAIN_COMMIT=$(git rev-parse origin/main)
          if [ "$TAG_COMMIT" != "$MAIN_COMMIT" ]; then
            echo "❌ Tag does not point to main HEAD"
            exit 1
          fi

      - name: Verify consensus anchor unchanged
        run: |
          EXPECTED="8d809d3f2d26b0da80b71cba6509eb9aa217f681"
          ACTUAL=$(git rev-list -n 1 consensus-v1.0.0)
          if [ "$ACTUAL" != "$EXPECTED" ]; then
            echo "❌ CRITICAL: consensus-v1.0.0 tag moved!"
            exit 1
          fi

      - name: Verify expected files exist
        run: |
          git ls-tree -r --name-only ${{ github.ref_name }} | \
          grep -E "REPRODUCIBLE_BUILDS|CONFIGURATION|RPC_COMPATIBILITY|RELEASE_CHECKLIST" || {
            echo "❌ Expected files missing from release"
            exit 1
          }
```

---

## See Also

- **docs/TAGGING_POLICY.md** - Tag governance and immutability rules
- **docs/RELEASE_CHECKLIST_V1.md** - Complete pre-release checklist
- **docs/REPRODUCIBLE_BUILDS.md** - Deterministic build procedures
- **contrib/release-verify.sh** - Automated verification script

---

**Remember:** Verification is not optional. Every release MUST pass all checks before announcement.

**When in doubt:** Re-verify. Better to delay a release than announce a broken one.
