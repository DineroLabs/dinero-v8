# Ring 4 Phase 4c: Correctness Properties Implementation Plan

**Date:** 2026-01-02
**Status:** 📋 DESIGN (Implementation Plan)
**Purpose:** Implement MC1-MC5 correctness property checkers using Phase 4b framework

---

## Executive Summary

Phase 4c implements the **correctness oracle** that checks mining properties against traces from Phase 4b framework.

**Key Principle:** Oracle validates properties WITHOUT modifying the dumb simulator.

**Deliverables:**
1. `MiningCorrectnessOracle` - Checks C1-C5 properties against traces
2. Consensus subsidy calculator (for property checking only)
3. Template validator (for property checking only)
4. Property test suite (MC1-MC5 tests)
5. Integration with Phase 4b framework

**Success Criteria:** All MC1-MC5 property tests pass using mock scenarios.

---

## 1. Scope & Non-Goals

### 1.1 What Phase 4c IS

✅ **Property checking only**:
- Oracle implementation that checks traces
- Consensus subsidy calculation (for validation, not simulation)
- Template validity checking (for validation, not assembly)
- Transaction context validation (for checking, not execution)

✅ **Test implementation**:
- MC1: Subsidy correctness verification
- MC2: Coinbase structure verification
- MC3: Template validity verification
- MC4: Transaction context verification
- MC5: Consensus enforcement verification

### 1.2 What Phase 4c is NOT

❌ **No changes to Phase 4b framework**:
- Simulator remains dumb (no validation added)
- Generator remains neutral (no correctness baked in)
- Trace structure unchanged

❌ **No production mining code changes**:
- `src/mining/*` untouched
- Real BlockAssembler not used yet
- Production refactoring deferred to Phase 4h

❌ **No safety/liveness properties yet**:
- MS1-MS5 (safety) deferred to Phase 4d
- ML1-ML5 (liveness) deferred to Phase 4e
- Only correctness (C1-C5) in Phase 4c

### 1.3 Why This Separation Matters

**Phase 4b built:** Dumb infrastructure that records everything
**Phase 4c builds:** Smart oracle that validates correctness
**Separation ensures:** Framework bias-free, oracle testable

---

## 2. MiningCorrectnessOracle Architecture

### 2.1 Oracle Interface

**Purpose:** Check correctness properties against mining traces.

```cpp
namespace mining_test {

// ============================================================================
// MiningCorrectnessOracle - Validates C1-C5 properties
// ============================================================================

class MiningCorrectnessOracle {
public:
    explicit MiningCorrectnessOracle(const ConsensusParams& params);

    // MC1: Subsidy Correctness
    struct SubsidyViolation {
        uint64_t template_height;
        uint64_t claimed_subsidy;
        uint64_t expected_subsidy;
        std::string description;
    };
    std::vector<SubsidyViolation> checkSubsidyCorrectness(const MiningTrace& trace);

    // MC2: Coinbase Structure
    struct CoinbaseViolation {
        uint64_t template_height;
        std::string violation_type;  // "not_first", "no_null_input", "missing_height", etc.
        std::string description;
    };
    std::vector<CoinbaseViolation> checkCoinbaseStructure(const MiningTrace& trace);

    // MC3: Template Validity
    struct TemplateViolation {
        uint64_t template_height;
        std::string invalidity_reason;
        std::string description;
    };
    std::vector<TemplateViolation> checkTemplateValidity(const MiningTrace& trace);

    // MC4: Transaction Context
    struct ContextViolation {
        uint64_t template_height;
        uint64_t tx_hash;  // Placeholder
        std::string context_error;  // "immature_spend", "double_spend", "locktime", etc.
        std::string description;
    };
    std::vector<ContextViolation> checkTransactionContext(const MiningTrace& trace);

    // MC5: No Consensus Bypass
    struct BypassViolation {
        uint64_t template_height;
        std::string bypassed_check;  // "signature", "script_size", "softfork", etc.
        std::string description;
    };
    std::vector<BypassViolation> checkNoConsensusBypass(const MiningTrace& trace);

    // Convenience: Check all properties
    struct CorrectnessReport {
        std::vector<SubsidyViolation> subsidy_violations;
        std::vector<CoinbaseViolation> coinbase_violations;
        std::vector<TemplateViolation> template_violations;
        std::vector<ContextViolation> context_violations;
        std::vector<BypassViolation> bypass_violations;

        bool allPropertiesSatisfied() const {
            return subsidy_violations.empty() &&
                   coinbase_violations.empty() &&
                   template_violations.empty() &&
                   context_violations.empty() &&
                   bypass_violations.empty();
        }
    };
    CorrectnessReport checkAllProperties(const MiningTrace& trace);

private:
    // Consensus subsidy calculation (for validation)
    uint64_t calculateExpectedSubsidy(uint32_t height) const;

    // Consensus params (frozen)
    ConsensusParams params_;
};

}  // namespace mining_test
```

### 2.2 Design Principles

**Principle 1: Oracle is separate from simulator**
- Simulator records (dumb)
- Oracle validates (smart)
- No coupling between them

**Principle 2: Oracle uses real consensus logic**
- Subsidy calculation matches `src/consensus/subsidy.h`
- Validation rules match `src/consensus/block_validation.h`
- But applied to traces, not production

**Principle 3: Violations are descriptive**
- Not just bool pass/fail
- Return structured violation objects
- Helpful for debugging test failures

---

## 3. Property Implementations

### 3.1 MC1: Subsidy Correctness

**What it checks:**
```
∀ TEMPLATE_CREATED events in trace:
  claimed_subsidy == consensus_subsidy(height)
```

**Implementation approach:**
1. Extract all `TEMPLATE_CREATED` events from trace
2. For each template, get `template_height` and `template_subsidy`
3. Calculate expected subsidy using consensus rules
4. Compare claimed vs expected
5. Return violations if mismatch

**Edge cases:**
- Genesis block (height 0)
- First halving boundary
- Pre-halving vs post-halving
- Maximum height (subsidy → 0)

**Test strategy:**
```cpp
// Test MC1 across halving boundary
MiningSequenceGenerator gen(seed);
auto actions = gen.generateSimpleScenario();  // Creates templates at various heights

MiningSimulator sim(seed);
for (const auto& action : actions) {
    sim.applyAction(action);
}

MiningCorrectnessOracle oracle(consensus_params);
auto violations = oracle.checkSubsidyCorrectness(sim.extractTrace());

EXPECT_TRUE(violations.empty()) << "Subsidy violations detected";
```

### 3.2 MC2: Coinbase Structure

**What it checks:**
```
∀ TEMPLATE_CREATED events:
  1. Coinbase is transaction index 0
  2. Coinbase has exactly 1 input (null input)
  3. Input scriptSig contains height
  4. Outputs are standard
```

**Implementation approach:**
1. Phase 4c limitation: Since simulator uses placeholders, we check structure rules only
2. Verify template metadata (height present, subsidy claimed)
3. In future phases, integrate with real BlockAssembler for full validation

**Placeholder validation (Phase 4c):**
```cpp
// For now, check that template metadata is consistent
// Full coinbase structure validation deferred to Phase 4h (production integration)
bool checkCoinbaseStructure(const MiningTrace& trace) {
    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            // Check metadata presence
            if (!event.template_height.has_value()) {
                violations.push_back({...});
            }
            if (!event.subsidy_claimed.has_value()) {
                violations.push_back({...});
            }
        }
    }
    return violations;
}
```

### 3.3 MC3: Template Validity

**What it checks:**
```
∀ templates T:
  if validate_template(T) == true:
    block = add_valid_pow(T)
    assert validate_block(block) == true
```

**Implementation approach:**
1. Extract TEMPLATE_CREATED events
2. Check that template metadata is internally consistent
3. Verify template would produce valid block (modulo PoW)

**Placeholder validation (Phase 4c):**
```cpp
// Check template consistency
// Full block validation integration deferred to Phase 4h
bool checkTemplateValidity(const MiningTrace& trace) {
    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            // Check template references valid chain tip
            if (event.template_height != current_height + 1) {
                violations.push_back({...});
            }
            // Check subsidy is non-negative
            if (event.subsidy_claimed && *event.subsidy_claimed < 0) {
                violations.push_back({...});
            }
        }
    }
    return violations;
}
```

### 3.4 MC4: Transaction Context

**What it checks:**
```
∀ transactions tx in templates:
  1. tx valid against UTXO set at prev_block
  2. tx inputs are mature (if coinbase spends)
  3. tx locktime satisfied
  4. No double-spends within block
```

**Implementation approach:**
1. Phase 4c limitation: Mock simulator doesn't track real UTXOs
2. Check transaction selection rules (no conflicts in template)
3. Full UTXO validation deferred to Phase 4h

**Placeholder validation (Phase 4c):**
```cpp
// Check transaction count consistency
bool checkTransactionContext(const MiningTrace& trace) {
    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::TEMPLATE_CREATED) {
            // Check tx count is reasonable
            if (event.template_tx_count && *event.template_tx_count > MAX_BLOCK_TXS) {
                violations.push_back({...});
            }
        }
    }
    return violations;
}
```

### 3.5 MC5: No Consensus Bypass

**What it checks:**
```
∀ blocks B:
  validate_block_consensus(B) must not skip any check
```

**Implementation approach:**
1. Verify all templates undergo complete validation
2. Check no shortcuts taken (e.g., skipping validation after crash/restart)
3. Ensure validation is deterministic (same template → same result)

**Placeholder validation (Phase 4c):**
```cpp
// Check that validation happens consistently
bool checkNoConsensusBypass(const MiningTrace& trace) {
    // Verify templates are validated after crash/restart
    bool has_crashed = false;
    for (const auto& event : trace.events) {
        if (event.type == MiningEventType::ERROR_OCCURRED) {
            has_crashed = true;
        }
        if (has_crashed && event.type == MiningEventType::TEMPLATE_CREATED) {
            // After restart, should create new template (not reuse old)
            // This is checked implicitly by crash/restart semantics from Phase 4b
        }
    }
    return violations;
}
```

---

## 4. Consensus Subsidy Calculator

### 4.1 Purpose

**Oracle needs:** Correct subsidy calculation to validate MC1
**Source of truth:** `src/consensus/subsidy.h` (Ring 1 frozen)
**Implementation:** Copy frozen logic for oracle use

### 4.2 Implementation

```cpp
namespace mining_test {

// Ring 4 Phase 4c: Consensus subsidy calculator
// Purpose: Calculate expected subsidy for property validation
// Source: Copied from src/consensus/subsidy.h (Ring 1 frozen)
// Rule: Read-only, never modify consensus logic

class ConsensusSubsidyCalculator {
public:
    explicit ConsensusSubsidyCalculator(const ConsensusParams& params);

    // Calculate subsidy for given height
    uint64_t getBlockSubsidy(uint32_t height) const;

    // Get halving interval
    uint32_t getHalvingInterval() const { return params_.halving_interval; }

    // Get initial subsidy
    uint64_t getInitialSubsidy() const { return params_.initial_subsidy; }

private:
    ConsensusParams params_;
};

// Subsidy calculation (matches Ring 1 spec)
uint64_t ConsensusSubsidyCalculator::getBlockSubsidy(uint32_t height) const {
    // Genesis block special case
    if (height == 0) {
        return params_.genesis_subsidy;
    }

    // Calculate number of halvings
    uint32_t halvings = height / params_.halving_interval;

    // Subsidy becomes zero after 64 halvings (or when it rounds to 0)
    if (halvings >= 64) {
        return 0;
    }

    // Initial subsidy: 100 DIN (10,000,000,000 una)
    uint64_t subsidy = params_.initial_subsidy;

    // Halve subsidy for each halving period
    subsidy >>= halvings;

    return subsidy;
}

}  // namespace mining_test
```

**ConsensusParams structure:**
```cpp
struct ConsensusParams {
    uint64_t genesis_subsidy{0};                    // Genesis premine (if any)
    uint64_t initial_subsidy{100 * 100000000ULL};   // 100 DIN in una
    uint32_t halving_interval{210000};              // Blocks per halving
    uint32_t coinbase_maturity{100};                // Blocks until coinbase spendable

    // Default constructor for testing
    ConsensusParams() = default;

    // Production params (from Ring 1)
    static ConsensusParams mainnet();
    static ConsensusParams testnet();
    static ConsensusParams regtest();
};
```

---

## 5. Property Test Suite

### 5.1 Test Structure

**Pattern:**
```cpp
TEST(MiningCorrectness, MC1_SubsidyCorrectness_NormalBlocks) {
    // 1. Generate scenario
    MiningSequenceGenerator gen(seed);
    auto actions = gen.generateSimpleScenario();

    // 2. Run simulator
    MiningSimulator sim(seed);
    for (const auto& action : actions) {
        sim.applyAction(action);
    }

    // 3. Check property
    MiningCorrectnessOracle oracle(ConsensusParams::regtest());
    auto violations = oracle.checkSubsidyCorrectness(sim.extractTrace());

    // 4. Assert no violations
    EXPECT_TRUE(violations.empty()) << formatViolations(violations);
}
```

### 5.2 Test Coverage Matrix

| Property | Scenario | Expected Result |
|----------|----------|----------------|
| MC1 | Simple mining | Subsidy = 100 DIN |
| MC1 | Pre-halving boundary | Subsidy = 100 DIN |
| MC1 | Post-halving boundary | Subsidy = 50 DIN |
| MC1 | Genesis block | Subsidy = genesis amount |
| MC2 | Normal template | Coinbase at index 0 |
| MC2 | After crash/restart | New coinbase created |
| MC3 | Valid template | No violations |
| MC3 | After reorg | Template rebuilt on new tip |
| MC4 | Normal transactions | All valid in context |
| MC4 | Empty mempool | Coinbase-only valid |
| MC5 | All scenarios | No bypass detected |

### 5.3 Test Files

**Create:**
- `tests/mining/properties/test_mc1_subsidy_correctness.cpp`
- `tests/mining/properties/test_mc2_coinbase_structure.cpp`
- `tests/mining/properties/test_mc3_template_validity.cpp`
- `tests/mining/properties/test_mc4_transaction_context.cpp`
- `tests/mining/properties/test_mc5_no_consensus_bypass.cpp`

**Integration test:**
- `tests/mining/properties/test_all_correctness_properties.cpp`

---

## 6. Implementation Order

### Step 1: ConsensusParams & SubsidyCalculator
**Files:**
- `tests/mining/properties/consensus_params.h`
- `tests/mining/properties/subsidy_calculator.h/cpp`

**Tasks:**
- Define ConsensusParams structure
- Implement getBlockSubsidy() matching Ring 1 spec
- Add unit tests for subsidy calculation

### Step 2: MiningCorrectnessOracle Base
**Files:**
- `tests/mining/properties/mining_correctness_oracle.h/cpp`

**Tasks:**
- Define oracle interface
- Implement violation structures
- Add checkAllProperties() aggregator

### Step 3: MC1 Property (Subsidy Correctness)
**Files:**
- `tests/mining/properties/test_mc1_subsidy_correctness.cpp`

**Tasks:**
- Implement checkSubsidyCorrectness()
- Add subsidy validation tests
- Test across halving boundaries

### Step 4: MC2-MC5 Properties (Placeholder Validation)
**Files:**
- `tests/mining/properties/test_mc2_coinbase_structure.cpp`
- `tests/mining/properties/test_mc3_template_validity.cpp`
- `tests/mining/properties/test_mc4_transaction_context.cpp`
- `tests/mining/properties/test_mc5_no_consensus_bypass.cpp`

**Tasks:**
- Implement placeholder checks for MC2-MC5
- Add property tests for each
- Document limitations (full validation in Phase 4h)

### Step 5: Integration Tests
**Files:**
- `tests/mining/properties/test_all_correctness_properties.cpp`

**Tasks:**
- Test all MC1-MC5 together
- Run on complex scenarios (crash, reorg, restart)
- Verify no false positives/negatives

### Step 6: Update CMakeLists.txt
**Tasks:**
- Add property test library
- Link against Phase 4b framework
- Register property tests

---

## 7. Exit Criteria

### 7.1 Code Completeness

✅ All property checkers implemented:
- MC1: Subsidy correctness ✓
- MC2: Coinbase structure (placeholder) ✓
- MC3: Template validity (placeholder) ✓
- MC4: Transaction context (placeholder) ✓
- MC5: No consensus bypass (placeholder) ✓

✅ All tests pass:
- MC1 tests: 5+ scenarios
- MC2-MC5 tests: Basic validation
- Integration test: All properties together

✅ Oracle integrates with Phase 4b:
- Uses MiningTrace from simulator
- No modifications to simulator
- Clean separation maintained

### 7.2 Documentation

✅ Properties documented:
- What each property checks
- Placeholder vs full validation
- Future Phase 4h integration

✅ Test coverage documented:
- Which scenarios tested
- Known limitations
- False positive/negative risks

### 7.3 Quality Gates

✅ Builds cleanly (no warnings)
✅ All property tests pass
✅ Subsidy calculation matches Ring 1
✅ No coupling to simulator
✅ Ready for Phase 4d (safety properties)

---

## 8. Relationship to Other Phases

### 8.1 Phase 4b Foundation

**Phase 4c depends on:**
- MiningTrace structure
- MiningState snapshots
- MiningEvent log
- Deterministic simulator

**Phase 4c does NOT modify:**
- Simulator logic
- Generator logic
- Framework self-tests

### 8.2 Phase 4d-4g Future

**Phase 4d will add:** MS1-MS5 safety properties
**Phase 4e will add:** ML1-ML5 liveness properties
**Phase 4f will add:** MD1-MD5 determinism properties
**Phase 4g will add:** MR1-MR5 restart properties

**All will use same pattern:**
1. Define oracle
2. Implement property checkers
3. Write tests
4. Verify no violations

### 8.3 Phase 4h Production Integration

**Phase 4h will:**
- Replace mock simulator with real BlockAssembler
- Add full coinbase structure validation (MC2)
- Add full template validity checking (MC3)
- Add real UTXO context validation (MC4)
- Add full consensus bypass detection (MC5)

**Phase 4c prepares for this by:**
- Establishing oracle pattern
- Defining violation structures
- Creating test scaffolding

---

## 9. Known Limitations

### 9.1 Placeholder Validation

**MC2-MC5 are partially implemented:**
- MC2: Metadata checks only (no real coinbase)
- MC3: Consistency checks only (no block validation)
- MC4: Count checks only (no UTXO validation)
- MC5: Implicit checks only (no bypass detection)

**Rationale:** Phase 4b simulator uses placeholders, so full validation requires Phase 4h production integration.

### 9.2 No Real Blocks

**Oracle validates traces, not blocks:**
- Traces contain events/states, not actual transactions
- Subsidy checked, but no real coinbase created
- Template "validity" is metadata consistency

**This is intentional:** Phase 4c proves oracle works before integrating real mining.

### 9.3 No Performance Testing

**Phase 4c focuses on correctness:**
- Oracle performance not measured
- Validation speed not optimized
- Stress testing deferred to Phase 4i

---

## 10. Summary

**Phase 4c Goal:** Implement correctness oracle (MC1-MC5)

**Deliverables:**
1. ConsensusSubsidyCalculator ✓
2. MiningCorrectnessOracle ✓
3. MC1: Full subsidy validation ✓
4. MC2-MC5: Placeholder validation ✓
5. Property test suite ✓

**Exit Criteria:** All MC* tests pass

**Next Phase:** Phase 4d - Safety Properties (MS1-MS5)

**Status:** Design complete, ready for implementation

---

## Document Metadata

- **Created:** 2026-01-02
- **Author:** Claude Sonnet 4.5 (via Claude Code)
- **Purpose:** Implementation plan for Ring 4 Phase 4c (correctness properties)
- **Status:** Design complete, awaiting implementation

**Next Action:** Begin implementation with ConsensusSubsidyCalculator

