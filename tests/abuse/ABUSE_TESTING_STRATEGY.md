# AssumeUTXO Abuse Testing Strategy

**Purpose:** Verify that the node never corrupts consensus state, no matter what happens.

**Philosophy:** If Bitcoin Core can survive it, we must survive it. If we can't, we have a bug.

---

## Critical Invariants (MUST NEVER VIOLATE)

1. **UTXO Set Integrity**
   - UTXO set must always be internally consistent
   - No double-spends after crash
   - No missing UTXOs after crash
   - Checksum must match UTXO contents

2. **Chain State Integrity**
   - Active chain tip must be valid
   - No orphan blocks in active chain
   - Block index must be consistent with ChainDB
   - No corrupted block data

3. **Snapshot State Integrity**
   - If `assumeutxo_active` = true, base block must exist
   - Background validation state must be recoverable
   - Snapshot metadata must match loaded UTXO set
   - Checksum verification must prevent bad snapshots

4. **Pruning State Integrity**
   - Never prune blocks needed for reorg
   - Pruning state must be crash-recoverable
   - Can't prune if validation incomplete
   - Can't prune if snapshot not loaded

---

## Test Categories

### Category 1: Crash During Critical Operations

**Goal:** Verify crash-safety of all state-changing operations.

**Tests:**
1. Kill during snapshot import (various points)
2. Kill during background validation (various blocks)
3. Kill during block acceptance
4. Kill during reorg execution
5. Kill during pruning operation
6. Kill during snapshot export

**Expected Behavior:**
- Node restarts without corruption
- Either operation completes or rolls back cleanly
- No partial state persisted
- No UTXO set corruption

### Category 2: Data Corruption

**Goal:** Verify that corrupted data is detected and rejected.

**Tests:**
1. Corrupted snapshot file (bad checksum)
2. Corrupted snapshot file (wrong format version)
3. Corrupted snapshot file (truncated data)
4. Corrupted snapshot file (wrong block hash)
5. Snapshot from wrong chain (testnet on mainnet)
6. Snapshot with missing UTXOs
7. Snapshot with duplicate UTXOs

**Expected Behavior:**
- Corruption detected before state change
- Clear error message to operator
- Node falls back to safe state
- No consensus corruption

### Category 3: Attack Scenarios

**Goal:** Verify that malicious inputs can't corrupt state.

**Tests:**
1. Deep reorg during AssumeUTXO (> 1000 blocks)
2. Conflicting snapshot + blockchain data
3. Snapshot at height X, blockchain at height Y < X
4. Multiple snapshot loads (overwrite protection)
5. Background validation fails (bad snapshot detection)
6. Pruning beyond safe limit attempts

**Expected Behavior:**
- Safe mode activation for deep reorg
- Background validation detects conflicts
- Clear operator warnings
- Automatic fallback to safe state

### Category 4: Resource Exhaustion

**Goal:** Verify graceful degradation under resource limits.

**Tests:**
1. Disk full during snapshot import
2. Disk full during block sync
3. Disk full during background validation
4. Out of memory during snapshot load
5. Very large snapshot file (> available RAM)

**Expected Behavior:**
- Operation fails with clear error
- No partial state committed
- Node remains operational
- Operator gets actionable error message

### Category 5: Edge Cases

**Goal:** Verify correct behavior in unusual scenarios.

**Tests:**
1. Load snapshot at genesis
2. Load snapshot at current tip
3. Background validation at 100% (should complete immediately)
4. Prune with zero blocks available
5. Reorg to shorter chain (not just longer)
6. Snapshot older than pruned data

**Expected Behavior:**
- Sensible behavior in all cases
- No crashes or hangs
- Correct state transitions

---

## Test Execution Strategy

### Phase 1: Automated Tests (This Phase)
- Create test scripts for each scenario
- Run in isolated environment
- Verify invariants after each test
- Document pass/fail for each test

### Phase 2: Manual Testing (Next)
- Operator-driven scenarios
- Interactive testing with real node
- Document operational procedures

### Phase 3: Fuzzing (Future)
- Random snapshot generation
- Random crash points
- Random input corruption
- Run for days/weeks

---

## Success Criteria

**Minimum Bar (Required for Production):**
- ✅ All Category 1 tests pass (crash safety)
- ✅ All Category 2 tests pass (corruption detection)
- ✅ All Category 3 tests pass (attack resistance)
- ✅ 80%+ of Category 4 tests pass (resource limits)
- ✅ 80%+ of Category 5 tests pass (edge cases)

**Gold Standard (Bitcoin Core Level):**
- ✅ All tests in all categories pass
- ✅ Fuzzing runs for 1 week without failure
- ✅ Manual testing by external operators
- ✅ All critical paths documented

---

## Documentation Requirements

For each test:
1. Test ID
2. Scenario description
3. Expected behavior
4. Actual behavior
5. Pass/Fail status
6. Issues found (if any)
7. Fixes applied (if any)

**Output Files:**
- `ABUSE_TEST_RESULTS.md` - Test run results
- `SNAPSHOT_SECURITY.md` - Security model documentation
- `CRASH_SAFETY.md` - Crash recovery guarantees
- `OPERATOR_GUIDE.md` - Operational procedures

---

## Current Status

**Date:** 2024-12-24
**Phase:** Design Complete
**Next Step:** Implement test scripts for Category 1 (crash safety)

