# Phase F.7 Implementation Plan

**Phase**: F.7 - Wallet Persistence Implementation
**Status**: Planning
**Purpose**: Implement wallet persistence to satisfy F.6 test contract

---

## Executive Summary

Phase F.7 implements wallet persistence behavior to make F.6 tests pass.

**This is not new design work.** The behavior is already specified by invariants W.1–W.7 and tests T1–T11.

**This is mechanical implementation**: Make tests pass, one at a time, in order of dependency.

---

## Hard Rules (From F.6)

These constraints are non-negotiable:

1. **Wallet code must not be written without making these tests pass**
   - No implementation without validation
   - Tests already define correct behavior

2. **No release can claim wallet safety without executing this suite**
   - 9/10 P0 tests must pass for certification
   - Execution is mandatory

3. **No bugfix can redefine W.1–W.7 without a new invariant commit**
   - Invariants are frozen
   - Bug = violation of existing invariant

4. **No "just this once" exception is acceptable**
   - No emergency bypasses
   - No "we'll fix it later"

5. **If F.7 fails F.6 tests, the release is invalid**
   - Tests are objective truth
   - No debate, no exceptions

---

## Implementation Strategy

### Phased Approach (Minimal to Complete)

**Phase 1: Restart Safety (T1, T3)**
- Implement: `WalletManager::getBalance()`, `WalletManager::getUTXOCount()`
- Integrate: Wallet DB persistence (RocksDB)
- Validate: Balance unchanged after restart

**Phase 2: Determinism (T2, T10)**
- Implement: `WalletManager::rescan()`
- Validate: Rescan idempotency

**Phase 3: Scope Limitation (T11)**
- Implement: Mempool exclusion from wallet DB
- Validate: Unconfirmed tx not persisted

**Phase 4: Mining Integration (T7, T8, T9)**
- Integrate: MiningManager v2 (Phase F.5)
- Implement: Coinbase tracking, maturity
- Validate: Mining rewards appear, mature, disappear on reorg

**Phase 5: Reorg Safety (T5)**
- Integrate: ChainDB reorg callbacks
- Implement: Orphaned UTXO removal
- Validate: Wallet state matches canonical chain

---

## Implementation Order

### Why This Order

1. **T1, T3 first**: Simplest, no external dependencies
2. **T2, T10 second**: Builds on T1/T3 infrastructure
3. **T11 third**: Validates scope boundaries
4. **T7, T8, T9 fourth**: Requires mining integration (F.5)
5. **T5 last**: Requires ChainDB reorg machinery

**Principle**: Start simple, add complexity incrementally.

---

## Minimal API Surface

### WalletManager APIs (Required)

```cpp
class WalletManager {
public:
    // Phase 1 (T1, T3)
    uint64_t getBalance() const;
    size_t getUTXOCount() const;

    // Phase 2 (T2, T10)
    void rescan();

    // Phase 3 (T11)
    // No new API - validation only

    // Phase 4 (T7, T8, T9)
    void onCoinbaseReceived(const Transaction& coinbase, uint32_t height);
    void onBlockOrphaned(uint32_t height);
};
```

### ChainDB Integration (Required for T5, T7-T9)

```cpp
// Callback interface for wallet to receive chain events
class IChainObserver {
public:
    virtual void onBlockConnected(const Block& block) = 0;
    virtual void onBlockDisconnected(const Block& block) = 0;
};
```

---

## What Gets Implemented (Explicit)

### IN SCOPE

1. **Wallet Database Layer**
   - RocksDB integration for UTXO persistence
   - Atomic write operations
   - Key schema: address → UTXOs

2. **Balance Calculation**
   - Sum of confirmed UTXO values
   - Excludes immature coinbase
   - Excludes mempool transactions

3. **Rescan Logic**
   - Iterate blockchain from genesis
   - Rebuild wallet UTXO set
   - Idempotent operation

4. **Coinbase Tracking**
   - Mark coinbase UTXOs with height
   - Maturity check (100 confirmations)
   - Orphan handling on reorg

5. **Chain Event Handling**
   - Subscribe to block connect/disconnect
   - Update wallet state on reorg
   - Remove orphaned UTXOs

### OUT OF SCOPE (Explicitly Deferred)

1. **Wallet Features**
   - HD derivation
   - Multi-signature
   - Coin control
   - Address labeling

2. **Performance Optimization**
   - Database indexing
   - Caching strategies
   - Query optimization

3. **Encryption**
   - Wallet password
   - Key encryption
   - Secure storage

4. **Mempool Integration**
   - Pending transaction tracking
   - Unconfirmed balance
   - Transaction conflict resolution

5. **Lightning**
   - Channel state
   - Off-chain transactions
   - Any Lightning-related code

---

## Success Criteria

### Certification Requirements

**For v0.16.0-f6 (Full Wallet Persistence Certification)**:

- [ ] T1: Balance determinism after restart ✅ PASS
- [ ] T2: Balance determinism after rescan ✅ PASS
- [ ] T3: Restart with unchanged chain ✅ PASS
- [ ] T5: Chain reorg depth 1 ✅ PASS
- [ ] T7: Mining reward appears in wallet ✅ PASS
- [ ] T8: Mining reward matures correctly ✅ PASS
- [ ] T9: Orphaned mining reward disappears ✅ PASS
- [ ] T10: Rescan idempotency ✅ PASS
- [ ] T11: Mempool tx not persisted ✅ PASS

**Required**: 9/9 P0 tests passing (T4 deferred)

**No partial certification.** Either all tests pass or no release.

---

## Implementation Constraints

### Code Quality Standards

1. **No debug scaffolding in production code**
   - Remove all `std::cerr` debug output
   - No temporary validation checks
   - Clean production-ready code only

2. **No global variables**
   - Dependency injection only
   - State passed explicitly

3. **Thread safety**
   - Wallet DB access must be thread-safe
   - Use mutexes for shared state

4. **Error handling**
   - Proper exception handling
   - No silent failures
   - Clear error messages

### Testing Standards

1. **Every commit must compile**
   - No broken intermediate states
   - Build must pass at every step

2. **Tests run before commit**
   - Verify new tests pass
   - Ensure existing tests still pass

3. **No "TODO" comments in production code**
   - TODOs allowed in tests (mark integration points)
   - No TODOs in wallet implementation

---

## Risk Mitigation

### Known Risks

1. **Database Corruption**
   - Mitigation: Atomic writes, write-ahead logging
   - Testing: Crash consistency (T4, deferred)

2. **Reorg Edge Cases**
   - Mitigation: ChainDB integration testing
   - Testing: Deep reorg simulation

3. **Performance Degradation**
   - Mitigation: Defer optimization to future phase
   - Testing: None (out of scope)

### Unknowns

1. **ChainDB API Availability**
   - Status: API may need to be designed
   - Blocker: Yes, for T5, T7-T9
   - Plan: Define minimal API surface

2. **MiningManager v2 Integration**
   - Status: F.5 certified, API exists
   - Blocker: No, API is stable
   - Plan: Use existing mining hooks

---

## Milestones

### Milestone 1: Basic Persistence (T1, T3)
- **Goal**: Wallet state persists across restarts
- **APIs**: getBalance(), getUTXOCount()
- **Tests**: 2/9 passing

### Milestone 2: Rescan (T2, T10)
- **Goal**: Wallet can rebuild state from chain
- **APIs**: rescan()
- **Tests**: 4/9 passing

### Milestone 3: Scope Validation (T11)
- **Goal**: Only confirmed state persists
- **APIs**: None (validation only)
- **Tests**: 5/9 passing

### Milestone 4: Mining Integration (T7, T8, T9)
- **Goal**: Coinbase rewards tracked correctly
- **APIs**: onCoinbaseReceived(), maturity checks
- **Tests**: 8/9 passing

### Milestone 5: Reorg Safety (T5)
- **Goal**: Wallet handles chain reorgs
- **APIs**: onBlockDisconnected()
- **Tests**: 9/9 passing ✅ CERTIFICATION

---

## Timeline Philosophy

**This phase does NOT have a timeline.**

Per RELEASE_POLICY.md, phases are scope-based, not time-based.

F.7 is complete when:
- All in-scope items implemented
- 9/9 P0 tests passing
- No known correctness issues
- Code quality standards met

This could take 1 week or 3 months. The timeline is irrelevant.

---

## Comparison to F.5 and F.6

| Aspect | F.5 (Mining) | F.6 (Wallet Spec) | F.7 (Wallet Impl) |
|--------|--------------|-------------------|-------------------|
| Type | Implementation + Tests | Specification | Implementation |
| Tests Written | During phase | Before phase | Already exist ✅ |
| Tests Passing | 12/12 ✅ | 0/9 (framework only) | Target: 9/9 |
| Heroics Required | Yes (ODR bug) | No (planning only) | No (tests guide) |
| Scope Creep Risk | Medium | None (locked) | None (locked) |

**F.7 is easier than F.5** because tests already exist and scope is locked.

---

## What Makes F.7 Different

### Before F.6 (Hypothetical)

If wallet implementation happened without F.6:
- ❌ No clear success criteria
- ❌ Scope would creep
- ❌ Testing would be retroactive
- ❌ Bugs would hide as "edge cases"

### After F.6 (Actual)

With F.6 complete before F.7:
- ✅ Success criteria explicit (9/9 tests)
- ✅ Scope cannot creep (locked)
- ✅ Tests constrain implementation
- ✅ Bugs fail objective tests

**This is the power of specification-first development.**

---

## Next Steps

### Before Starting Implementation

1. **Review F.6 artifacts**
   - Re-read invariants W.1–W.7
   - Re-read test matrix T1–T11
   - Understand scope boundaries

2. **Design minimal APIs**
   - WalletManager interface
   - ChainDB observer interface
   - No over-engineering

3. **Set up development workflow**
   - Run tests frequently
   - Commit after each test passes
   - No batch commits

### During Implementation

1. **Start with T1 (simplest)**
   - Implement getBalance()
   - Persist to RocksDB
   - Verify restart safety

2. **Proceed in order**
   - T3, T2, T10, T11, T7, T8, T9, T5
   - One test at a time
   - No parallelization

3. **Stop when all tests pass**
   - 9/9 P0 passing = certification
   - No "one more feature"
   - Lock and release

---

## Approval Process

This implementation plan must be approved before coding begins.

**Approval means**:
- Implementation strategy is sound
- Scope boundaries are clear
- Success criteria are objective

**Scope changes require**:
- Updated plan document
- Re-approval before proceeding

---

**Document Version**: 1.0
**Last Updated**: 2025-12-29
**Maintained By**: DineroCoin Engineering Team
