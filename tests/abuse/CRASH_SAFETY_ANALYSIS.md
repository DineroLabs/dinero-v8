# Crash Safety Analysis: AssumeUTXO Implementation

**Purpose:** Map all state mutations and identify crash boundaries that could leave partial/corrupt state.

**Critical Invariant:** Node must be safe after SIGKILL at ANY instruction boundary.

---

## State Mutation Points in LoadSnapshot()

### Current Implementation Analysis

**File:** `src/daemon/services/chainstate_service.cpp:1732` (LoadSnapshot)

**State mutation timeline:**

```
┌─────────────────────────────────────────────────────────┐
│ Phase 1: Header Read (lines 1760-1836)                 │
│ ────────────────────────────────────────────────────    │
│ State: IN-MEMORY ONLY                                   │
│ Crash safety: ✅ SAFE (no disk writes)                 │
└─────────────────────────────────────────────────────────┘
          ↓
┌─────────────────────────────────────────────────────────┐
│ Phase 2: UTXO Read + Checksum (lines 1858-1926)        │
│ ────────────────────────────────────────────────────    │
│ State: std::vector<UTXO> utxos (in memory)              │
│ Crash safety: ✅ SAFE (no disk writes)                 │
│                                                          │
│ CRITICAL: Checksum verified BEFORE any persistence      │
└─────────────────────────────────────────────────────────┘
          ↓
┌─────────────────────────────────────────────────────────┐
│ Phase 3: UTXO Import Loop (lines 1931-1943)            │
│ ────────────────────────────────────────────────────    │
│ for (const auto& utxo : utxos) {                        │
│     utxo_index_->AddUTXO(utxo);  ← DISK WRITE?         │
│     result.utxos_imported++;                            │
│ }                                                        │
│                                                          │
│ Crash safety: ⚠️  UNKNOWN - depends on UTXOIndex impl  │
│                                                          │
│ CRITICAL QUESTIONS:                                     │
│ 1. Does AddUTXO() write to disk immediately?            │
│ 2. Is there a flush/commit step after the loop?         │
│ 3. Is UTXOIndex transactional?                          │
│                                                          │
│ If AddUTXO() writes immediately:                        │
│   → Crash mid-loop = partial UTXO set on disk          │
│   → assumeutxo_active_ not set yet                     │
│   → Restart sees: UTXO set populated but flag=false    │
│   → DANGEROUS: Inconsistent state                      │
└─────────────────────────────────────────────────────────┘
          ↓
┌─────────────────────────────────────────────────────────┐
│ Phase 4: Assume-Valid Flag Set (lines 1945-1949)       │
│ ────────────────────────────────────────────────────    │
│ assumeutxo_active_ = true;                              │
│ assumeutxo_base_block_ = header.block_hash;             │
│ assumeutxo_base_height_ = header.block_height;          │
│                                                          │
│ Crash safety: ⚠️  CRITICAL                              │
│                                                          │
│ QUESTION: Are these flags persisted to disk?            │
│ If YES and crash before background validation starts:  │
│   → Flag set, but validation never started             │
│   → Node thinks snapshot loaded but validation stuck   │
│                                                          │
│ If NO (in-memory only):                                 │
│   → Restart loses assumeutxo_active_ = true            │
│   → Node has full UTXO set but doesn't know it's       │
│      from snapshot                                      │
│   → No background validation triggered                 │
│   → DANGEROUS: Unvalidated snapshot assumed valid      │
└─────────────────────────────────────────────────────────┘
          ↓
┌─────────────────────────────────────────────────────────┐
│ Phase 5: Background Validation Start (line 1958)       │
│ ────────────────────────────────────────────────────    │
│ StartBackgroundValidation();                            │
│                                                          │
│ Crash safety: ⚠️  CRITICAL                              │
│                                                          │
│ QUESTION: Is bg_validation_status_ persisted?           │
│ If crash during thread creation:                        │
│   → Thread may or may not be running                   │
│   → Status flag may be inconsistent                    │
│   → Restart behavior undefined                         │
└─────────────────────────────────────────────────────────┘
```

---

## Critical Questions (MUST ANSWER)

### Q1: UTXOIndex Persistence Model

**Where to check:** `wallet/utxo_index.h` and `wallet/utxo_index.cpp`

**Questions:**
1. Does `AddUTXO()` write to disk immediately (write-through)?
2. Or does it buffer in memory (write-back)?
3. Is there a `Flush()` or `Commit()` method?
4. Is the index transactional (begin/commit/rollback)?

**Required behavior:**
- **Option A (Safe):** All UTXOs buffered in memory, explicit Flush() after loop
- **Option B (Unsafe):** Immediate disk writes → need transaction wrapper
- **Option C (Best):** Transactional interface with rollback support

### Q2: AssumeUTXO State Persistence

**Flags in question:**
```cpp
bool assumeutxo_active_ = false;
uint256 assumeutxo_base_block_;
uint32_t assumeutxo_base_height_ = 0;
```

**Questions:**
1. Are these persisted to disk?
2. If yes, when? (immediately on assignment, or during shutdown?)
3. If no, how does node know snapshot is loaded after restart?

**Required behavior:**
- Either persist atomically with UTXO import
- Or reconstruct from UTXO set on restart
- **Never**: UTXO set loaded but flags not set

### Q3: Background Validation State Persistence

**Flags in question:**
```cpp
BackgroundValidationStatus bg_validation_status_;
uint32_t bg_validation_current_height_;
uint32_t bg_validation_blocks_validated_;
std::string bg_validation_error_;
```

**Questions:**
1. Are these persisted to disk?
2. Can validation resume after restart?
3. What happens if thread dies mid-validation?

**Required behavior:**
- Validation progress persisted (for resumption)
- Or validation restarts from beginning
- **Never**: Stuck "in progress" state after restart

---

## Crash Test Matrix

### Snapshot Import Crashes

| Boundary | State Before Crash | Expected After Restart | Status |
|----------|-------------------|------------------------|--------|
| 1. Before header read | Clean state | Clean state | ✅ Trivial |
| 2. After header read | Header in memory | Clean state | ✅ Expected |
| 3. Mid UTXO pass-1 | Partial vector | Clean state | ✅ Expected |
| 4. After checksum verify | Vector complete | Clean state | ✅ Expected |
| 5. Mid UTXO import (50%) | ??? | ??? | 🔴 **CRITICAL** |
| 6. After UTXO import | UTXOs on disk? | ??? | 🔴 **CRITICAL** |
| 7. After assumeutxo_active_ set | Flags set? | ??? | 🔴 **CRITICAL** |
| 8. During BG validation start | Thread starting | ??? | 🔴 **CRITICAL** |

### Background Validation Crashes

| Boundary | State Before Crash | Expected After Restart | Status |
|----------|-------------------|------------------------|--------|
| 1. During block iteration | Mid-validation | Resume or restart | 🔴 **CRITICAL** |
| 2. During UTXO comparison | Comparing sets | Resume or restart | 🔴 **CRITICAL** |
| 3. On validation success | Complete | assumeutxo_active_ cleared | 🔴 **CRITICAL** |
| 4. On validation failure | Mismatch detected | Safe mode OR rollback | 🔴 **CRITICAL** |

---

## Invariants to Prove

### Invariant 1: No Partial UTXO Sets
**Rule:** UTXO index is either empty OR contains complete snapshot.

**Test:** Kill during UTXO import loop, restart, check UTXO count.
- **Pass:** UTXO count = 0 (rolled back)
- **Fail:** UTXO count > 0 and < expected (partial import)

### Invariant 2: Flag Consistency
**Rule:** If `assumeutxo_active_ = true`, UTXO set must be complete snapshot.

**Test:** Kill after flag set but before validation starts, restart.
- **Pass:** Flag and UTXO set both present OR both absent
- **Fail:** Flag set but UTXO set incomplete (or vice versa)

### Invariant 3: No Stuck Validation
**Rule:** Background validation either running, complete, or not started.

**Test:** Kill during background validation, restart.
- **Pass:** Validation restarts or resumes cleanly
- **Fail:** Status stuck "in progress" with no thread running

### Invariant 4: Restart Idempotency
**Rule:** Restart always produces same result for same on-disk state.

**Test:** Kill at same boundary twice, restart both times.
- **Pass:** Same final state both times
- **Fail:** Different states (nondeterministic)

---

## Action Items (Before Writing Tests)

### IMMEDIATE: Code Inspection

1. **Read UTXOIndex implementation:**
   - File: `wallet/utxo_index.cpp`
   - Question: When do AddUTXO() writes hit disk?
   - Question: Is there a Flush() method?

2. **Search for state persistence:**
   ```bash
   grep -r "assumeutxo_active" --include="*.cpp"
   grep -r "bg_validation_status" --include="*.cpp"
   ```
   - Question: Where are these flags saved/loaded?

3. **Check restart/initialization code:**
   - File: `daemon_app.cpp` or `chainstate_service.cpp`
   - Question: How does node detect snapshot on restart?

### AFTER Code Inspection: Write Tests

Based on findings, write crash tests that:
1. Kill at specific instruction boundaries (using debugger or instrumentation)
2. Restart node
3. Verify invariants
4. Document actual behavior

---

## Expected Findings

### Best Case (Current Code is Safe)
- UTXOIndex buffers in memory, explicit Flush() after loop
- Flags persisted atomically with Flush()
- Background validation resumable from persisted state

### Likely Case (Needs Fixes)
- UTXOIndex writes immediately (no transaction support)
- Flags not persisted (lost on restart)
- Background validation restarts from beginning

### Worst Case (Major Refactor Needed)
- Partial UTXO writes possible
- Flag inconsistency possible
- No crash recovery at all

---

## Next Steps

1. ✅ Document crash boundaries (this file)
2. 🔄 Inspect UTXOIndex implementation
3. 🔄 Inspect state persistence
4. 🔄 Write crash test script with instrumentation
5. 🔄 Run tests and document actual behavior
6. 🔄 Fix any bugs found
7. 🔄 Re-test until all invariants proven

**Status:** Analysis complete, code inspection required before testing.
