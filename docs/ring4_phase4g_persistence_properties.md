# Ring 4 Phase 4g: Persistence Properties (MR1–MR5)

**Status:** Design Complete
**Phase:** 4g - Persistence Correctness
**Depends On:** Phase 4f (Determinism Properties)
**Precedes:** Phase 4h (Production Mining Integration)

---

## Executive Summary

Phase 4g introduces **persistence correctness properties** for mining state. This phase proves that mining state can be persisted and recovered correctly across crashes, restarts, and partial failures.

**Critical Boundary:**
- Phase 4g = Abstract persistence model (deterministic simulation)
- Phase 4h = Real persistence (RocksDB, SQLite, disk I/O)

Phase 4g does **NOT** touch real filesystems or production consensus code. All persistence is simulated deterministically.

---

## Problem Statement

### What We're Solving

Mining involves stateful computation that must survive:
- Process crashes
- Planned restarts
- Power failures
- Disk corruption
- Partial writes

**The Challenge:**
- State must be recoverable
- Recovery must be deterministic
- Partial persistence must be safe
- No state duplication after crash
- Persistence must not break determinism

### Why This Matters

**Without persistence correctness:**
- Miners lose work after crashes
- Duplicate subsidies after restart
- Non-deterministic recovery behavior
- Silent state corruption
- Unpredictable failure modes

**With persistence correctness:**
- All failure modes are understood
- Recovery is reproducible
- State is never duplicated
- Corruption is detected
- Determinism is preserved

---

## Persistence Model (Abstract)

### Persistence Boundaries

We define three state categories:

```
1. VOLATILE STATE
   - Never persisted
   - Lost on crash
   - Example: RNG internal state, in-flight work

2. PERSISTED STATE
   - Written to durable storage
   - Survives crash
   - Example: block height, last subsidy, template hash

3. RECONSTRUCTIBLE STATE
   - Derived from persisted state
   - Can be rebuilt after restart
   - Example: mining statistics, cache
```

### Persistence Operations

```cpp
// Abstract persistence interface (no real I/O)
struct PersistenceModel {
    // Checkpoint: save state snapshot
    void persist(const MiningState& state);

    // Recovery: load state snapshot
    MiningState recover();

    // Partial persist: simulate incomplete write
    void partialPersist(const MiningState& state, size_t bytes_written);

    // Corruption: simulate disk corruption
    void corruptPersisted(size_t offset, size_t length);
};
```

### Failure Modes

Phase 4g tests these failure scenarios:

1. **Clean Restart**: Full state persisted, clean recovery
2. **Crash Before Persist**: State lost, recover from last checkpoint
3. **Partial Persist**: Write interrupted mid-operation
4. **Corrupt Persist**: Persisted data corrupted on disk
5. **Multiple Crashes**: Repeated crash/restart cycles

---

## Properties (MR1–MR5)

### MR1: State Survives Restart Correctly

**Property:** If state S is persisted before crash, recovery yields S.

**Formal Definition:**
```
∀ state S, crash point C:
  IF persist(S) completes before C
  THEN recover() after C yields S
```

**What This Catches:**
- State not persisted correctly
- Recovery reading wrong data
- Stale state after restart

**Test Strategy:**
```cpp
1. Run mining scenario
2. Persist state at checkpoint
3. Crash simulator
4. Restart and recover
5. Verify recovered state == checkpointed state
```

---

### MR2: No State Duplication After Crash

**Property:** Recovery never duplicates persisted state.

**Formal Definition:**
```
∀ state S, crash point C:
  IF S contains unique identifier (height, subsidy)
  THEN recover() after C does NOT duplicate S
```

**What This Catches:**
- Duplicate block heights after restart
- Double-counting subsidies
- Replaying already-processed work

**Test Strategy:**
```cpp
1. Mine block at height H
2. Persist state
3. Crash simulator
4. Restart and continue
5. Verify next block is H+1, not H again
```

---

### MR3: Partial Persistence Recovers Safely

**Property:** Incomplete persist operations leave state in valid state.

**Formal Definition:**
```
∀ state S, partial write at offset O:
  recover() after partial persist yields EITHER:
    - Last complete checkpoint (safe rollback)
    - OR valid intermediate state
  BUT NEVER corrupted/invalid state
```

**What This Catches:**
- Torn writes leaving corrupt state
- Invalid state after incomplete persist
- Undetected corruption

**Test Strategy:**
```cpp
1. Begin persist operation
2. Interrupt at random byte offset
3. Crash simulator
4. Recover
5. Verify state is valid (checksum, consistency checks)
```

---

### MR4: Restart Converges to Valid State

**Property:** After any failure sequence, recovery eventually reaches valid state.

**Formal Definition:**
```
∀ failure sequence F (crashes, partial persists, corruptions):
  ∃ recovery attempt N where:
    recover_attempt(N) yields valid state
```

**What This Catches:**
- Infinite recovery loops
- Unrecoverable corruption
- Cascading failures

**Test Strategy:**
```cpp
1. Generate random failure sequence (10 crashes)
2. After each failure, attempt recovery
3. Verify system eventually stabilizes
4. Measure: attempts to convergence, max iterations
```

---

### MR5: Persistence Does Not Break Determinism

**Property:** Persist/recover preserves determinism from Phase 4f.

**Formal Definition:**
```
∀ seed S, persist/recover sequence P:
  trace(S, P) == trace(S, P')
  WHERE P and P' are same logical operations
```

**What This Catches:**
- Hidden entropy in persist/recover
- Non-deterministic recovery order
- Time-dependent persistence

**Test Strategy:**
```cpp
1. Run scenario with seed S, persist at event E
2. Crash and recover
3. Continue to completion
4. Repeat with same seed S, same persist point E
5. Verify traces are identical (MD1 from Phase 4f)
```

---

## Oracle Design: MiningPersistenceOracle

### Interface

```cpp
class MiningPersistenceOracle {
public:
    // Check MR1-MR5 properties
    std::vector<PersistenceViolation> check(
        const MiningTrace& pre_crash,
        const PersistedSnapshot& snapshot,
        const MiningTrace& post_recovery
    ) const;

private:
    // MR1: State survival
    bool checkStateSurvival(
        const PersistedSnapshot& persisted,
        const MiningState& recovered
    ) const;

    // MR2: No duplication
    bool checkNoDuplication(
        const MiningTrace& pre_crash,
        const MiningTrace& post_recovery
    ) const;

    // MR3: Partial persist safety
    bool checkPartialPersistSafety(
        const PersistedSnapshot& partial,
        const MiningState& recovered
    ) const;

    // MR4: Convergence
    bool checkConvergence(
        const std::vector<RecoveryAttempt>& attempts,
        size_t max_iterations
    ) const;

    // MR5: Determinism preservation
    bool checkDeterminismPreservation(
        const MiningTrace& trace1,
        const MiningTrace& trace2
    ) const;
};
```

### Violation Reporting

```cpp
struct PersistenceViolation {
    std::string property;        // "MR1", "MR2", etc.
    std::string message;         // Human-readable description
    uint64_t divergence_index;   // Where violation occurred

    // MR-specific context
    std::optional<std::string> expected_state;
    std::optional<std::string> actual_state;
    std::optional<size_t> recovery_attempts;
};
```

---

## Deterministic Persistence Simulation

### In-Memory Persistence Store

```cpp
class DeterministicPersistenceStore {
public:
    explicit DeterministicPersistenceStore(uint64_t seed);

    // Persist operations
    void persist(const MiningState& state);
    MiningState recover();

    // Failure injection
    void injectPartialWrite(size_t bytes_written);
    void injectCorruption(size_t offset, size_t length);
    void clearStore();  // Simulate disk wipe

    // Inspection
    bool hasCheckpoint() const;
    size_t getStoredBytes() const;
    bool isCorrupt() const;

private:
    uint64_t seed_;
    std::vector<uint8_t> storage_;  // Simulated disk
    bool corrupted_ = false;

    // Deterministic corruption simulation
    std::mt19937_64 corruption_rng_;
};
```

### Persistence in MiningSimulator

Extend `MiningSimulator` with persistence:

```cpp
class MiningSimulator {
public:
    // ... existing interface ...

    // Phase 4g: Persistence operations
    void setPersistenceStore(DeterministicPersistenceStore* store);
    void persistCheckpoint();
    void recoverFromCheckpoint();

private:
    DeterministicPersistenceStore* persist_store_ = nullptr;
};
```

---

## Test Structure

### Test Organization

```
tests/mining/properties/
├── mining_persistence_oracle.h         # Base oracle
├── mining_persistence_oracle.cpp       # Base oracle impl
├── mining_persistence_oracle_mr1.h     # MR1: State Survival
├── mining_persistence_oracle_mr1.cpp
├── test_mining_persistence_oracle_mr1.cpp  # 6 tests
├── mining_persistence_oracle_mr2.h     # MR2: No Duplication
├── mining_persistence_oracle_mr2.cpp
├── test_mining_persistence_oracle_mr2.cpp  # 6 tests
├── mining_persistence_oracle_mr3.h     # MR3: Partial Persist Safety
├── mining_persistence_oracle_mr3.cpp
├── test_mining_persistence_oracle_mr3.cpp  # 6 tests
├── mining_persistence_oracle_mr4.h     # MR4: Convergence
├── mining_persistence_oracle_mr4.cpp
├── test_mining_persistence_oracle_mr4.cpp  # 6 tests
├── mining_persistence_oracle_mr5.h     # MR5: Determinism Preservation
├── mining_persistence_oracle_mr5.cpp
└── test_mining_persistence_oracle_mr5.cpp  # 6 tests
```

**Total:** 30 persistence property tests (6 per property × 5 properties)

### Test Patterns

Each MR test file follows this pattern:

```cpp
void test_mr1_clean_restart() {
    MR1Oracle oracle;
    DeterministicPersistenceStore store(seed);

    // Run scenario
    MiningSimulator sim(seed);
    sim.setPersistenceStore(&store);

    // ... mine some blocks ...

    // Persist checkpoint
    sim.persistCheckpoint();
    MiningTrace pre_crash = sim.extractTrace();

    // Crash and recover
    MiningSimulator sim2(seed);
    sim2.setPersistenceStore(&store);
    sim2.recoverFromCheckpoint();
    MiningTrace post_recovery = sim2.extractTrace();

    // Verify MR1
    auto violations = oracle.check(pre_crash, store.getSnapshot(), post_recovery);
    assert_no_violations(violations, "MR1: Clean restart");
}
```

---

## What Phase 4g Does NOT Include

**Explicitly Out of Scope:**

❌ **Real Disk I/O**
- No `fopen`, `fwrite`, `fsync`
- No RocksDB or SQLite
- No filesystem operations

❌ **Production Mining Integration**
- No real `BlockAssembler`
- No real UTXO set
- No real consensus validation

❌ **Network Persistence**
- No syncing persisted state across nodes
- No distributed consensus on state

❌ **Performance Optimization**
- No WAL (write-ahead log)
- No compression
- No incremental checkpoints

**These are Phase 4h concerns.**

---

## Implementation Phases

### Phase 4g.1: Persistence Model Foundation

**Deliverables:**
1. `DeterministicPersistenceStore` class
2. Integration with `MiningSimulator`
3. Basic persist/recover operations
4. Framework self-tests (determinism, idempotence)

### Phase 4g.2: MR1-MR3 Properties

**Deliverables:**
1. MR1 oracle + 6 tests (state survival)
2. MR2 oracle + 6 tests (no duplication)
3. MR3 oracle + 6 tests (partial persist safety)

### Phase 4g.3: MR4-MR5 Properties

**Deliverables:**
1. MR4 oracle + 6 tests (convergence)
2. MR5 oracle + 6 tests (determinism preservation)

### Phase 4g.4: Integration and Verification

**Deliverables:**
1. All 30 tests passing
2. CMake integration
3. Documentation complete

---

## Success Criteria

Phase 4g is **complete** when:

✅ **All 30 persistence tests pass**
- MR1: State survival (6 tests)
- MR2: No duplication (6 tests)
- MR3: Partial persist safety (6 tests)
- MR4: Convergence (6 tests)
- MR5: Determinism preservation (6 tests)

✅ **Persistence model is deterministic**
- Same seed → same persist/recover behavior
- Reproducible failure injection
- No hidden entropy sources

✅ **Framework is ready for Phase 4h**
- Clean abstraction boundary
- Easy to replace simulation with real persistence
- Oracle interface supports production code

✅ **Documentation is complete**
- Properties formally defined
- Test coverage documented
- Known limitations identified

---

## Transition to Phase 4h

**Phase 4h will replace simulation with reality:**

| Phase 4g (Simulation) | Phase 4h (Production) |
|----------------------|----------------------|
| `DeterministicPersistenceStore` | RocksDB / SQLite |
| Abstract `MiningState` | Real mining state (templates, nonces, subsidies) |
| Simulated blocks | Real `BlockAssembler` output |
| Deterministic crashes | Real crash handling |
| In-memory storage | Disk I/O with fsync |

**The oracle stays the same.** MR1-MR5 properties remain unchanged.

---

## Risks and Mitigations

### Risk 1: Abstraction Mismatch

**Risk:** Simulated persistence doesn't match real persistence semantics.

**Mitigation:**
- Design simulation based on real persistence patterns
- Use byte-level storage simulation (mirrors disk)
- Model real failure modes (partial writes, corruption)

### Risk 2: Missing Failure Modes

**Risk:** Real crashes have failure modes we didn't simulate.

**Mitigation:**
- Study crash recovery literature (databases, filesystems)
- Test on real hardware in Phase 4h
- Add missing failure modes as discovered

### Risk 3: Persistence Breaks Determinism

**Risk:** Persist/recover operations introduce hidden entropy.

**Mitigation:**
- MR5 explicitly tests determinism preservation
- Use deterministic RNG for corruption simulation
- Verify with MD4 oracle from Phase 4f

---

## References

**Prior Art:**
- Database crash recovery (ARIES algorithm)
- Filesystem journaling (ext4, XFS)
- Bitcoin persistence model (LevelDB corruption handling)
- Phase 4f MD1-MD5 determinism properties

**Related Work:**
- Phase 4b: Mining test framework
- Phase 4f: Determinism properties (foundation for MR5)
- Phase 4h: Production persistence (next phase)

---

## Appendix: Property Summary Table

| Property | Short Name | What It Proves | Test Count |
|----------|-----------|---------------|------------|
| MR1 | State Survival | Persisted state recovers correctly | 6 |
| MR2 | No Duplication | No duplicate subsidies after crash | 6 |
| MR3 | Partial Safety | Incomplete persists leave valid state | 6 |
| MR4 | Convergence | Recovery eventually succeeds | 6 |
| MR5 | Determinism | Persistence preserves determinism | 6 |
| **Total** | | **All persistence correctness** | **30** |

---

**End of Phase 4g Design Document**

**Next Step:** Implement Phase 4g.1 (Persistence Model Foundation)
