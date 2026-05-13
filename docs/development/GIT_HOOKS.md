# Git Hooks

## Overview

DineroCoin uses git hooks to enforce consensus safety and release discipline.

## pre-push Hook

Location: `.git/hooks/pre-push`

### Policy 1: Tag Requirement (Protected Branches)

**Rule:** Every commit on `main` and `release/*` branches must be tagged before pushing.

**Rationale:** Ensures every commit on main is a release point with semantic versioning.

**Enforcement:**
```bash
BRANCH="$(git rev-parse --abbrev-ref HEAD)"
case "$BRANCH" in
  main|release/*)
    if ! git describe --exact-match --tags >/dev/null 2>&1; then
      echo "Refuse push: HEAD must be exactly at a tag on $BRANCH"
      exit 1
    fi
  ;;
esac
```

**If push is rejected:**
```bash
# Tag your commit
git tag v2.2.2 -a -m "Release message"

# Push commit and tag
git push origin main
git push origin v2.2.2
```

### Policy 2: Build Check (Hermetic Builds)

**Status:** Skipped in git hooks (as of v2.2.1)

**Rationale:** DineroCoin requires hermetic builds with `SOURCE_DATE_EPOCH` set for deterministic compilation (see CMakeLists.txt:20). Git hooks run without this variable, causing CMake to fail.

**Correct workflow:**

1. **Run hermetic builds manually:**
   ```bash
   ./scripts/hermetic-build.sh test_serialization_vectors
   ```

2. **Run tests manually:**
   ```bash
   ./build_hermetic/tests/consensus/test_serialization_vectors
   ```

3. **Tag your commit:**
   ```bash
   git tag v2.2.2 -a -m "Release v2.2.2"
   ```

4. **Push:**
   ```bash
   git push origin main  # Hook allows this (build check skipped)
   git push origin v2.2.2
   ```

**Hook logic:**
```bash
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
  echo "ℹ️  Skipping build check in git hook"
  echo "   Use './scripts/hermetic-build.sh <target>' for hermetic builds"
  exit 0  # Skip build, allow push
fi
# If SOURCE_DATE_EPOCH is set, proceed with build check
```

### Bypassing Hooks (Emergency Use Only)

For documentation-only changes or emergency fixes:
```bash
git push --no-verify origin main
```

**Warning:** This skips **all** pre-push checks including tag enforcement. Use only when necessary.

## pre-commit Hook

Location: `.git/hooks/pre-commit` → symlinked to `../../scripts/pre-commit-hook.sh`

**Checks:**
- Banned global variables (detects consensus-breaking patterns)
- See `scripts/pre-commit-hook.sh` for details

## Troubleshooting

### "Refuse push: HEAD must be exactly at a tag on main"

**Cause:** You're pushing to main without a tag.

**Fix:**
```bash
git tag v2.2.2
git push origin main
git push origin v2.2.2
```

### "HERMETIC BUILD VIOLATION: SOURCE_DATE_EPOCH is not set"

**Cause:** Old pre-push hook (before v2.2.1) tried to run CMake directly.

**Fix:** Update `.git/hooks/pre-push` with the fixed version that skips build checks.

**Verify fix:**
```bash
cat .git/hooks/pre-push | grep "SOURCE_DATE_EPOCH"
# Should show: if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
```

### Hook won't execute

**Cause:** Hook not executable.

**Fix:**
```bash
chmod +x .git/hooks/pre-push
chmod +x .git/hooks/pre-commit
```

---

**Updated:** 2026-01-05 (v2.2.1)
**See also:** `docs/consensus/DETERMINISTIC_SERIALIZATION.md` (hermetic build rationale)
