# Ring 4 Phase 4b: Property Test Framework Design

**Date:** 2026-01-02
**Status:** 📋 DESIGN (No Code Yet)
**Purpose:** Define testing infrastructure before implementing properties

---

## Executive Summary

Phase 4b creates the **testing substrate** for Ring 4 mining properties (phases 4c-4g), without asserting any properties yet.

**Key Principle:** Build a neutral, reusable framework that doesn't bake in assumptions about mining behavior.

**Deliverables:**
1. Core abstractions (MiningAction, MiningState, MiningEvent, MiningTrace)
2. Deterministic mining simulator
3. Scenario/sequence generator
4. Crash/restart injection model
5. Test harness contract

**Success Criteria:** Framework self-tests pass, determinism proven, NO mining properties asserted yet.

---

## 1. Scope & Non-Goals

### 1.1 What Phase 4b IS

✅ **Infrastructure only**:
- Core abstractions for representing mining scenarios
- Deterministic simulator for replaying scenarios
- Scenario generator for creating random-but-replayable tests
- Crash/restart injection mechanism
- Test harness for future property tests

✅ **Self-tests only**:
- Determinism verification (same seed → same trace)
- Crash/restart replay verification
- Generator coverage checks

### 1.2 What Phase 4b is NOT

❌ **No mining property assertions**:
- No correctness checks (C1-C5)
- No safety checks (S1-S5)
- No liveness checks (L1-L5)
- No determinism checks (MD1-MD5)
- No restart checks (MR1-MR5)

❌ **No consensus logic**:
- Framework must not validate blocks
- Framework must not calculate subsidies
- Framework must not check transaction validity
- All consensus validation deferred to Phase 4c+

❌ **No production code changes**:
- Phase 4b only creates test infrastructure
- No modifications to `src/mining/*` yet
- Production refactoring deferred to Phase 4h

### 1.3 Why This Separation Matters

**Ring 3 Lesson:** Test framework encoded P2P behavior assumptions → had to refactor framework later.

**Ring 4 Discipline:** Framework is dumb, properties are smart.
- Framework records what happened
- Properties assert what should happen
- Separation prevents bias

---

## 2. Core Abstractions

### 2.1 MiningAction (Input Events)

**Purpose:** Represents external events that affect mining.

```cpp
enum class MiningActionType {
    START_MINING,         // Begin mining
    STOP_MINING,          // Stop mining
    NEW_BLOCK_ARRIVED,    // Chain tip changed
    TX_ADDED_TO_MEMPOOL,  // Mempool updated
    TX_REMOVED_FROM_MEMPOOL,
    TIME_ADVANCED,        // Mock clock tick
    CRASH,                // Simulate crash
    RESTART,              // Simulate restart
    REORG                 // Chain reorganization
};

struct MiningAction {
    MiningActionType type;
    uint64_t timestamp;        // Mock time when action occurred

    // Action-specific data (union or variant)
    std::optional<BlockHash> new_tip_hash;
    std::optional<TxHash> tx_hash;
    std::optional<uint32_t> reorg_depth;

    // Metadata for replay
    uint64_t sequence_number;  // For total ordering
    std::string description;   // Human-readable
};
```

**Design Notes:**
- Actions are **inputs** to the system (external events)
- Actions are **deterministic** given RNG seed
- Actions do NOT encode expected outcomes
- Actions are **replayable** from sequence number

### 2.2 MiningState (System State)

**Purpose:** Represents observable state of mining system at a point in time.

```cpp
enum class MiningPhase {
    STOPPED,     // No mining thread
    IDLE,        // Thread running, no work
    ASSEMBLING,  // Building block template
    MINING,      // Hashing
    SUBMITTING   // Found solution, submitting block
};

struct MiningState {
    MiningPhase phase;
    uint64_t timestamp;

    // Chain state
    BlockHash current_tip;
    uint32_t current_height;

    // Mempool state
    uint32_t mempool_size;
    uint64_t mempool_total_fees;

    // Mining state
    std::optional<BlockHash> template_prev_hash;
    std::optional<uint32_t> template_height;
    std::optional<uint64_t> template_subsidy;
    std::optional<uint32_t> template_tx_count;

    // Statistics
    uint64_t hashes_computed;
    uint64_t blocks_found;
    uint64_t templates_created;

    // Lifecycle tracking
    bool has_crashed;
    uint32_t restart_count;
};
```

**Design Notes:**
- State is **observable** (read-only view of system)
- State is **complete** (enough to check any property)
- State does NOT include implementation details (thread IDs, locks, etc.)
- State is **serializable** (can persist and restore)

### 2.3 MiningEvent (Output Events)

**Purpose:** Represents observable outcomes of mining operations.

```cpp
enum class MiningEventType {
    TEMPLATE_CREATED,      // Block template assembled
    TEMPLATE_DISCARDED,    // Old template abandoned
    POW_STARTED,           // Started hashing
    POW_STOPPED,           // Stopped hashing
    SOLUTION_FOUND,        // Valid PoW found
    BLOCK_SUBMITTED,       // Block submitted to network
    BLOCK_ACCEPTED,        // Block accepted by consensus
    BLOCK_REJECTED,        // Block rejected by consensus
    ERROR_OCCURRED         // Error in mining pipeline
};

struct MiningEvent {
    MiningEventType type;
    uint64_t timestamp;

    // Event-specific data
    std::optional<BlockHash> block_hash;
    std::optional<uint32_t> template_height;
    std::optional<uint64_t> subsidy_claimed;
    std::optional<std::string> error_message;

    // Metadata
    uint64_t sequence_number;
    std::string description;
};
```

**Design Notes:**
- Events are **outputs** from the system (observables)
- Events are **timestamped** (can reconstruct timeline)
- Events do NOT include expected values (that's for assertions)
- Events are **traceable** (linked to causal actions)

### 2.4 MiningTrace (Execution History)

**Purpose:** Complete record of a mining scenario execution.

```cpp
struct MiningTrace {
    // Configuration
    uint64_t rng_seed;
    std::string scenario_name;

    // Execution history
    std::vector<MiningAction> actions;    // Inputs
    std::vector<MiningEvent> events;      // Outputs
    std::vector<MiningState> snapshots;   // State at checkpoints

    // Determinism verification
    uint64_t final_hash;  // Hash of entire trace

    // Metadata
    uint64_t start_time;
    uint64_t end_time;
    bool completed_successfully;
    std::optional<std::string> failure_reason;
};
```

**Design Notes:**
- Trace is **complete record** of execution
- Trace is **deterministic** given seed
- Trace is **replayable** (can re-execute from actions)
- Trace is **minimizable** (can delta-debug failing scenarios)

### 2.5 MiningOracle (Deferred to Phase 4c+)

**Purpose:** Will check properties against traces in future phases.

```cpp
// NOT IMPLEMENTED IN PHASE 4B
// This is just the interface contract for future phases

class MiningOracle {
public:
    // Phase 4c: Correctness properties
    virtual bool checkSubsidyCorrectness(const MiningTrace& trace) = 0;
    virtual bool checkCoinbaseStructure(const MiningTrace& trace) = 0;
    virtual bool checkTemplateValidity(const MiningTrace& trace) = 0;

    // Phase 4d: Safety properties
    virtual bool checkNoInflationOnRestart(const MiningTrace& trace) = 0;
    virtual bool checkNoDuplicateSubsidy(const MiningTrace& trace) = 0;

    // ... (ML*, MD*, MR* deferred)
};
```

**Design Notes:**
- Oracle is **NOT implemented in Phase 4b**
- Oracle interface defined to ensure framework is testable
- Oracle implementations in phases 4c-4g
- Framework must support oracle without depending on it

---

## 3. Deterministic Mining Simulator

### 3.1 Purpose

Create a **mock mining environment** that:
- Accepts `MiningAction` inputs
- Produces `MiningEvent` outputs
- Maintains `MiningState`
- Is fully deterministic given RNG seed

**Key Constraint:** Simulator must NOT contain real consensus logic.

### 3.2 Simulator Interface

```cpp
class DeterministicMiningSimulator {
public:
    // Configuration
    explicit DeterministicMiningSimulator(uint64_t rng_seed);

    // Execution
    void applyAction(const MiningAction& action);
    MiningState getCurrentState() const;
    std::vector<MiningEvent> getEventsSince(uint64_t timestamp) const;

    // State management
    void saveCheckpoint();
    void restoreCheckpoint();
    void reset();

    // Trace extraction
    MiningTrace extractTrace() const;

private:
    uint64_t rng_seed_;
    std::mt19937_64 rng_;

    MiningState current_state_;
    std::vector<MiningEvent> event_log_;
    std::vector<MiningAction> action_log_;
    std::vector<MiningState> checkpoints_;

    // Mock components (NOT real implementations)
    MockChainTip chain_tip_;
    MockMempool mempool_;
    MockBlockAssembler assembler_;
    MockMiningEngine engine_;
};
```

### 3.3 Determinism Contract

**Requirement:**
```
∀ seeds s:
  ∀ action sequences A:
    simulate(s, A) → trace T₁
    simulate(s, A) → trace T₂

    T₁.final_hash == T₂.final_hash
```

**Proof Obligation:** Phase 4b must test this property.

**Test:**
```cpp
TEST(DeterministicSimulator, SameSeedProducesSameTrace) {
    const uint64_t seed = 42;
    const auto actions = generateRandomActions(seed, 100);

    auto sim1 = DeterministicMiningSimulator(seed);
    auto sim2 = DeterministicMiningSimulator(seed);

    for (const auto& action : actions) {
        sim1.applyAction(action);
        sim2.applyAction(action);
    }

    auto trace1 = sim1.extractTrace();
    auto trace2 = sim2.extractTrace();

    EXPECT_EQ(trace1.final_hash, trace2.final_hash);
    EXPECT_EQ(trace1.events.size(), trace2.events.size());
}
```

### 3.4 Mock Components

**MockChainTip:**
- Tracks current tip hash and height
- Responds to `NEW_BLOCK_ARRIVED` and `REORG` actions
- Does NOT validate blocks (just records state)

**MockMempool:**
- Tracks set of transactions
- Responds to `TX_ADDED` and `TX_REMOVED` actions
- Does NOT validate transactions
- Returns deterministic transaction selection

**MockBlockAssembler:**
- Creates mock block templates
- Subsidy is **placeholder** (not consensus-accurate)
- Transaction selection is **deterministic** (seeded RNG)
- Does NOT perform actual consensus validation

**MockMiningEngine:**
- Simulates PoW hashing
- Finds "solutions" based on RNG (not real SHA256d)
- Deterministic: given seed, always finds solution at same iteration
- Records hashes computed for statistics

### 3.5 Explicit Non-Consensus Guarantees

**What simulator does NOT do:**
- ❌ Validate block structures
- ❌ Calculate real subsidies
- ❌ Check transaction validity
- ❌ Enforce consensus rules
- ❌ Compute real merkle roots
- ❌ Perform real PoW

**Why:** Consensus validation is the property being tested, not the framework.

**Implication:** Phase 4c will add `ConsensusOracle` to check these, separate from simulator.

---

## 4. Scenario / Sequence Generator

### 4.1 Purpose

Generate random-but-replayable sequences of `MiningAction` to explore state space.

**Design Principle:** Generator knows NOTHING about expected outcomes.

### 4.2 Generator Interface

```cpp
class MiningSequenceGenerator {
public:
    explicit MiningSequenceGenerator(uint64_t seed);

    // Generate action sequences
    std::vector<MiningAction> generateSimpleScenario();
    std::vector<MiningAction> generateRestartScenario();
    std::vector<MiningAction> generateReorgScenario();
    std::vector<MiningAction> generateCrashScenario();
    std::vector<MiningAction> generateRandomScenario(size_t action_count);

    // Combine scenarios
    std::vector<MiningAction> combineScenarios(
        std::vector<std::vector<MiningAction>> scenarios
    );

private:
    std::mt19937_64 rng_;

    // Probability distributions for random scenarios
    std::discrete_distribution<> action_type_dist_;
    std::uniform_int_distribution<> time_delta_dist_;
};
```

### 4.3 Scenario Types

**Simple Scenario:**
```
START_MINING
  → TIME_ADVANCED (simulate hashing)
  → SOLUTION_FOUND (deterministic)
  → BLOCK_ACCEPTED
  → STOP_MINING
```

**Restart Scenario:**
```
START_MINING
  → TIME_ADVANCED
  → CRASH
  → RESTART
  → START_MINING
  → SOLUTION_FOUND
```

**Reorg Scenario:**
```
START_MINING
  → MINING on tip A
  → REORG (tip changes to B)
  → TEMPLATE_DISCARDED
  → MINING on tip B
```

**Crash Scenario:**
```
START_MINING
  → ASSEMBLING template
  → CRASH (mid-assembly)
  → RESTART
  → STATE_RECOVERED (no partial template)
```

**Random Scenario:**
```
Random interleaving of:
  - START/STOP mining
  - New blocks arriving
  - Mempool updates
  - Time advances
  - Crashes/restarts
  - Reorgs
```

### 4.4 Bias Control

**Problem:** Pure random scenarios have low coverage of interesting states.

**Solution:** Weighted probabilities for action types.

**Example Weights:**
```cpp
action_type_dist_ = std::discrete_distribution<>({
    10,  // START_MINING (rare)
    10,  // STOP_MINING (rare)
    30,  // NEW_BLOCK_ARRIVED (common)
    20,  // TX_ADDED (common)
    20,  // TIME_ADVANCED (common)
    5,   // CRASH (uncommon)
    5    // REORG (uncommon)
});
```

**Configurable:** Different test suites can use different weights.

### 4.5 Determinism Contract

**Requirement:**
```
∀ seeds s:
  generate(s) → actions A₁
  generate(s) → actions A₂

  A₁ == A₂ (exact same sequence)
```

**Test:**
```cpp
TEST(SequenceGenerator, SameSeedProducesSameSequence) {
    const uint64_t seed = 123;

    auto gen1 = MiningSequenceGenerator(seed);
    auto gen2 = MiningSequenceGenerator(seed);

    auto seq1 = gen1.generateRandomScenario(100);
    auto seq2 = gen2.generateRandomScenario(100);

    EXPECT_EQ(seq1, seq2);
}
```

---

## 5. Crash / Restart Injection Model

### 5.1 Purpose

Model what happens during crash and restart, without real process termination.

**Key Insight:** We don't actually kill processes; we model state transitions.

### 5.2 Crash Semantics

**When `CRASH` action occurs:**
```cpp
void DeterministicMiningSimulator::applyCrash() {
    // 1. Mark state as crashed
    current_state_.has_crashed = true;
    current_state_.phase = MiningPhase::STOPPED;

    // 2. Discard volatile state (memory-only)
    discardVolatileState();

    // 3. Persist durable state (disk-backed)
    persistDurableState();

    // 4. Record crash event
    recordEvent(MiningEventType::ERROR_OCCURRED, "Crash injected");
}
```

**What is discarded (volatile):**
- Current block template
- In-flight nonces
- Mining thread state
- Lock states
- Partial assembly state

**What is preserved (durable):**
- Chain tip hash/height
- Mempool contents (if disk-backed)
- Wallet state
- Configuration

### 5.3 Restart Semantics

**When `RESTART` action occurs:**
```cpp
void DeterministicMiningSimulator::applyRestart() {
    // 1. Verify system was crashed
    if (!current_state_.has_crashed) {
        throw std::logic_error("Cannot restart if not crashed");
    }

    // 2. Reload durable state from "disk"
    reloadDurableState();

    // 3. Reset volatile state
    resetVolatileState();

    // 4. Increment restart counter
    current_state_.restart_count++;
    current_state_.has_crashed = false;

    // 5. Do NOT auto-start mining (requires explicit START_MINING action)
}
```

**Invariant After Restart:**
```cpp
// Chain state preserved
EXPECT_EQ(state_after_restart.current_tip, state_before_crash.current_tip);

// Mempool preserved (if durable)
EXPECT_EQ(state_after_restart.mempool_size, state_before_crash.mempool_size);

// Mining state reset
EXPECT_EQ(state_after_restart.phase, MiningPhase::STOPPED);
EXPECT_FALSE(state_after_restart.template_prev_hash.has_value());
```

### 5.4 Crash Points (Future Extension)

**Phase 4b:** Only supports coarse-grained crash (entire simulator).

**Phase 4g:** May add fine-grained crash points:
- During template assembly
- During PoW hashing
- During block submission
- During mempool update

**Not Needed Yet:** Keep simple for Phase 4b.

### 5.5 Nondeterminism Boundaries

**Deterministic:**
- When crash occurs (seeded RNG)
- Which state is discarded vs preserved
- Restart sequence

**Nondeterministic (but controlled):**
- Exact timestamp of crash (within bounds)
- Race conditions during crash (modeled explicitly)

**Test:**
```cpp
TEST(CrashInjection, CrashAndRestartIsReproducible) {
    const uint64_t seed = 999;

    auto sim1 = DeterministicMiningSimulator(seed);
    auto sim2 = DeterministicMiningSimulator(seed);

    sim1.applyAction(MiningAction{.type = START_MINING});
    sim2.applyAction(MiningAction{.type = START_MINING});

    sim1.applyAction(MiningAction{.type = CRASH});
    sim2.applyAction(MiningAction{.type = CRASH});

    sim1.applyAction(MiningAction{.type = RESTART});
    sim2.applyAction(MiningAction{.type = RESTART});

    EXPECT_EQ(sim1.getCurrentState(), sim2.getCurrentState());
}
```

---

## 6. Test Harness Contract

### 6.1 Purpose

Define how property tests (phases 4c-4g) will use this framework.

**Design Goal:** Properties plug into framework without modifying framework.

### 6.2 Property Test Pattern

```cpp
// Phase 4c example (NOT IMPLEMENTED IN 4b)
TEST(MiningCorrectness, MC1_SubsidyCorrectness) {
    // 1. Generate scenario
    MiningSequenceGenerator gen(seed);
    auto actions = gen.generateRandomScenario(100);

    // 2. Simulate
    DeterministicMiningSimulator sim(seed);
    for (const auto& action : actions) {
        sim.applyAction(action);
    }
    auto trace = sim.extractTrace();

    // 3. Check property (Phase 4c implements this)
    MiningCorrectnessOracle oracle;
    EXPECT_TRUE(oracle.checkSubsidyCorrectness(trace));
}
```

### 6.3 Trace Minimization

**Problem:** Random scenario with 1000 actions fails. Which action caused failure?

**Solution:** Delta debugging.

```cpp
MiningTrace minimizeFailingTrace(
    const MiningTrace& original_trace,
    std::function<bool(const MiningTrace&)> property_check
) {
    auto actions = original_trace.actions;

    // Binary search for minimal failing subset
    while (actions.size() > 1) {
        auto half = actions.size() / 2;
        auto subset = std::vector(actions.begin(), actions.begin() + half);

        auto trace = replay(subset);
        if (!property_check(trace)) {
            // Failure reproduced with smaller subset
            actions = subset;
        } else {
            // Need second half
            actions = std::vector(actions.begin() + half, actions.end());
        }
    }

    return replay(actions);
}
```

**Requirement:** Simulator must support partial replay.

**Test:**
```cpp
TEST(TestHarness, TraceMinimizationWorks) {
    // Generate a failing trace (artificially)
    auto large_trace = generateLargeFailingTrace();

    auto minimal_trace = minimizeFailingTrace(large_trace, [](auto& trace) {
        return trace.events.size() < 10;  // Dummy property
    });

    EXPECT_LT(minimal_trace.actions.size(), large_trace.actions.size());
}
```

### 6.4 Regression Test Capture

**When property fails:**
```cpp
// Automatically save trace to file
void saveRegressionTest(const MiningTrace& trace, const std::string& property_name) {
    std::ofstream file("tests/mining/regression/" + property_name + ".trace");
    file << serializeTrace(trace);
}

// Later: Replay exact trace
TEST(Regression, MC1_FailedOn2026_01_02) {
    auto trace = loadTrace("tests/mining/regression/MC1_subsidy.trace");

    DeterministicMiningSimulator sim(trace.rng_seed);
    for (const auto& action : trace.actions) {
        sim.applyAction(action);
    }

    auto replay_trace = sim.extractTrace();
    EXPECT_EQ(replay_trace.final_hash, trace.final_hash);
}
```

### 6.5 Test Harness Self-Tests

**Phase 4b Exit Criteria:** These tests must pass.

```cpp
// 1. Determinism
TEST(Framework, DeterminismVerified);

// 2. Crash/Restart
TEST(Framework, CrashAndRestartReplayable);

// 3. Trace extraction
TEST(Framework, TraceExtractionComplete);

// 4. Scenario generation
TEST(Framework, ScenarioGenerationDeterministic);

// 5. Minimization
TEST(Framework, TraceMinimizationWorks);
```

**No property assertions yet** — just framework sanity checks.

---

## 7. Exit Criteria

### 7.1 Phase 4b Completion Checklist

**Code Deliverables:**
- [ ] `tests/mining/property_test_framework.h` (core abstractions)
- [ ] `tests/mining/deterministic_mining_simulator.h/.cpp`
- [ ] `tests/mining/mining_sequence_generator.h/.cpp`
- [ ] `tests/mining/crash_injection.h/.cpp`
- [ ] `tests/mining/test_framework_sanity.cpp` (self-tests)

**Test Deliverables:**
- [ ] Determinism test passes (same seed → same trace)
- [ ] Crash/restart test passes (reproducible)
- [ ] Trace extraction test passes (complete history)
- [ ] Scenario generation test passes (reproducible)
- [ ] Trace minimization test passes (delta debugging works)

**Documentation Deliverables:**
- [ ] This document (Phase 4b design)
- [ ] Code comments documenting abstractions
- [ ] Example usage in test file

**Negative Criteria (must NOT exist):**
- [ ] No mining property assertions (C*, S*, L*, M*, R*)
- [ ] No consensus validation logic
- [ ] No real block assembly
- [ ] No production code changes

### 7.2 Verification Tests

**Test 1: Determinism**
```cpp
TEST(FrameworkSanity, DeterminismGuarantee) {
    for (int seed = 0; seed < 100; seed++) {
        auto gen = MiningSequenceGenerator(seed);
        auto actions = gen.generateRandomScenario(50);

        auto sim1 = DeterministicMiningSimulator(seed);
        auto sim2 = DeterministicMiningSimulator(seed);

        for (const auto& action : actions) {
            sim1.applyAction(action);
            sim2.applyAction(action);
        }

        auto trace1 = sim1.extractTrace();
        auto trace2 = sim2.extractTrace();

        EXPECT_EQ(trace1.final_hash, trace2.final_hash);
    }
}
```

**Test 2: Crash/Restart**
```cpp
TEST(FrameworkSanity, CrashRestartReproducible) {
    const uint64_t seed = 42;

    auto gen = MiningSequenceGenerator(seed);
    auto actions = gen.generateCrashScenario();

    auto sim1 = DeterministicMiningSimulator(seed);
    auto sim2 = DeterministicMiningSimulator(seed);

    for (const auto& action : actions) {
        sim1.applyAction(action);
        sim2.applyAction(action);
    }

    EXPECT_EQ(sim1.getCurrentState(), sim2.getCurrentState());
}
```

**Test 3: No Property Assertions**
```cpp
// This test verifies that framework DOES NOT assert properties
TEST(FrameworkSanity, NoPropertiesAssertedYet) {
    // Generate obviously incorrect scenario (subsidy too high)
    auto actions = createInvalidSubsidyScenario();

    DeterministicMiningSimulator sim(123);

    // Simulator should NOT fail on invalid subsidy
    // (property checking is Phase 4c's job, not framework's)
    EXPECT_NO_THROW({
        for (const auto& action : actions) {
            sim.applyAction(action);
        }
    });

    // Framework just records events, doesn't validate
    auto trace = sim.extractTrace();
    EXPECT_FALSE(trace.events.empty());
}
```

### 7.3 Success Criteria Summary

**Phase 4b is complete when:**

1. ✅ All framework self-tests pass
2. ✅ Determinism proven (100 seeds tested)
3. ✅ Crash/restart injection works
4. ✅ Trace minimization works
5. ✅ NO mining properties asserted
6. ✅ NO consensus logic in framework
7. ✅ Documentation complete

**Phase 4b is NOT complete if:**

- ❌ Any property assertion exists (C*, S*, L*, M*, R*)
- ❌ Framework validates subsidies
- ❌ Framework validates blocks
- ❌ Production code modified

---

## 8. Relationship to Future Phases

### 8.1 Phase 4c: Correctness Properties

**Will use framework like this:**
```cpp
TEST(MiningCorrectness, MC1_SubsidyCorrectness) {
    MiningSequenceGenerator gen(seed);
    auto actions = gen.generateRandomScenario(100);

    DeterministicMiningSimulator sim(seed);
    for (const auto& action : actions) {
        sim.applyAction(action);
    }

    // Phase 4c adds this:
    MiningCorrectnessOracle oracle;
    EXPECT_TRUE(oracle.checkSubsidyCorrectness(sim.extractTrace()));
}
```

**Framework provides:** Actions, State, Events, Trace

**Phase 4c provides:** Oracle implementation

### 8.2 Phase 4d: Safety Properties

**Will use same framework:**
```cpp
TEST(MiningSafety, MS1_NoInflationOnRestart) {
    auto actions = gen.generateRestartScenario();
    // ... simulate ...

    // Phase 4d adds this:
    MiningSafetyOracle oracle;
    EXPECT_TRUE(oracle.checkNoInflation(sim.extractTrace()));
}
```

### 8.3 Phases 4e-4g: Similar Pattern

All phases use same framework, add different oracles.

**Framework stability:** Once Phase 4b is complete, framework should NOT change.

---

## 9. Design Decisions

### 9.1 Why Mock Components Instead of Real?

**Decision:** Use `MockBlockAssembler`, `MockMempool`, etc.

**Alternative:** Use real components.

**Reasoning:**
- Real components have real consensus logic
- Testing framework shouldn't depend on production correctness
- Mocks allow testing property violations (invalid subsidies, etc.)
- Mocks are deterministic (real components may not be)

**Tradeoff:** Mocks don't catch production bugs in BlockAssembler.

**Mitigation:** Phase 4h integration tests use real components.

### 9.2 Why Deterministic Simulator Instead of Real Mining?

**Decision:** Simulator uses seeded RNG, not real PoW.

**Alternative:** Real SHA256d mining.

**Reasoning:**
- Real PoW is slow (minutes per block)
- Real PoW is nondeterministic (can't replay)
- Property tests need 1000s of iterations
- Simulator can "find solution" instantly (seeded)

**Tradeoff:** Doesn't test PoW correctness.

**Mitigation:** Phase 4h tests real PoW separately.

### 9.3 Why Separate Generator from Simulator?

**Decision:** `MiningSequenceGenerator` creates actions, `DeterministicMiningSimulator` executes them.

**Alternative:** Generator directly calls simulator.

**Reasoning:**
- Separation of concerns (creation vs execution)
- Allows action sequences to be saved/loaded
- Enables trace minimization (replay partial sequences)
- Supports multiple simulators (production vs mock)

**Benefit:** Can reuse same action sequences across phases.

### 9.4 Why Trace Final Hash Instead of Deep Comparison?

**Decision:** Compare `trace1.final_hash == trace2.final_hash`.

**Alternative:** Compare every field in trace.

**Reasoning:**
- Hash is fast (single comparison)
- Hash catches any difference (no need to check each field)
- Hash is deterministic (same trace → same hash)

**Tradeoff:** Hash doesn't show WHERE traces differ.

**Mitigation:** On hash mismatch, do deep comparison for debugging.

---

## 10. Implementation Strategy

### 10.1 Implementation Order (Recommended)

**Step 1:** Core abstractions (2-3 hours)
- Implement `MiningAction`, `MiningState`, `MiningEvent`, `MiningTrace` structs
- Add serialization (for trace persistence)
- Add equality operators (for determinism checks)

**Step 2:** Mock components (3-4 hours)
- Implement `MockChainTip`, `MockMempool`, `MockBlockAssembler`, `MockMiningEngine`
- Keep simple (no real consensus logic)
- Make deterministic (seeded RNG)

**Step 3:** Deterministic simulator (4-5 hours)
- Implement `DeterministicMiningSimulator`
- Wire up mock components
- Add state tracking
- Add event logging

**Step 4:** Sequence generator (2-3 hours)
- Implement `MiningSequenceGenerator`
- Add scenario templates (simple, crash, reorg, etc.)
- Add random scenario generation
- Add determinism test

**Step 5:** Crash/restart injection (2-3 hours)
- Implement volatile vs durable state separation
- Add crash semantics
- Add restart semantics
- Add determinism test

**Step 6:** Test harness utilities (2-3 hours)
- Implement trace minimization
- Add regression test saving/loading
- Add trace comparison utilities
- Add framework self-tests

**Step 7:** Documentation and cleanup (1-2 hours)
- Add code comments
- Write usage examples
- Verify exit criteria
- Tag completion

**Total Estimate:** 16-23 hours (2-3 days)

### 10.2 Testing During Implementation

**After Step 1:** Test that structs serialize/deserialize correctly.

**After Step 2:** Test that mocks are deterministic.

**After Step 3:** Test that simulator produces same trace given same seed.

**After Step 4:** Test that generator produces same sequences given same seed.

**After Step 5:** Test that crash/restart is reproducible.

**After Step 6:** Test that trace minimization works.

**Final:** Run all framework sanity tests.

---

## 11. Phase 4b Completion Report Template

**When Phase 4b is done, create:**
`docs/ring4_phase4b_completion_report.md`

**Sections:**
1. Implementation summary
2. Test results (all sanity tests passing)
3. Determinism verification results
4. Code statistics (lines added, files created)
5. Known limitations
6. Next steps (Phase 4c preview)

**Tag:** `v1.4.1-ring4-phase4b-framework`

---

## Summary

**Phase 4b builds the testing substrate for Ring 4 without asserting any properties.**

**Core Principle:** Framework is dumb, properties are smart.

**Deliverables:**
- Core abstractions (Action, State, Event, Trace)
- Deterministic simulator
- Scenario generator
- Crash/restart injection
- Test harness utilities

**Success Criteria:**
- Framework self-tests pass
- Determinism proven
- NO property assertions

**Next Phase:** 4c (Correctness Properties)

**Estimated Effort:** 16-23 hours

---

**Document Status:** ✅ DESIGN COMPLETE

**Next Action:** Review this plan, then implement in phases 4c-4g will use this framework.

---

_Ring 4 Phase 4b Design — Complete_
_Implementation can proceed when approved_
