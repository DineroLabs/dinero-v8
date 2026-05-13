# Ring 4 Phase 4d: Safety Properties Implementation Plan

**Date:** 2026-01-02
**Status:** 📋 DESIGN (Implementation Plan)
**Purpose:** Implement MS1-MS5 safety property checkers using Phase 4b framework

---

## Executive Summary

Phase 4d implements the **safety oracle** that checks mining safety properties (MS1-MS5) against traces from Phase 4b framework.

**Key Principle:** Safety means "nothing bad happens" - no inflation, no duplicate subsidies, no invalid transactions.

**Deliverables:**
1. `MiningSafetyOracle` - Checks S1-S5 properties against traces
2. Restart inflation detector (for MS1)
3. Duplicate subsidy detector (for MS2)
4. Invalid transaction detector (for MS3)
5. Consensus bypass detector (for MS4)
6. Stale block detector (for MS5)
7. Safety property test suite (MS1-MS5 tests)

**Success Criteria:** All MS1-MS5 property tests pass using mock scenarios.

---

## 1. Scope & Non-Goals

### 1.1 What Phase 4d IS

✅ **Safety property checking**:
- MS1: No inflation under restart
- MS2: No duplicate subsidy across blocks
- MS3: No invalid transaction inclusion
- MS4: Consensus always enforced
- MS5: No stale block acceptance after reorg

✅ **Trace-based validation**:
- Analyze Phase 4b traces for safety violations
- Track subsidy claims across restarts
- Detect duplicate work
- Verify validation happened

### 1.2 What Phase 4d is NOT

❌ **No changes to Phase 4b framework**:
- Simulator remains dumb
- Generator remains neutral
- Trace structure unchanged

❌ **No production mining code changes**:
- `src/mining/*` untouched
- Real BlockAssembler not used yet
- Production refactoring deferred to Phase 4h

❌ **No liveness/correctness properties**:
- MC1-MC5 (correctness) already done in Phase 4c
- ML1-ML5 (liveness) deferred to Phase 4e
- Only safety (S1-S5) in Phase 4d

### 1.3 Why Safety Matters

**Safety guarantees:**
- No inflation creation (money doesn't appear from bugs)
- No duplicate rewards (restart doesn't double subsidy)
- No invalid states (malformed blocks never accepted)
- No shortcuts (validation always runs)
- No stale work (old templates discarded)

**These prevent:**
- Consensus failure
- Economic attacks
- Chain splits
- Silent corruption

---

## 2. MiningSafetyOracle Architecture

### 2.1 Oracle Interface

```cpp
namespace mining_test {

// ============================================================================
// MiningSafetyOracle - Validates S1-S5 properties
// ============================================================================

class MiningSafetyOracle {
public:
    explicit MiningSafetyOracle(const ConsensusParams& params);

    // MS1: No Inflation Under Restart
    struct InflationViolation {
        uint64_t restart_sequence;
        uint64_t subsidy_before_restart;
        uint64_t subsidy_after_restart;
        uint64_t excess_subsidy;  // How much inflation occurred
        std::string description;
    };
    std::vector<InflationViolation> checkNoInflationUnderRestart(const MiningTrace& trace);

    // MS2: No Duplicate Subsidy
    struct DuplicateSubsidyViolation {
        uint32_t height;
        uint64_t first_subsidy_claim;
        uint64_t second_subsidy_claim;
        std::string description;
    };
    std::vector<DuplicateSubsidyViolation> checkNoDuplicateSubsidy(const MiningTrace& trace);

    // MS3: No Invalid Transaction Inclusion
    struct InvalidTxViolation {
        uint64_t template_height;
        uint64_t invalid_tx_hash;  // Placeholder
        std::string invalidity_reason;
        std::string description;
    };
    std::vector<InvalidTxViolation> checkNoInvalidTransactions(const MiningTrace& trace);

    // MS4: Consensus Always Enforced
    struct ConsensusSkipViolation {
        uint64_t event_sequence;
        std::string skipped_validation;
        std::string condition;  // "restart", "reorg", "crash", etc.
        std::string description;
    };
    std::vector<ConsensusSkipViolation> checkConsensusAlwaysEnforced(const MiningTrace& trace);

    // MS5: No Stale Block Acceptance
    struct StaleBlockViolation {
        uint64_t block_hash;
        uint64_t stale_prev_hash;
        uint64_t current_tip;
        std::string description;
    };
    std::vector<StaleBlockViolation> checkNoStaleBlockAcceptance(const MiningTrace& trace);

    // Convenience: Check all safety properties
    struct SafetyReport {
        std::vector<InflationViolation> inflation_violations;
        std::vector<DuplicateSubsidyViolation> duplicate_subsidy_violations;
        std::vector<InvalidTxViolation> invalid_tx_violations;
        std::vector<ConsensusSkipViolation> consensus_skip_violations;
        std::vector<StaleBlockViolation> stale_block_violations;

        bool allPropertiesSatisfied() const {
            return inflation_violations.empty() &&
                   duplicate_subsidy_violations.empty() &&
                   invalid_tx_violations.empty() &&
                   consensus_skip_violations.empty() &&
                   stale_block_violations.empty();
        }

        size_t totalViolations() const {
            return inflation_violations.size() +
                   duplicate_subsidy_violations.size() +
                   invalid_tx_violations.size() +
                   consensus_skip_violations.size() +
                   stale_block_violations.size();
        }
    };
    SafetyReport checkAllSafetyProperties(const MiningTrace& trace);

    // Get consensus params
    const ConsensusParams& getParams() const { return params_; }

private:
    ConsensusParams params_;
    ConsensusSubsidyCalculator subsidy_calc_;
};

}  // namespace mining_test
```

### 2.2 Design Principles

**Principle 1: Track state across events**
- Safety violations often span multiple events
- Must track: restarts, subsidies claimed, blocks found
- State machine approach: track "expected" vs "actual"

**Principle 2: Placeholder validation for Phase 4d**
- Full UTXO tracking deferred to Phase 4h
- MS1-MS2: Track subsidy claims (feasible with placeholders)
- MS3-MS5: Basic consistency checks (full validation later)

**Principle 3: Conservative detection**
- Err on side of flagging potential violations
- False positives acceptable (will refine in Phase 4h)
- False negatives unacceptable (must catch real bugs)

---

## 3. Property Implementations

### 3.1 MS1: No Inflation Under Restart

**What it checks:**
```
∀ restart events:
  subsidy_claimed_after_restart <= subsidy_expected_from_consensus
  No extra subsidy appears from restart
```

**Implementation approach:**
1. Track all TEMPLATE_CREATED events and subsidy claims
2. Track CRASH and RESTART events
3. After each restart, verify next subsidy claim is correct
4. Detect if subsidy "duplicated" after restart

**Algorithm:**
```cpp
checkNoInflationUnderRestart(trace):
  total_subsidy_claimed = 0
  expected_subsidy = 0

  for each event in trace:
    if event == TEMPLATE_CREATED:
      total_subsidy_claimed += event.subsidy_claimed

    if event == SOLUTION_FOUND:
      expected_subsidy += consensus_subsidy(event.height)

  if total_subsidy_claimed > expected_subsidy:
    return INFLATION_VIOLATION
```

### 3.2 MS2: No Duplicate Subsidy

**What it checks:**
```
∀ heights H:
  count(subsidy_claimed_at_height[H]) <= 1
  Even across reorgs, same subsidy not claimed twice
```

**Implementation approach:**
1. Track subsidy claims by height
2. Detect multiple SOLUTION_FOUND at same height
3. Flag if subsidy claimed multiple times (even on different forks)

**Algorithm:**
```cpp
checkNoDuplicateSubsidy(trace):
  subsidy_by_height = map<height, list<subsidy>>

  for each event in trace:
    if event == SOLUTION_FOUND:
      height = event.template_height
      subsidy = event.subsidy_claimed

      if subsidy_by_height[height].contains(subsidy):
        return DUPLICATE_VIOLATION

      subsidy_by_height[height].add(subsidy)
```

### 3.3 MS3: No Invalid Transaction Inclusion

**What it checks:**
```
∀ templates T:
  all transactions in T are valid
  No double-spends, no invalid inputs
```

**Implementation approach (Placeholder):**
1. Phase 4d limitation: No real UTXOs in simulator
2. Check transaction count consistency
3. Check no obvious conflicts
4. Full UTXO validation deferred to Phase 4h

**Algorithm:**
```cpp
checkNoInvalidTransactions(trace):
  for each event in trace:
    if event == TEMPLATE_CREATED:
      // Placeholder: Check tx count is reasonable
      if event.tx_count > MAX_BLOCK_TXS:
        return INVALID_TX_VIOLATION

      // Full validation in Phase 4h
```

### 3.4 MS4: Consensus Always Enforced

**What it checks:**
```
∀ exceptional conditions (crash, restart, reorg):
  validation still runs
  No shortcuts taken
```

**Implementation approach:**
1. Track crash/restart events
2. Verify TEMPLATE_CREATED after restart
3. Ensure no templates created while crashed (would skip validation)

**Algorithm:**
```cpp
checkConsensusAlwaysEnforced(trace):
  is_crashed = false

  for each event in trace:
    if event == CRASH:
      is_crashed = true

    if event == RESTART:
      is_crashed = false

    if event == TEMPLATE_CREATED and is_crashed:
      return CONSENSUS_SKIP_VIOLATION  // Template while crashed!
```

### 3.5 MS5: No Stale Block Acceptance

**What it checks:**
```
∀ reorg events:
  old templates discarded
  Only blocks on current tip accepted
```

**Implementation approach:**
1. Track current chain tip
2. Track REORG events (tip changes)
3. Verify SOLUTION_FOUND blocks reference current tip
4. Flag if stale block submitted

**Algorithm:**
```cpp
checkNoStaleBlockAcceptance(trace):
  current_tip = genesis_hash

  for each event in trace:
    if event == REORG:
      current_tip = event.new_tip

    if event == SOLUTION_FOUND:
      if event.prev_hash != current_tip:
        return STALE_BLOCK_VIOLATION

      current_tip = event.block_hash  // Update tip
```

---

## 4. Implementation Order

### Step 1: MiningSafetyOracle Base Class
**Files:**
- `tests/mining/properties/mining_safety_oracle.h`
- `tests/mining/properties/mining_safety_oracle.cpp`

**Tasks:**
- Define oracle interface
- Implement violation structures
- Add checkAllSafetyProperties() aggregator

### Step 2: MS1 Property (No Inflation Under Restart)
**Files:**
- Implement checkNoInflationUnderRestart()
- Add subsidy tracking logic
- Test with restart scenarios

### Step 3: MS2 Property (No Duplicate Subsidy)
**Files:**
- Implement checkNoDuplicateSubsidy()
- Add height-based subsidy tracking
- Test with reorg scenarios

### Step 4: MS3-MS5 Properties (Placeholder)
**Files:**
- Implement checkNoInvalidTransactions() (basic)
- Implement checkConsensusAlwaysEnforced()
- Implement checkNoStaleBlockAcceptance()

### Step 5: Safety Property Tests
**Files:**
- `tests/mining/properties/test_mining_safety_oracle.cpp`

**Tasks:**
- Test MS1 with crash/restart scenarios
- Test MS2 with reorg scenarios
- Test MS3-MS5 with Phase 4b scenarios
- Integration test (all properties)

### Step 6: Update CMakeLists.txt
**Tasks:**
- Add safety oracle library
- Link against Phase 4b framework
- Register safety property tests

---

## 5. Test Coverage Matrix

| Property | Scenario | Expected Result |
|----------|----------|----------------|
| MS1 | Simple mining | No inflation |
| MS1 | Crash and restart | Subsidy preserved |
| MS1 | Multiple restarts | No cumulative inflation |
| MS2 | Single block | One subsidy |
| MS2 | Reorg with same height | No duplicate subsidy |
| MS2 | Multiple blocks | Each height has one subsidy |
| MS3 | Normal templates | No invalid tx |
| MS3 | After crash | Validation still runs |
| MS4 | All conditions | Consensus enforced |
| MS4 | While crashed | No bypass |
| MS5 | Reorg | Old template discarded |
| MS5 | Stale submission | Rejected |

---

## 6. Exit Criteria

### 6.1 Code Completeness

✅ All property checkers implemented:
- MS1: No inflation under restart ✓
- MS2: No duplicate subsidy ✓
- MS3: No invalid transactions (placeholder) ✓
- MS4: Consensus always enforced ✓
- MS5: No stale block acceptance ✓

✅ All tests pass:
- MS1 tests: Restart scenarios
- MS2 tests: Reorg scenarios
- MS3-MS5 tests: Basic validation
- Integration test: All properties together

✅ Oracle integrates with Phase 4b:
- Uses MiningTrace from simulator
- No modifications to simulator
- Clean separation maintained

### 6.2 Documentation

✅ Properties documented:
- What each property prevents
- Placeholder vs full validation
- Future Phase 4h integration

✅ Test coverage documented:
- Which scenarios tested
- Known limitations
- False positive/negative risks

### 6.3 Quality Gates

✅ Builds cleanly (no warnings)
✅ All safety tests pass
✅ No false negatives (catches real violations)
✅ Ready for Phase 4e (liveness properties)

---

## 7. Relationship to Other Phases

### 7.1 Phase 4b Foundation

**Phase 4d depends on:**
- MiningTrace with restart/crash/reorg events
- MiningState snapshots
- MiningEvent log (TEMPLATE_CREATED, SOLUTION_FOUND, etc.)

**Phase 4d does NOT modify:**
- Simulator logic
- Generator logic
- Framework self-tests

### 7.2 Phase 4c Correctness

**Phase 4c provided:**
- ConsensusSubsidyCalculator (reused for MS1/MS2)
- ConsensusParams (reused)
- Property oracle pattern

**Phase 4d extends:**
- Same oracle pattern
- Different property domain (safety vs correctness)

### 7.3 Future Phases

**Phase 4e will add:** ML1-ML5 liveness properties
**Phase 4f will add:** MD1-MD5 determinism properties
**Phase 4g will add:** MR1-MR5 restart properties
**Phase 4h will upgrade:** MS3 to full UTXO validation

---

## 8. Known Limitations

### 8.1 Placeholder Validation

**MS3-MS5 are partially implemented:**
- MS3: Count checks only (no real UTXO validation)
- MS4: Basic crash detection (no deep validation bypass detection)
- MS5: Tip tracking only (no fork validation)

**Rationale:** Phase 4b simulator uses placeholders, full validation requires Phase 4h.

### 8.2 No Real Blocks

**Oracle validates traces, not blocks:**
- MS2 tracks claimed subsidies, not actual outputs
- MS3 cannot validate real transaction scripts
- MS5 tracks events, not actual block connections

**This is intentional:** Phase 4d proves oracle works before integrating real mining.

### 8.3 Conservative Detection

**Some false positives expected:**
- MS1 might flag legitimate subsidy patterns
- MS2 might misidentify reorg behavior
- Will be refined in Phase 4h

---

## 9. Summary

**Phase 4d Goal:** Implement safety oracle (MS1-MS5)

**Deliverables:**
1. MiningSafetyOracle ✓
2. MS1: No inflation (full tracking) ✓
3. MS2: No duplicate subsidy (full tracking) ✓
4. MS3-MS5: Placeholder validation ✓
5. Safety property test suite ✓

**Exit Criteria:** All MS* tests pass

**Next Phase:** Phase 4e - Liveness Properties (ML1-ML5)

**Status:** Design complete, ready for implementation

---

## Document Metadata

- **Created:** 2026-01-02
- **Author:** Claude Sonnet 4.5 (via Claude Code)
- **Purpose:** Implementation plan for Ring 4 Phase 4d (safety properties)
- **Status:** Design complete, awaiting implementation

**Next Action:** Begin implementation with MiningSafetyOracle base class
