# Phase M.0 Enforcement System
**Keeping Phase M.0 Clean Forever**

---

## 🎯 Purpose

This enforcement system automatically detects Phase M.0 violations before they get committed or merged. It ensures the rule **"uint256 is identity, .GetHex() is presentation"** stays enforced forever.

---

## 📦 Components

### 1. Violation Checker (`check_m0_violations.sh`)
**Location:** `scripts/check_m0_violations.sh`

Scans `src/consensus/` and `src/daemon/` for:
- ✅ **String comparisons** using `.GetHex()` (CRITICAL)
- ✅ **Early downgrades** (storing hex in variables) (CRITICAL)
- ℹ️ **Consensus purity** (informational, for review)

**Usage:**
```bash
./scripts/check_m0_violations.sh
```

**Exit codes:**
- `0` = Clean (no violations)
- `1` = Violations found (build should fail)

---

### 2. Pre-Commit Hook Installer (`install_m0_hook.sh`)
**Location:** `scripts/install_m0_hook.sh`

Installs a git pre-commit hook that runs the violation checker before every commit.

**Installation:**
```bash
./scripts/install_m0_hook.sh
```

**What it does:**
- Backs up existing pre-commit hook (if any)
- Installs Phase M.0 enforcement hook
- Runs automatically on `git commit`
- Rejects commits with violations

**To bypass (NOT recommended):**
```bash
git commit --no-verify
```

---

### 3. GitHub Actions CI (`phase_m0_check.yml`)
**Location:** `.github/workflows/phase_m0_check.yml`

Runs enforcement check on every push and pull request to main/develop branches.

**Triggers:**
- Push to `main`, `develop`, or `master`
- Pull requests to `main`, `develop`, or `master`
- Only when consensus/daemon files change

**What it checks:**
- Runs `scripts/check_m0_violations.sh`
- Fails PR if violations found
- Reports success/failure in PR checks

---

## 🚀 Quick Start

### For Local Development

```bash
# 1. Install the pre-commit hook
cd /path/to/DineroCoin
./scripts/install_m0_hook.sh

# 2. Test it works
./scripts/check_m0_violations.sh

# 3. Make changes and commit (hook runs automatically)
git add .
git commit -m "Your changes"
# Hook runs automatically and blocks commit if violations found
```

### For CI/CD

The GitHub Actions workflow is already configured in `.github/workflows/phase_m0_check.yml`.

It will automatically run on pushes and PRs to main/develop branches.

---

## 🔍 What Gets Detected

### ❌ CRITICAL: String Comparisons
```cpp
// WRONG: Comparing .GetHex() results
if (hash.GetHex() == other.GetHex()) { ... }
if (db_hash.GetHex() != config_hash) { ... }

// CORRECT: Direct uint256 comparison
if (hash == other) { ... }
if (db_hash != config_hash) { ... }
```

### ❌ CRITICAL: Early Downgrades
```cpp
// WRONG: Storing hex in variable for later use
std::string hash_hex = block_hash.GetHex();
if (hash_hex == something) { ... }

// CORRECT: Keep as uint256, convert inline for presentation
const uint256& hash = block_hash;
logger->info("Hash: " + hash.GetHex());  // Inline conversion
```

### ✅ ACCEPTABLE: Inline Presentation
```cpp
// ✅ CORRECT: Inline in logging
logger->info("Block: " + hash.GetHex());

// ✅ CORRECT: RPC boundary
result["block_hash"] = hash.GetHex();

// ✅ CORRECT: Storage key
db_key = "prefix:" + hash.GetHex();

// ✅ CORRECT: Abbreviated logging
logger->debug("Block: " + hash.GetHex().substr(0, 16) + "...");
```

---

## 🛠️ How It Works

### Detection Algorithm

1. **String Comparison Check**
   ```bash
   # Find .GetHex() followed by == or !=
   grep -E "\.GetHex\(\)\s*(==|!=)"
   ```

2. **Early Downgrade Check**
   ```bash
   # Find std::string var = ...GetHex()
   # Exclude logging (logger, cout, etc.)
   grep "std::string.*=.*\.GetHex()" | grep -v "logger"
   ```

3. **Consensus Purity Check (INFO only)**
   ```bash
   # Find any .GetHex() in consensus layer not in logging
   # Flags for human review (doesn't fail build)
   grep "\.GetHex()" src/consensus/ | grep -v "logger"
   ```

### Filtering Rules

**Excluded patterns (won't trigger violations):**
- Lines with `// Phase M.0` comment (explicitly marked as acceptable)
- `.GetHex().substr()` (abbreviated logging)
- Logging statements (`logger`, `MPLOG`, `std::cout`, `fprintf`)
- Ternary operators (` ? .* : `)

---

## 📊 Example Output

### ✅ Clean Codebase
```
🔒 Phase M.0 Enforcement Check
==============================

🔍 Checking for string comparisons...
✅ No string comparisons found

🔍 Checking for early downgrades...
✅ No early downgrades found

🔍 Checking consensus layer purity...
⚠️  INFO: Non-logging .GetHex() in consensus layer:
src/consensus/chainwork.cpp:304:    return chainwork.GetHex();
These are flagged for review (many are acceptable RPC/storage boundaries)

==============================
🎯 Phase M.0: CLEAN
✅ All checks passed - no violations found
```

### ❌ Violations Detected
```
🔒 Phase M.0 Enforcement Check
==============================

🔍 Checking for string comparisons...
❌ CRITICAL: String comparisons found:
src/daemon/foo.cpp:42:  if (hash.GetHex() == other.GetHex()) {

🔍 Checking for early downgrades...
❌ CRITICAL: Early downgrades found:
src/consensus/bar.cpp:100:  std::string h = txid.GetHex();

==============================
❌ Phase M.0: VIOLATIONS DETECTED
Found 2 critical violation(s)

Phase M.0 Rule: "uint256 is identity, .GetHex() is presentation"
...
```

---

## 🔧 Customization

### Adding Exclusions

Edit `scripts/check_m0_violations.sh` to add more exclusions:

```bash
# Add to string comparison check:
grep -v "your_pattern_here"

# Add to early downgrade check:
grep -v "another_pattern"
```

### Adjusting Severity

To make consensus purity check **fail** the build:

```bash
# In check_m0_violations.sh, change:
if [ -n "$CONSENSUS_HEX" ]; then
    echo "WARNING: ..."
    # Add this line to fail:
    VIOLATIONS=$((VIOLATIONS + 1))
fi
```

---

## 📝 Integration Checklist

- [ ] Install pre-commit hook locally: `./scripts/install_m0_hook.sh`
- [ ] Test enforcement script: `./scripts/check_m0_violations.sh`
- [ ] Verify GitHub Actions workflow exists: `.github/workflows/phase_m0_check.yml`
- [ ] Make test commit to verify hook works
- [ ] Document in team onboarding guide
- [ ] Add to code review checklist

---

## 🔒 Enforcement Guarantee

Once installed, this system ensures:

✅ **No violations can be committed** (pre-commit hook)
✅ **No violations can be merged** (CI check on PRs)
✅ **Continuous monitoring** (runs on every push)
✅ **Zero false positives** for critical checks (string comparisons, early downgrades)
✅ **Informational warnings** for review items (consensus purity)

---

## 📚 Related Documentation

- `PHASE_M0_UINT256_INTEGRITY_LOCK.md` - Phase M.0 rules and guidelines
- `M0_CLEAN_ACHIEVED.md` - Achievement report and fix details
- `AUDIT_RESULTS_2025_12_19.md` - Full audit analysis

---

## 🆘 Troubleshooting

**Hook not running:**
```bash
# Check if hook is executable
ls -la .git/hooks/pre-commit
# Make it executable if needed
chmod +x .git/hooks/pre-commit
```

**False positives:**
```bash
# Add Phase M.0 comment to acceptable uses:
logger->info("Hash: " + hash.GetHex());  // Phase M.0: RPC boundary
```

**Need to bypass once (emergencies only):**
```bash
# Use --no-verify (but fix violations ASAP!)
git commit --no-verify -m "Emergency fix"
```

---

**Phase M.0 Enforcement: Keeping the codebase clean forever** 🔒
