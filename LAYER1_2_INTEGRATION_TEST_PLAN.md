# Layer 1-2 Integration Test Plan

**Status:** 📋 PLANNING
**Goal:** Test REAL Layer 1-2 implementation (not concepts)
**Estimated Time:** 2-3 days

---

## What We Proved (Torture Tests)

✅ **Concepts are correct:**
- DisconnectBlock logic restores UTXO state exactly
- RollbackToFork handles arbitrary depths
- Value conservation invariants hold
- 1000 block reorg works conceptually

---

## What We Need to Prove (Integration Tests)

❌ **Real implementation works:**
- Real `DisconnectBlock()` in `src/p2p/state_transition.cpp`
- Real `RollbackToFork()` in ActivateBestChain
- Real `UTXOSet` with ChainDB backing
- Real `ReorgGuard` atomic commits
- Real undo data persistence
- Real block loading from disk

---

## Gap Analysis: Concepts vs. Implementation

| Aspect | Torture Tests (Concepts) | Integration Tests (Real) |
|--------|--------------------------|--------------------------|
| UTXO storage | In-memory map | RocksDB-backed UTXOSet |
| Block data | Simplified OutPoints | Real Block objects |
| Undo data | Not tested | Real undo serialization |
| Persistence | Not tested | ChainDB commits |
| Crash recovery | Not tested | RocksDB WriteBatch |
| Performance | <1ms (mocks) | Real disk I/O |

**Conclusion:** We've tested the LOGIC. We haven't tested the PLUMBING.

---

## Test Categories

### Category 1: Real DisconnectBlock with Real Blocks

**Goal:** Verify real `DisconnectBlock()` works with real block data.

**Setup:**
```cpp
// 1. Create test ChainDB (temp directory)
// 2. Create test BlockStorage (temp directory)
// 3. Build test block with real transactions
// 4. Write block to storage
// 5. Call real ConnectBlock (create undo data)
// 6. Call real DisconnectBlock
// 7. Verify UTXO state matches pre-connect
```

**Test Cases:**
1. **Simple Block:** 1 coinbase + 1 regular tx
2. **Multi-TX Block:** Coinbase + 10 regular txs
3. **Chain of Blocks:** 10 blocks, disconnect all
4. **Large Block:** Maximum size allowed

**Success Criteria:**
- UTXO set exactly matches snapshot before ConnectBlock
- Undo data is correctly read from disk
- No RocksDB errors
- No memory leaks

**Files to Test:**
- `src/p2p/state_transition.cpp::DisconnectBlock`
- `src/consensus/utxo_set.cpp`
- `src/storage/chain_db.cpp`

---

### Category 2: Real RollbackToFork with Real Chain

**Goal:** Verify real `RollbackToFork()` handles reorgs correctly.

**Setup:**
```cpp
// 1. Build main chain: Genesis → A → B → C (3 blocks)
// 2. Build fork:       Genesis → A → D → E (2 blocks from fork)
// 3. Call RollbackToFork(fork_point = A, new_tip = E)
// 4. Verify:
//    - Blocks B, C disconnected
//    - UTXO state at block A
//    - Undo data preserved
```

**Test Cases:**
1. **1-block reorg:** Disconnect 1 block
2. **10-block reorg:** Disconnect 10 blocks
3. **100-block reorg:** Stress test
4. **Fork at genesis:** Disconnect entire chain

**Success Criteria:**
- All blocks disconnected in reverse order (C → B)
- UTXO state matches fork point snapshot
- No partial disconnects (all-or-nothing)
- Block index updated correctly

**Files to Test:**
- `src/consensus/block_validation.cpp` (or wherever RollbackToFork is)
- `src/p2p/state_transition.cpp::DisconnectBlock`
- `src/consensus/utxo_set.cpp`

---

### Category 3: Real UTXOSet Persistence

**Goal:** Verify real `UTXOSet` persists to ChainDB correctly.

**Setup:**
```cpp
// 1. Create UTXOSet with test ChainDB
// 2. Add 1000 UTXOs
// 3. Flush to disk
// 4. Destroy UTXOSet
// 5. Create new UTXOSet
// 6. LoadFromDB()
// 7. Verify all 1000 UTXOs are restored
```

**Test Cases:**
1. **Add + Flush + Load:** Verify persistence
2. **Spend + Flush + Load:** Verify dirty tracking
3. **Large UTXO set:** 100,000 UTXOs
4. **Crash before flush:** Verify dirty changes discarded

**Success Criteria:**
- All UTXOs persisted correctly
- Dirty tracking works (added/removed coins)
- LoadFromDB() restores exact state
- No ChainDB corruption

**Files to Test:**
- `src/consensus/utxo_set.cpp::Flush`
- `src/consensus/utxo_set.cpp::LoadFromDB`
- `src/storage/chain_db.cpp`

---

### Category 4: Real ReorgGuard Atomic Commits

**Goal:** Verify real `ReorgGuard` ensures atomic reorgs.

**Setup:**
```cpp
// 1. Create ReorgGuard with UTXOSet
// 2. Perform reorg (disconnect 10 blocks)
// 3. Verify guard flushes:
//    - UTXO changes
//    - Tip update
//    - Block index updates
// 4. All in single RocksDB WriteBatch
```

**Test Cases:**
1. **Successful reorg:** Guard commits atomically
2. **Failed reorg:** Guard doesn't commit
3. **Simulated crash:** Verify no partial state
4. **RocksDB failure:** Verify std::terminate

**Success Criteria:**
- All state changes in single WriteBatch
- Commit is atomic (all-or-nothing)
- Crash before commit → old state preserved
- Crash during commit → RocksDB guarantees apply

**Files to Test:**
- `include/consensus/reorg_guard.h`
- `src/consensus/utxo_set.cpp::Flush`
- `src/storage/chain_db.cpp`

---

### Category 5: Crash Recovery Scenarios

**Goal:** Verify crash recovery works correctly.

**Setup:**
```cpp
// 1. Start reorg
// 2. Disconnect 5 blocks
// 3. Simulate crash (exit without flush)
// 4. Restart
// 5. Verify:
//    - UTXO state matches last committed state
//    - No partial disconnects
//    - Chain tip is consistent
```

**Test Cases:**
1. **Crash before commit:** Old state preserved
2. **Crash during commit:** RocksDB handles it
3. **Crash after commit:** New state persisted
4. **Multiple crashes:** Verify robustness

**Success Criteria:**
- No UTXO corruption
- No orphaned undo data
- Chain tip consistent with UTXO best block
- Can resume normal operation

**Files to Test:**
- All Layer 1-2 components
- RocksDB WriteBatch behavior
- Startup verification (VerifyChainstateOnStartup)

---

## Test Infrastructure Needed

### 1. Test ChainDB Factory

```cpp
class TestChainDBFactory {
public:
    static std::unique_ptr<ChainDB> Create(const std::string& test_name) {
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / test_name;
        std::filesystem::create_directories(temp_dir);
        return std::make_unique<ChainDB>(temp_dir.string());
    }

    static void Cleanup(const std::string& test_name) {
        std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / test_name;
        std::filesystem::remove_all(temp_dir);
    }
};
```

### 2. Test Block Builder

```cpp
class TestBlockBuilder {
public:
    Block BuildGenesisBlock();
    Block BuildBlock(const Block& prev, std::vector<Transaction> txs);
    Transaction BuildCoinbase(uint64_t value, const std::string& address);
    Transaction BuildSpend(const OutPoint& input, uint64_t value, const std::string& address);
};
```

### 3. UTXO Snapshot Helper

```cpp
class UTXOSnapshotHelper {
public:
    static std::map<OutPoint, TxOut> TakeSnapshot(const UTXOSet& utxo_set);
    static bool CompareSnapshots(const std::map<OutPoint, TxOut>& a,
                                  const std::map<OutPoint, TxOut>& b);
    static void PrintDiff(const std::map<OutPoint, TxOut>& before,
                          const std::map<OutPoint, TxOut>& after);
};
```

### 4. Test Result Reporter

```cpp
struct IntegrationTestResult {
    std::string test_name;
    bool passed;
    std::string error_message;
    std::chrono::milliseconds duration;

    void Print() const;
};

class IntegrationTestRunner {
public:
    void RunTest(const std::string& name, std::function<bool()> test_func);
    void PrintSummary();
private:
    std::vector<IntegrationTestResult> results_;
};
```

---

## Implementation Plan

### Phase 1: Test Infrastructure (1 day)

1. Create TestChainDBFactory
2. Create TestBlockBuilder
3. Create UTXOSnapshotHelper
4. Create IntegrationTestRunner

### Phase 2: Basic Integration Tests (1 day)

1. Test real DisconnectBlock (simple case)
2. Test real ConnectBlock → DisconnectBlock (round trip)
3. Test real UTXOSet persistence (basic)

### Phase 3: Advanced Integration Tests (1 day)

1. Test real RollbackToFork (10 blocks)
2. Test real ReorgGuard (atomic commits)
3. Test crash recovery (simulated)

### Phase 4: Edge Cases & Stress (½ day)

1. Test 100-block reorg
2. Test empty blocks
3. Test coinbase-only blocks
4. Test maximum-size blocks

---

## Success Criteria (Overall)

✅ **All integration tests pass**
- No UTXO corruption
- No partial state after crashes
- Performance is acceptable (<100ms per block)

✅ **Real implementation matches concepts**
- DisconnectBlock restores state exactly
- RollbackToFork handles arbitrary depths
- Value conservation holds

✅ **Crash recovery works**
- All-or-nothing commits
- No orphaned data
- Startup verification catches issues

---

## Risk Assessment

### High Risk Areas

1. **Undo Data Serialization**
   - Risk: Corruption during write/read
   - Mitigation: Checksums, version markers

2. **RocksDB WriteBatch**
   - Risk: Partial commits on crash
   - Mitigation: RocksDB guarantees (test them!)

3. **UTXO Dirty Tracking**
   - Risk: Missing added/removed coins
   - Mitigation: Extensive logging, snapshot comparison

4. **Block Index Consistency**
   - Risk: BlockIndex out of sync with UTXO
   - Mitigation: Startup verification checks

---

## Next Steps

**Option 1: Build Full Test Suite** (~2-3 days)
- Implement all test infrastructure
- Run all test categories
- Fix bugs found
- Document results

**Option 2: Quick Smoke Test** (~4 hours)
- Build minimal test (DisconnectBlock only)
- Test with 1 real block
- Verify basic correctness
- Defer comprehensive testing

**Option 3: Manual Testing** (~1 day)
- Build test blocks manually
- Run through scenarios by hand
- Less systematic, faster

---

## Recommendation

**Build the full test suite** (Option 1).

**Why:**
- We're about to integrate AssumeUTXO (high stakes)
- Bugs in Layer 1-2 will cause AssumeUTXO failures
- Systematic testing finds edge cases
- Test suite is reusable for future changes

**Timeline:**
- Day 1: Test infrastructure
- Day 2: Basic integration tests
- Day 3: Advanced tests + stress
- **Total:** 3 days

**After this:** Fill AssumeUTXO TODOs with **confidence** that the spine is proven.

---

**This is the disciplined path.**
**Prove the plumbing before hanging more weight on it.**

---

**Document Date:** December 19, 2025
**Author:** Claude Sonnet 4.5
**Status:** 📋 PLAN READY FOR EXECUTION
