# Ring 4 Phase 4e: Liveness Properties Implementation Plan

**Date:** 2026-01-02
**Status:** 📋 DESIGN (Implementation Plan)
**Purpose:** Implement ML1-ML5 liveness property checkers using Phase 4b framework

---

## Executive Summary

Phase 4e implements the **liveness oracle** that checks mining liveness properties (ML1-ML5) against traces from Phase 4b framework.

**Key Principle:** Liveness means "something good eventually happens" - templates created, solutions found, blocks submitted.

**Deliverables:**
1. `MiningLivenessOracle` - Checks ML1-ML5 properties against traces
2. Template creation liveness detector (for ML1)
3. Solution finding liveness detector (for ML2)
4. Block submission liveness detector (for ML3)
5. Restart recovery liveness detector (for ML4)
6. Template freshness liveness detector (for ML5)
7. Liveness property test suite (ML1-ML5 tests)

**Success Criteria:** All ML1-ML5 property tests pass using mock scenarios.

---

## 1. Scope & Non-Goals

### 1.1 What Phase 4e IS

✅ **Liveness property checking**:
- ML1: Templates eventually created when mining
- ML2: Solutions eventually found (given enough attempts)
- ML3: Blocks eventually submitted when found
- ML4: Mining eventually restarts after crash
- ML5: Stale templates eventually discarded

✅ **Trace-based validation**:
- Analyze Phase 4b traces for liveness violations
- Track progress toward goals
- Detect stuck states
- Verify forward progress

### 1.2 What Phase 4e is NOT

❌ **No changes to Phase 4b/4c/4d**:
- Simulator remains dumb
- Generator remains neutral
- Trace structure unchanged
- Safety/correctness oracles untouched

❌ **No production mining code changes**:
- `src/mining/*` untouched
- Real mining not used yet
- Production refactoring deferred to Phase 4h

❌ **No timing guarantees**:
- ML1-ML5 check "eventually", not "within X seconds"
- No hard time bounds in Phase 4e
- Performance testing deferred to Phase 4g

### 1.3 Why Liveness Matters

**Liveness guarantees:**
- Mining makes forward progress (doesn't get stuck)
- System recovers from crashes (restarts work)
- Templates are created (mining can proceed)
- Solutions are attempted (blocks can be found)
- Stale work is discarded (resources not wasted)

**These prevent:**
- Deadlocks
- Stuck states
- Resource exhaustion
- Silent failures
- Progress stalls

---

## 2. MiningLivenessOracle Architecture

### 2.1 Oracle Interface

```cpp
namespace mining_test {

// ============================================================================
// LivenessViolation - Represents a detected liveness violation
// ============================================================================

struct LivenessViolation {
    std::string property;      // e.g. "ML1", "ML2", etc.
    std::string message;       // Human-readable explanation
    uint64_t at_event{0};      // Index into MiningTrace.events

    LivenessViolation() = default;

    LivenessViolation(const std::string& prop, const std::string& msg, uint64_t event_idx)
        : property(prop), message(msg), at_event(event_idx) {}
};

// ============================================================================
// MiningLivenessOracle - Base class for liveness property checkers
// ============================================================================

class MiningLivenessOracle {
public:
    explicit MiningLivenessOracle(const ConsensusParams& params);
    virtual ~MiningLivenessOracle() = default;

    virtual std::string name() const = 0;
    virtual void reset();

    virtual void observe(
        const MiningState& state,
        const MiningEvent& event,
        uint64_t event_index
    ) = 0;

    virtual void finalize();

    std::vector<LivenessViolation> check(const MiningTrace& trace);

    const ConsensusParams& getParams() const { return params_; }

protected:
    void reportViolation(
        const std::string& property,
        const std::string& message,
        uint64_t event_index
    );

    ConsensusParams params_;

private:
    std::vector<LivenessViolation> violations_;
};

}  // namespace mining_test
```

### 2.2 Design Principles

**Principle 1: Eventually, not immediately**
- Liveness violations occur when progress never happens
- Not when progress is slow
- Track attempts and outcomes over full trace

**Principle 2: Bounded waiting**
- "Eventually" needs bounds for testing
- Use event count as proxy for "enough time"
- Flag if no progress after N events

**Principle 3: Conservative detection**
- Err on side of allowing slow progress
- False negatives worse than false positives
- Phase 4g will add performance bounds

---

## 3. Property Implementations

### 3.1 ML1: Templates Eventually Created

**What it checks:**
```
∀ START_MINING actions:
  ∃ TEMPLATE_CREATED event within reasonable event window
  Mining activity eventually produces templates
```

**Implementation approach:**
1. Track START_MINING actions
2. Track TEMPLATE_CREATED events
3. Verify each mining period eventually creates template
4. Flag if mining runs without creating templates

**Algorithm:**
```cpp
checkTemplatesEventuallyCreated(trace):
  mining_started = false
  events_since_start = 0
  template_created = false

  for each event in trace:
    if event == START_MINING:
      mining_started = true
      events_since_start = 0
      template_created = false

    if mining_started:
      events_since_start++

    if event == TEMPLATE_CREATED:
      template_created = true
      mining_started = false

    if event == STOP_MINING:
      if mining_started and not template_created:
        if events_since_start > THRESHOLD:
          return LIVENESS_VIOLATION
      mining_started = false
```

### 3.2 ML2: Solutions Eventually Found

**What it checks:**
```
∀ extended mining periods:
  ∃ SOLUTION_FOUND event (given enough attempts)
  Mining eventually finds solutions (not stuck)
```

**Implementation approach:**
1. Track mining duration (event count)
2. Track SOLUTION_FOUND events
3. Verify mining makes progress
4. Flag if mining runs indefinitely without solutions

**Algorithm:**
```cpp
checkSolutionsEventuallyFound(trace):
  total_mining_events = 0
  solution_count = 0

  for each event in trace:
    if state.phase == MINING:
      total_mining_events++

    if event == SOLUTION_FOUND:
      solution_count++

  // At least some solutions should be found
  // if mining ran for extended period
  if total_mining_events > THRESHOLD and solution_count == 0:
    return LIVENESS_VIOLATION
```

### 3.3 ML3: Blocks Eventually Submitted

**What it checks:**
```
∀ SOLUTION_FOUND events:
  ∃ BLOCK_SUBMITTED event shortly after
  Solutions are eventually submitted
```

**Implementation approach:**
1. Track SOLUTION_FOUND events
2. Track BLOCK_SUBMITTED events
3. Verify solutions lead to submissions
4. Flag if solutions never submitted

**Algorithm:**
```cpp
checkBlocksEventuallySubmitted(trace):
  solution_found = false
  events_since_solution = 0

  for each event in trace:
    if event == SOLUTION_FOUND:
      solution_found = true
      events_since_solution = 0

    if solution_found:
      events_since_solution++

    if event == BLOCK_SUBMITTED:
      solution_found = false

    if solution_found and events_since_solution > THRESHOLD:
      return LIVENESS_VIOLATION
```

### 3.4 ML4: Mining Eventually Restarts After Crash

**What it checks:**
```
∀ CRASH events:
  ∃ RESTART event within reasonable window
  System recovers from crashes
```

**Implementation approach:**
1. Track CRASH events
2. Track RESTART events
3. Verify crashes eventually followed by restarts
4. Flag if system stays crashed

**Algorithm:**
```cpp
checkMiningEventuallyRestartsAfterCrash(trace):
  is_crashed = false
  events_since_crash = 0

  for each event in trace:
    if event == CRASH:
      is_crashed = true
      events_since_crash = 0

    if is_crashed:
      events_since_crash++

    if event == RESTART:
      is_crashed = false

  // Check final state
  if is_crashed and events_since_crash > THRESHOLD:
    return LIVENESS_VIOLATION
```

### 3.5 ML5: Stale Templates Eventually Discarded

**What it checks:**
```
∀ reorg/new block events:
  ∃ TEMPLATE_DISCARDED event
  Stale templates are eventually cleaned up
```

**Implementation approach:**
1. Track reorg/new block events
2. Track TEMPLATE_DISCARDED events
3. Verify stale templates get discarded
4. Flag if templates never cleaned up

**Algorithm:**
```cpp
checkStaleTemplatesEventuallyDiscarded(trace):
  template_exists = false
  tip_changed = false
  events_since_tip_change = 0

  for each event in trace:
    if event == TEMPLATE_CREATED:
      template_exists = true

    if event == NEW_BLOCK_ARRIVED or event == REORG:
      if template_exists:
        tip_changed = true
        events_since_tip_change = 0

    if tip_changed:
      events_since_tip_change++

    if event == TEMPLATE_DISCARDED:
      tip_changed = false
      template_exists = false

    if tip_changed and events_since_tip_change > THRESHOLD:
      return LIVENESS_VIOLATION
```

---

## 4. Implementation Order

### Step 1: MiningLivenessOracle Base Class
**Files:**
- `tests/mining/properties/mining_liveness_oracle.h`
- `tests/mining/properties/mining_liveness_oracle.cpp`

**Tasks:**
- Define oracle interface (same pattern as safety oracle)
- Implement violation structures
- Add check() orchestration method

### Step 2: ML1 Property (Templates Eventually Created)
**Files:**
- `tests/mining/properties/mining_liveness_oracle_ml1.h`
- `tests/mining/properties/mining_liveness_oracle_ml1.cpp`
- `tests/mining/properties/test_mining_liveness_oracle_ml1.cpp`

**Tasks:**
- Implement observe() to track mining starts
- Track template creation
- Flag if no templates created within threshold

### Step 3: ML2 Property (Solutions Eventually Found)
**Files:**
- `tests/mining/properties/mining_liveness_oracle_ml2.h`
- `tests/mining/properties/mining_liveness_oracle_ml2.cpp`
- `tests/mining/properties/test_mining_liveness_oracle_ml2.cpp`

**Tasks:**
- Track mining duration
- Track solution count
- Flag if no solutions after extended mining

### Step 4: ML3 Property (Blocks Eventually Submitted)
**Files:**
- `tests/mining/properties/mining_liveness_oracle_ml3.h`
- `tests/mining/properties/mining_liveness_oracle_ml3.cpp`
- `tests/mining/properties/test_mining_liveness_oracle_ml3.cpp`

**Tasks:**
- Track solutions found
- Track block submissions
- Flag if solutions never submitted

### Step 5: ML4 Property (Mining Eventually Restarts)
**Files:**
- `tests/mining/properties/mining_liveness_oracle_ml4.h`
- `tests/mining/properties/mining_liveness_oracle_ml4.cpp`
- `tests/mining/properties/test_mining_liveness_oracle_ml4.cpp`

**Tasks:**
- Track crash events
- Track restart events
- Flag if crash never recovers

### Step 6: ML5 Property (Stale Templates Discarded)
**Files:**
- `tests/mining/properties/mining_liveness_oracle_ml5.h`
- `tests/mining/properties/mining_liveness_oracle_ml5.cpp`
- `tests/mining/properties/test_mining_liveness_oracle_ml5.cpp`

**Tasks:**
- Track template existence
- Track tip changes
- Flag if stale templates never discarded

### Step 7: Update CMakeLists.txt
**Tasks:**
- Add liveness oracle libraries
- Link against Phase 4b framework
- Register liveness property tests

---

## 5. Test Coverage Matrix

| Property | Scenario | Expected Result |
|----------|----------|-----------------|
| ML1 | Normal mining | Templates created ✓ |
| ML1 | Mining without progress | Violation flagged |
| ML1 | Start/stop cycles | Each cycle creates template |
| ML2 | Normal mining | Solutions found ✓ |
| ML2 | Mining without solutions | Violation flagged (if extended) |
| ML2 | High difficulty | Eventually finds solution |
| ML3 | Solution found | Block submitted ✓ |
| ML3 | Solution not submitted | Violation flagged |
| ML3 | Multiple solutions | All submitted |
| ML4 | Crash and restart | Recovery occurs ✓ |
| ML4 | Crash without restart | Violation flagged |
| ML4 | Multiple crashes | Each recovers |
| ML5 | Reorg | Template discarded ✓ |
| ML5 | New block | Template discarded ✓ |
| ML5 | Stale template kept | Violation flagged |

---

## 6. Thresholds and Bounds

**Phase 4e event count thresholds:**
- ML1: 100 events without template creation → violation
- ML2: 1000 events mining without solution → violation
- ML3: 50 events after solution without submission → violation
- ML4: 200 events crashed without restart → violation
- ML5: 100 events with stale template → violation

**Rationale:**
- Conservative bounds (allow slow progress)
- Event count as time proxy
- Phase 4g will add real timing

---

## 7. Exit Criteria

### 7.1 Code Completeness

✅ All property checkers implemented:
- ML1: Templates eventually created
- ML2: Solutions eventually found
- ML3: Blocks eventually submitted
- ML4: Mining eventually restarts
- ML5: Stale templates eventually discarded

✅ All tests pass:
- ML1-ML5 tests: Progress scenarios
- Integration test: All properties together

✅ Oracle integrates with Phase 4b:
- Uses MiningTrace from simulator
- No modifications to simulator
- Clean separation maintained

### 7.2 Documentation

✅ Properties documented:
- What each property ensures
- Threshold values and rationale
- Future Phase 4g integration

✅ Test coverage documented:
- Which scenarios tested
- Known limitations
- Threshold tuning notes

### 7.3 Quality Gates

✅ Builds cleanly (no warnings)
✅ All liveness tests pass
✅ No false negatives (catches stuck states)
✅ Ready for Phase 4f (determinism properties)

---

## 8. Relationship to Other Phases

### 8.1 Phase 4b Foundation

**Phase 4e depends on:**
- MiningTrace with full event log
- MiningState snapshots
- MiningEvent types (all actions recorded)

**Phase 4e does NOT modify:**
- Simulator logic
- Generator logic
- Framework self-tests

### 8.2 Phase 4c/4d Properties

**Difference from correctness/safety:**
- Correctness: "answers are right"
- Safety: "nothing bad happens"
- Liveness: "something good happens"

**Complementary properties:**
- Safety prevents inflation (MS1)
- Liveness ensures progress (ML1)
- Both needed for robust mining

### 8.3 Future Phases

**Phase 4f will add:** MD1-MD5 determinism properties
**Phase 4g will add:** MR1-MR5 restart properties
**Phase 4h will upgrade:** Real performance bounds for liveness

---

## 9. Known Limitations

### 9.1 Event Count as Time Proxy

**Limitation:** Event count ≠ real time
- Some events are fast (state change)
- Some events are slow (PoW attempt)
- Phase 4e treats all events equal

**Rationale:** Phase 4b simulator is deterministic, no real time

### 9.2 Conservative Thresholds

**Limitation:** Thresholds are arbitrary
- Based on "reasonable" event counts
- May flag slow-but-correct behavior
- May miss very slow deadlocks

**Rationale:** Phase 4e proves oracle works, Phase 4g adds real timing

### 9.3 No Performance Guarantees

**Limitation:** "Eventually" has no upper bound
- ML1-ML5 check progress occurs
- Not how fast progress occurs
- Performance testing separate concern

**Rationale:** Correctness before performance

---

## 10. Summary

**Phase 4e Goal:** Implement liveness oracle (ML1-ML5)

**Deliverables:**
1. MiningLivenessOracle base class ✓
2. ML1: Templates eventually created ✓
3. ML2: Solutions eventually found ✓
4. ML3: Blocks eventually submitted ✓
5. ML4: Mining eventually restarts ✓
6. ML5: Stale templates eventually discarded ✓
7. Liveness property test suite ✓

**Exit Criteria:** All ML* tests pass

**Next Phase:** Phase 4f - Determinism Properties (MD1-MD5)

**Status:** Design complete, ready for implementation

---

## Document Metadata

- **Created:** 2026-01-02
- **Author:** Claude Sonnet 4.5 (via Claude Code)
- **Purpose:** Implementation plan for Ring 4 Phase 4e (liveness properties)
- **Status:** Design complete, awaiting implementation

**Next Action:** Begin implementation with MiningLivenessOracle base class
