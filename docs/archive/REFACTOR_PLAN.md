# Surgical Refactor Plan - Make Address-Based Ownership Impossible

**Duration:** 2-3 days  
**Status:** Ready to execute  
**Safety:** All tests must pass after each phase

---

## Phase 1: Function Renaming (Low Risk, High Signal)
**Time:** 4-6 hours  
**Goal:** Names enforce correct usage

### 1.1 Rename Address-Taking Functions → scriptPubKey-Taking

**Files to touch:**
- `src/wallet/wallet_manager.cpp` (or wherever wallet functions live)
- `src/core/wallet/wallet_manager.cpp`

**Changes:**
```cpp
// BEFORE (misleading)
PrivateKey getPrivateKeyForAddress(const std::string& address);
bool hasAddress(const std::string& address);

// AFTER (truth)
PrivateKey deriveKeyForScriptPubKey(const std::vector<uint8_t>& scriptPubKey);
bool hasScriptPubKey(const std::vector<uint8_t>& scriptPubKey);
```

**Search targets:**
```bash
grep -rn "getPrivateKeyForAddress\|getKeyForAddress" src/
grep -rn "hasAddress.*(" src/ | grep -v "hasAddressType"
```

**Done criteria:**
- ✅ No functions named `get*ForAddress` in wallet code
- ✅ All tests pass
- ✅ Grep shows only display/RPC functions use "address" in names

---

## Phase 2: Separate Display from Ownership (Medium Risk, Critical)
**Time:** 8-12 hours  
**Goal:** Ownership logic never touches address strings

### 2.1 Extract Address Encoding to Separate Module

**Create:**
- `src/wallet/address_encoding.cpp` (display only)
- `src/wallet/script_ownership.cpp` (consensus only)

**Move:**
```cpp
// address_encoding.cpp - DISPLAY ONLY
std::string encodeAddress(const std::vector<uint8_t>& scriptPubKey, const std::string& hrp);
std::vector<uint8_t> decodeAddress(const std::string& address); // RPC input only

// script_ownership.cpp - CONSENSUS ONLY  
bool canSpendScriptPubKey(const std::vector<uint8_t>& scriptPubKey);
KeyOriginInfo getKeyOriginForScriptPubKey(const std::vector<uint8_t>& scriptPubKey);
```

**Header comments:**
```cpp
// address_encoding.h
// ⚠️ DISPLAY ONLY - NEVER USE FOR OWNERSHIP
// Address strings are for UI/RPC presentation ONLY
// scriptPubKey determines ownership (see script_ownership.h)

// script_ownership.h
// ✅ CONSENSUS-CRITICAL - OWNERSHIP LOGIC
// UTXO ownership determined by scriptPubKey ONLY
// Address strings are irrelevant (see address_encoding.h for display)
```

**Done criteria:**
- ✅ `script_ownership.cpp` has zero `std::string address` parameters
- ✅ `address_encoding.cpp` has zero DB queries
- ✅ Clear module boundary (ownership vs display)
- ✅ All tests pass

---

## Phase 3: Fix Dangerous Address Comparisons (High Risk, Critical)
**Time:** 6-8 hours  
**Goal:** Remove address-based logic from ownership paths

### 3.1 Fix wallet.cpp:57,68 (Flagged by Test #3)

**File:** `src/daemon/wallet.cpp`

**Current (DANGEROUS):**
```cpp
if (address[0] == 'H' || address[0] == '1') {
    // legacy address handling
} else if (address[0] == '7' || address[0] == '3') {
    // script address handling  
}
```

**Analysis needed:**
- Is this for **RPC input validation**? (✅ safe, keep)
- Is this for **ownership determination**? (❌ dangerous, remove)

**If RPC validation (safe):**
```cpp
// RPC input parsing only - NOT ownership
if (!isValidAddressFormat(address)) {
    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
}
// Then immediately convert to scriptPubKey
scriptPubKey = decodeAddress(address);
// ALL subsequent logic uses scriptPubKey
```

**If ownership logic (must remove):**
```cpp
// WRONG - delete this
if (address[0] == 'H') { ... }

// RIGHT - replace with
if (scriptPubKey[0] == 0x00 && scriptPubKey.size() == 22) {
    // P2WPKH
}
```

**Done criteria:**
- ✅ No address prefix checks in ownership paths
- ✅ RPC input validation clearly separated
- ✅ Comments explain what is safe vs dangerous
- ✅ All tests pass

### 3.2 Add Guardrail Comments

**Files:** Any file with address handling

**Add at top:**
```cpp
// ⚠️ ADDRESS STRING USAGE RULES
// ✅ ALLOWED: RPC input/output, display, logging
// ❌ FORBIDDEN: Ownership, UTXO matching, spend authorization
// 
// Ownership is determined by scriptPubKey ONLY.
// See docs/architecture/scriptpubkey-ownership.md
```

**Done criteria:**
- ✅ Every file touching addresses has usage rules comment
- ✅ New developers see guardrails immediately

---

## Phase 4: Database Query Audit (Medium Risk, Time-Boxed)
**Time:** 4-6 hours MAX  
**Goal:** Understand which queries are safe vs need fixing

### 4.1 Categorize Flagged Queries

Test #3 flagged these queries. Categorize each:

```bash
# From Test #3 output:
src/wallet/wallet_manager.cpp:1782: SELECT address FROM addresses WHERE wallet_id = ?
```

**For each query, determine:**

**Category A - Safe (UI/listing):**
- `SELECT address FROM addresses` for display → ✅ Keep as-is
- Comment: `// Display only - not used for ownership`

**Category B - Needs script_pubkey:**
- `SELECT ... FROM addresses WHERE address = ?` → ❌ Dangerous
- Change to: `SELECT ... WHERE script_pubkey = ?`

**Category C - Spending paths (CRITICAL):**
- Any query in `sendtoaddress` flow
- Any query in UTXO selection
- **MUST use script_pubkey**

**Action:**
1. Grep for each flagged query
2. Trace call stack
3. Categorize A/B/C
4. Fix category B and C ONLY
5. Comment category A as "display only"

**Time-box:**
- Spend MAX 6 hours on this
- Fix only the obviously dangerous ones
- Document the rest for future cleanup

**Done criteria:**
- ✅ Spending paths use script_pubkey (no exceptions)
- ✅ Display queries have "display only" comments
- ✅ List of deferred cleanups documented
- ✅ All tests pass

---

## Phase 5: Final Validation & Tag
**Time:** 2 hours  
**Goal:** Verify refactor succeeded, tag, stop

### 5.1 Run Full Test Suite

```bash
./tests/wallet_tests/test_premine_invariants.sh
./tests/wallet_tests/test_seed_recovery_simulation.sh  
./tests/wallet_tests/test_negative_code_patterns.sh
./tests/wallet_tests/test_consensus_validation.sh
./tests/wallet_tests/test_taproot_scriptpubkey_spending.sh
```

**Required:** All tests pass (no exceptions)

### 5.2 Re-Run Test #3 (Negative Patterns)

```bash
./tests/wallet_tests/test_negative_code_patterns.sh
```

**Success criteria:**
- ✅ Fewer anti-pattern flags than before
- ✅ Remaining flags are documented as safe

### 5.3 Verify Naming Changes

```bash
# Should return NOTHING (or only RPC/display code)
grep -rn "getPrivateKeyForAddress\|getKeyForAddress" src/wallet/

# Should show new names
grep -rn "deriveKeyForScriptPubKey" src/wallet/
```

### 5.4 Git Commit & Tag

```bash
git add -A
git commit -m "Refactor: Make address-based ownership impossible

Structural changes (no behavior changes):
- Renamed address-taking functions → scriptPubKey-taking
- Separated display (address_encoding) from ownership (script_ownership)
- Fixed dangerous address comparisons in wallet.cpp
- Audited DB queries, fixed spending paths
- Added guardrail comments

All tests passing:
- Premine invariants: 19/19 ✅
- Seed recovery: 10/10 ✅  
- Negative patterns: improved ✅
- Consensus validation: 22/22 ✅
- Taproot spending: 14/14 ✅

Refactor complete. Consensus unchanged."

git tag -a refactor-ownership-v1 -m "Structural refactor: address-based ownership now impossible"
```

### 5.5 Document What Was Deferred

Create `DEFERRED_CLEANUP.md`:
```markdown
# Deferred Cleanup Items

These are safe to defer (not critical):

## Low Priority
- [ ] Some display-only DB queries could use better names
- [ ] HRP checks in encoding can be consolidated  
- [ ] Old test scaffolding can be removed

## Do NOT Touch (Working, Not Worth Risk)
- Legacy address validation in RPC (works, don't refactor)
- Old wallet migration code (deprecated but harmless)

Last updated: [DATE]
```

**Done criteria:**
- ✅ All tests pass
- ✅ Commit tagged
- ✅ Deferred items documented
- ✅ **STOP HERE** (no feature creep)

---

## 🚨 Abort Criteria

**If any of these happen, STOP and revert:**

1. ❌ Any test fails and you can't fix in 30 minutes
2. ❌ Refactor reveals a hidden consensus bug
3. ❌ More than 3 days elapsed
4. ❌ You're adding features instead of refactoring

**Revert command:**
```bash
git reset --hard [tag-before-refactor]
```

---

## 📊 Success Metrics

**Before:**
- Functions named `get*ForAddress`
- address comparisons in ownership code
- Mixed display/ownership logic
- Test #3: 3 dangerous patterns flagged

**After:**
- Functions named `deriveKey*ForScriptPubKey`
- No address comparisons in ownership code  
- Clear module boundaries (display vs ownership)
- Test #3: Fewer flags, remaining ones safe

**Most Important:**
- ✅ Future developers cannot accidentally reintroduce address-based ownership
- ✅ All tests still pass
- ✅ Consensus unchanged

---

## Order of Execution

1. **Day 1 Morning:** Phase 1 (renaming) - lowest risk
2. **Day 1 Afternoon:** Phase 2 (module separation) - medium risk
3. **Day 2 Morning:** Phase 3.1 (fix dangerous comparisons) - high risk
4. **Day 2 Afternoon:** Phase 3.2 (add guardrails) - low risk
5. **Day 3 Morning:** Phase 4 (DB audit, time-boxed) - medium risk
6. **Day 3 Afternoon:** Phase 5 (validate & tag) - low risk

**Total:** ~2.5 days if smooth, 3 days if issues

---

## Key Principles

1. **Tests guard everything** - run after each phase
2. **One thing at a time** - no feature creep
3. **Time-box ruthlessly** - Phase 4 gets max 6 hours
4. **Tag and stop** - resist urge to "just one more thing"
5. **Behavior unchanged** - only structure changes

This is **structural risk reduction**, not feature development.

---

**Ready to execute:** Yes  
**Abort plan ready:** Yes  
**Tests ready:** Yes  
**Time-box set:** Yes

Let's go. 🎯
