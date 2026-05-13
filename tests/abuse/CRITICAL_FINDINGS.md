# Critical Findings from Abuse Testing

**Document Purpose:** Track critical bugs found during abuse testing that could corrupt consensus state.

**Status:** 🔴 CRITICAL ISSUES FOUND

---

## CRITICAL-001: Checksum Verified AFTER UTXO Import

**Date:** 2024-12-24
**Severity:** 🔴 CRITICAL
**File:** `src/daemon/services/chainstate_service.cpp:1732` (LoadSnapshot)
**Status:** 🔴 OPEN

### The Bug

LoadSnapshot() adds UTXOs to the UTXO index **BEFORE** verifying the checksum.

**Problematic code flow:**
```cpp
// Lines 1845-1888: Import UTXOs into UTXO index
for (uint64_t i = 0; i < header.utxo_count; ++i) {
    // ... read UTXO data ...
    utxo_index_->AddUTXO(utxo);  // ❌ ADDED BEFORE CHECKSUM VERIFICATION
    result.utxos_imported++;
}

// Lines 1890-1906: THEN verify checksum
uint8_t stored_checksum[32];
file.read(reinterpret_cast<char*>(stored_checksum), 32);
sha256.Finalize(computed_checksum);

if (std::memcmp(computed_checksum, stored_checksum, 32) != 0) {
    result.error_message = "Snapshot checksum mismatch";
    return result;  // ❌ BUT UTXO INDEX ALREADY CORRUPTED!
}
```

### The Attack

1. Attacker crafts snapshot with valid header
2. Adds N-1 valid UTXOs
3. Adds 1 malicious UTXO (e.g., creates coins out of thin air)
4. Checksum will fail...
5. **...but all N UTXOs are already in the UTXO index!**

Result: **Consensus corruption**. Node has invalid UTXO set.

### Why This Matters

Bitcoin Core principle: **Verify THEN trust, never trust THEN verify.**

Current code violates this by modifying state before verification completes.

### The Fix

**Option 1: Two-pass import (safest)**
```cpp
// Pass 1: Read entire file and verify checksum
crypto::CSHA256 sha256;
std::vector<SerializedUTXO> utxos;

// Read all data into memory, computing checksum
for (uint64_t i = 0; i < header.utxo_count; ++i) {
    // Read UTXO, update checksum, store in vector
    utxos.push_back(ReadUTXO(file, sha256));
}

// Verify checksum BEFORE touching UTXO index
uint8_t stored_checksum[32];
file.read(reinterpret_cast<char*>(stored_checksum), 32);
sha256.Finalize(computed_checksum);

if (std::memcmp(computed_checksum, stored_checksum, 32) != 0) {
    return result;  // ✓ UTXO index untouched
}

// Pass 2: Only now add UTXOs (checksum already verified)
for (const auto& utxo : utxos) {
    utxo_index_->AddUTXO(utxo);
}
```

**Option 2: Transactional import (if supported)**
```cpp
// Begin transaction (if UTXO index supports it)
utxo_index_->BeginTransaction();

// Import UTXOs
for (...) {
    utxo_index_->AddUTXO(utxo);
}

// Verify checksum
if (checksum_invalid) {
    utxo_index_->RollbackTransaction();  // ✓ Atomic rollback
    return result;
}

utxo_index_->CommitTransaction();  // ✓ Atomic commit
```

**Option 3: Clear on failure (least safe)**
```cpp
// Import UTXOs
for (...) {
    utxo_index_->AddUTXO(utxo);
}

// Verify checksum
if (checksum_invalid) {
    utxo_index_->Clear();  // ⚠️ Clears ALL UTXOs, not just imported ones
    return result;
}
```

### Recommended Solution

**Use Option 1 (two-pass import).**

Pros:
- Simple to implement
- No UTXO index API changes needed
- Guaranteed safe
- Small memory cost (snapshot UTXOs in RAM briefly)

Cons:
- Requires holding UTXOs in memory during import
- For very large snapshots (millions of UTXOs), could use ~GB of RAM

**Memory estimate:**
- Average UTXO: ~100 bytes
- 10M UTXOs: ~1GB RAM
- Acceptable for modern systems

### Impact Assessment

**Current risk:**
- ✅ Mitigated by `assumeutxo_active_` = true precondition (UTXO set must be empty)
- ✅ Mitigated by background validation (bad snapshot will be detected later)
- ❌ Still dangerous: Window between import and validation where consensus is wrong

**Attack scenario:**
1. Node starts fresh (empty UTXO set)
2. Loads bad snapshot with checksum mismatch
3. Checksum fails, but UTXOs already added
4. Node thinks it failed, but UTXO set is corrupted
5. Node continues operating with invalid UTXOs until restart

**Severity:** CRITICAL because it can lead to consensus fork if not detected.

### Action Items

- [ ] Implement two-pass import in LoadSnapshot()
- [ ] Add test case: snapshot with valid header, invalid checksum, malicious UTXO
- [ ] Verify UTXO set unchanged after failed import
- [ ] Add UTXO count verification (compare imported vs header.utxo_count)
- [ ] Document checksum verification guarantees

---

## Testing Plan to Verify Fix

### Test Case: Bad Checksum with Malicious UTXO

```bash
#!/bin/bash
# Create snapshot with:
# - Valid header
# - 1 valid UTXO
# - 1 malicious UTXO (creates 1000 BTC from nowhere)
# - Invalid checksum

# Expected BEFORE fix:
#   - LoadSnapshot returns error
#   - BUT both UTXOs are in UTXO index
#   - Node has 1000 BTC that shouldn't exist

# Expected AFTER fix:
#   - LoadSnapshot returns error
#   - UTXO index unchanged (still empty)
#   - No consensus corruption
```

### Verification Commands

```bash
# Before importing bad snapshot
dinero-cli getblockcount
> 0

# Attempt to load bad snapshot
dinero-cli loadtxoutset bad_snapshot.dat
> Error: Snapshot checksum mismatch

# CRITICAL CHECK: UTXO count should still be zero
dinero-cli gettxoutsetinfo
> { "txouts": 0, ... }  # ✓ PASS if 0, ✗ FAIL if > 0

# If UTXO count > 0 after failed import → BUG CONFIRMED
```

---

---

## CRITICAL-002: No Transaction Wrapper for UTXO Import

**Date:** 2024-12-24
**Severity:** 🔴 CRITICAL
**File:** `src/daemon/services/chainstate_service.cpp:1931` (LoadSnapshot - Pass 2)
**Status:** 🔴 OPEN

### The Bug

LoadSnapshot() Pass 2 calls `AddUTXO()` in a loop WITHOUT wrapping in a transaction.

**Problematic code (lines 1931-1943):**
```cpp
// Pass 2: Now that checksum is verified, add UTXOs to index
for (const auto& utxo : utxos) {
    utxo_index_->AddUTXO(utxo);  // ❌ Each call commits immediately!
    result.utxos_imported++;
}
```

**UTXOIndex behavior:**
- `AddUTXO()` uses SQLite in autocommit mode
- Each call writes to disk immediately
- No transaction wrapper = no atomicity

### The Problem

**Crash at any point during the loop:**
1. First N UTXOs committed to disk ✓
2. Crash at UTXO N+1
3. Restart sees:
   - UTXO count = N (partial import)
   - `assumeutxo_active_` = false (never reached line 1947)
   - No indication these UTXOs are from a snapshot

**Result:** Node has partial UTXO set with no snapshot metadata. Inconsistent state.

### Attack Scenario (Less Severe than CRITICAL-001)

This is primarily an **operational bug**, not an attack vector:
- Operator kills node during snapshot import (SIGKILL, power loss, etc.)
- Node restarts with partial UTXO set
- No clear indication of corruption
- May cause mysterious balance/UTXO issues

### Why This Violates Bitcoin Core Principles

**Atomicity requirement:**
> "State transitions must be atomic. Either fully committed or fully rolled back."

**Current code:**
- CRITICAL-001 fix ensures checksum verified first ✅
- But import still not atomic ❌
- Crash during import = partial state

### The Fix

**Add transaction wrapper to UTXOIndex:**

```cpp
// wallet/utxo_index.h
class UTXOIndex {
public:
    // Add transaction control methods
    bool BeginTransaction();
    bool CommitTransaction();
    bool RollbackTransaction();
    //...
};
```

**Use in LoadSnapshot:**

```cpp
// Pass 2: Transactional import for atomicity
if (!utxo_index_->BeginTransaction()) {
    result.error_message = "Failed to begin transaction";
    return result;
}

for (const auto& utxo : utxos) {
    if (!utxo_index_->AddUTXO(utxo)) {
        utxo_index_->RollbackTransaction();  // ✓ Rollback on error
        result.error_message = "Failed to add UTXO";
        return result;
    }
    result.utxos_imported++;
}

if (!utxo_index_->CommitTransaction()) {  // ✓ Atomic commit
    utxo_index_->RollbackTransaction();
    result.error_message = "Failed to commit transaction";
    return result;
}
```

**Crash safety:**
- Crash before commit → SQLite rolls back automatically ✓
- Crash after commit → All UTXOs persisted atomically ✓
- No partial state possible ✓

### Implementation Plan

1. Add BeginTransaction/Commit/Rollback to UTXOIndex public API
2. Implement using existing SQLite transaction code (already used internally)
3. Wrap LoadSnapshot Pass 2 in transaction
4. Test crash at every point in import loop
5. Verify UTXO count is 0 OR full snapshot (never partial)

### Related Issues

- CRITICAL-001: Checksum verified after import (FIXED)
- This issue: Import not atomic (OPEN)

Together, these two bugs made snapshot loading completely unsafe. CRITICAL-001 prevented checksum-based attacks. CRITICAL-002 prevents crash-based corruption.

### Status

**Severity:** CRITICAL (consensus integrity)
**Urgency:** HIGH (found during crash safety analysis)
**Effort:** LOW (transaction API already exists internally, just needs exposure)
**Status:** ✅ FIXED

**Fix Details:**
- Added BeginTransaction/CommitTransaction/RollbackTransaction to UTXOIndex
- Wrapped LoadSnapshot Pass 2 in transaction
- Crash before commit → SQLite rolls back automatically
- Crash after commit → All UTXOs persisted atomically
- No partial state possible

---

## Status Summary

| Finding | Severity | Status | Action |
|---------|----------|--------|--------|
| CRITICAL-001 | 🔴 CRITICAL | ✅ FIXED | Checksum verified before import |
| CRITICAL-002 | 🔴 CRITICAL | ✅ FIXED | Transaction wrapper added |
| CRITICAL-003 | 🔴 CRITICAL | ✅ FIXED | Metadata persisted atomically |

---

---

## CRITICAL-003: AssumeUTXO Flags Not Persisted Atomically

**Date:** 2024-12-24
**Severity:** 🔴 CRITICAL
**File:** `src/daemon/services/chainstate_service.cpp:1969` (LoadSnapshot - after commit)
**Status:** 🔴 OPEN

### The Bug

AssumeUTXO state flags are set in memory AFTER CommitTransaction() completes.

**Problematic code flow:**
```cpp
// Line 1959: Commit transaction
if (!utxo_index_->CommitTransaction()) {  // ← UTXOs persisted to disk
    // ...
}

// Line 1967: Log success
logger_->info("[LoadSnapshot] Pass 2 complete...");

// Line 1971: Set flags (IN MEMORY ONLY)
assumeutxo_active_ = true;              // ❌ Not persisted!
assumeutxo_base_block_ = header.block_hash;  // ❌ Not persisted!
assumeutxo_base_height_ = header.block_height;  // ❌ Not persisted!
```

### The Problem

**Crash window between commit and flag set:**
1. CommitTransaction() completes → all UTXOs on disk ✓
2. **CRASH HERE** (power loss, SIGKILL, etc.)
3. Restart sees:
   - UTXO set populated (full snapshot)
   - `assumeutxo_active_` = false (default)
   - No indication these UTXOs are from snapshot
4. Background validation NEVER starts
5. Node operates with **unvalidated snapshot forever**

**This is catastrophic** - violates the entire AssumeUTXO security model.

### Why This Violates Bitcoin Core Principles

**AssumeUTXO safety requirement:**
> "Snapshot must ALWAYS trigger background validation. Unvalidated snapshots are unsafe."

**Current code:**
- ✅ Snapshot loaded
- ✅ UTXOs persisted
- ❌ Flags not persisted
- ❌ Background validation not resumed on restart

Result: **Permanently unvalidated state.**

### The Fix

**Persist metadata atomically with UTXO commit:**

```cpp
// Add metadata storage to UTXOIndex
class UTXOIndex {
public:
    bool SetMetadata(const std::string& key, const std::string& value);
    std::optional<std::string> GetMetadata(const std::string& key) const;
};
```

**Use in LoadSnapshot:**

```cpp
// Pass 2: Atomic import with metadata
if (!utxo_index_->BeginTransaction()) {
    return result;
}

// Add UTXOs
for (const auto& utxo : utxos) {
    utxo_index_->AddUTXO(utxo);
    result.utxos_imported++;
}

// CRITICAL: Store metadata in SAME transaction
utxo_index_->SetMetadata("assumeutxo_active", "true");
utxo_index_->SetMetadata("assumeutxo_base_block", header.block_hash.GetHex());
utxo_index_->SetMetadata("assumeutxo_base_height", std::to_string(header.block_height));

// Atomic commit: UTXOs + metadata together
if (!utxo_index_->CommitTransaction()) {
    utxo_index_->RollbackTransaction();
    return result;
}

// Load metadata into memory (already persisted)
assumeutxo_active_ = true;
assumeutxo_base_block_ = header.block_hash;
assumeutxo_base_height_ = header.block_height;
```

**On restart, load metadata:**

```cpp
// In ChainstateService::Start()
auto active = utxo_index_->GetMetadata("assumeutxo_active");
if (active && active.value() == "true") {
    assumeutxo_active_ = true;

    auto block_hash = utxo_index_->GetMetadata("assumeutxo_base_block");
    assumeutxo_base_block_ = uint256::FromHex(block_hash.value());

    auto height = utxo_index_->GetMetadata("assumeutxo_base_height");
    assumeutxo_base_height_ = std::stoul(height.value());

    // Resume background validation if not complete
    StartBackgroundValidation();
}
```

### Crash Safety

**Before fix:**
```
CommitTransaction()  ← UTXOs persisted
  CRASH HERE → flags lost!
Set flags (memory)
```

**After fix:**
```
BeginTransaction()
  Add UTXOs
  SetMetadata("assumeutxo_active", "true")  ← Flags in transaction
  SetMetadata("base_block", ...)
CommitTransaction()  ← Atomic: UTXOs + flags
  CRASH AFTER → both persisted ✓
```

### Implementation Plan

1. Add metadata table to UTXOIndex schema
2. Implement SetMetadata/GetMetadata
3. Store AssumeUTXO flags in transaction
4. Load flags on restart
5. Resume background validation if needed
6. Test crash at Point 12 (after commit)

### Related Issues

- CRITICAL-001: Checksum after import (FIXED)
- CRITICAL-002: Import not atomic (FIXED)
- **CRITICAL-003: Flags not persisted (OPEN)**

Together, these three bugs made AssumeUTXO completely unsafe:
1. Bad snapshot could corrupt state (001)
2. Crash could create partial state (002)
3. Crash could skip validation forever (003)

### Status

**Severity:** CRITICAL (security model violation)
**Urgency:** HIGH (found during crash test design)
**Effort:** MEDIUM (need metadata storage)
**Impact:** Catastrophic if not fixed (unvalidated state persists)
**Status:** ✅ FIXED

**Fix Details:**
- Added utxo_metadata table to UTXOIndex schema
- Implemented SetMetadata/GetMetadata/DeleteMetadata methods
- Store AssumeUTXO flags in SAME transaction as UTXOs (atomic commit)
- Restore flags from metadata on restart
- Resume background validation if incomplete
- No window where UTXOs exist without metadata

---

**All Critical Bugs Fixed:**
1. ✅ Fix CRITICAL-001 (checksum bug) - DONE
2. ✅ Fix CRITICAL-002 (transaction atomicity) - DONE
3. ✅ Fix CRITICAL-003 (metadata persistence) - DONE

**Next Steps:**
1. Implement crash test script with SIGKILL instrumentation
2. Test crash at all 14 boundaries identified in CRASH_TEST_INSTRUMENTATION.md
3. Verify UTXO count invariant: 0 OR full snapshot (never partial)
4. Test background validation crash safety
5. Document proven crash safety invariants
