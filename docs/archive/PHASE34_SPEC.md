# Phase 34: Mempool → Block Assembly Integration

## Executive Summary

**Purpose**: Connect mempool to block assembly so mined blocks include real transactions.

**Current State**: Wallet creates valid transactions → Mempool accepts them → **Mining ignores them**

**Target State**: Wallet creates valid transactions → Mempool accepts them → **Mining includes them**

**Classification**: Pure integration work (no consensus changes, no wallet refactoring)

---

## Entry Criteria (What Must Already Work)

Before starting Phase 34, these systems must be functional:

### ✅ Already Complete (Verified)

1. **Wallet Transaction Construction**
   - `wallet.sendtoaddress` creates valid transactions
   - Coin selection works (CoinSelector engine)
   - Taproot signing works (BIP341 key-path)
   - Fee calculation works

2. **Mempool Acceptance**
   - Mempool accepts wallet-created transactions
   - Transactions visible via `getrawmempool`
   - Basic validation rules enforced

3. **Mining Primitives**
   - `mining.generatetoaddress` can mine blocks
   - Coinbase transactions created correctly
   - Block headers valid

4. **Phase 1 Frozen Core**
   - scriptPubKey-based ownership locked
   - Consensus constants locked
   - All 73 core assertions passing

### ❌ Known Gap (This Is What Phase 34 Fixes)

- **Block assembler does not consume mempool transactions**
- Mined blocks contain only coinbase (empty of real txs)
- Wallet transactions never confirm

---

## Exit Criteria (What Phase 34 Delivers)

### Functional Requirements

1. **Block Assembly from Mempool**
   - `mining.generatetoaddress` creates blocks with mempool transactions
   - Transactions ordered correctly (parents before children)
   - Block weight limits respected
   - Fee-based transaction selection

2. **Transaction Confirmation**
   - Wallet transactions move from mempool → block
   - Mempool cleared after block accepted
   - Wallet balances update after confirmation
   - `gettransaction` shows confirmation count

3. **Coinbase Correctness**
   - Coinbase includes subsidy + fees
   - Fee calculation sums all tx fees in block
   - No off-by-one errors in fee accounting

4. **RPC Truthfulness**
   - `wallet.sendtoaddress` response removes "pending Phase 34" note
   - Mining RPCs return accurate block templates
   - Mempool size decreases after mining

### Test Requirements (New)

All of these tests must pass:

1. **test_block_assembly_mempool.sh** (NEW)
   - Assertions: 15+
   - Tests:
     - Empty mempool → coinbase-only block
     - 1 tx in mempool → block with coinbase + 1 tx
     - Multiple txs → all included (up to weight limit)
     - Parent/child txs → correct ordering
     - Fee calculation → coinbase amount correct

2. **test_rpc_spending_integration.sh** (EXISTING - Must Now Pass)
   - Current: 10/13 passing (3 failures expected)
   - Target: 13/13 passing
   - Critical: Test 11 (tx confirmation after mining) must pass

3. **test_wallet_confirmation_flow.sh** (NEW)
   - Assertions: 10+
   - Tests:
     - Send tx → 0 confirmations
     - Mine block → 1 confirmation
     - Mine 2nd block → 2 confirmations
     - Balance updates correctly
     - UTXO set reflects confirmation state

### Non-Functional Requirements

1. **Performance**
   - Block assembly completes in <100ms for typical mempool
   - No memory leaks in assembly loop

2. **Correctness**
   - All Phase 1 tests still pass (73 assertions)
   - No consensus rule violations
   - No scriptPubKey/ownership changes

---

## Scope Definition

### ✅ IN SCOPE (What Phase 34 Includes)

1. **BlockAssembler Integration**
   - File: `src/mining/block_assembler.cpp` (likely exists)
   - Change: Add mempool transaction selection
   - Logic: Pull transactions from mempool, validate, order, insert

2. **Mempool Query Interface**
   - File: `src/mempool/mempool.cpp`
   - Change: Add `GetTransactionsForBlock()` method
   - Logic: Return sorted, validated transactions ready for block

3. **Fee Calculation**
   - File: `src/consensus/subsidy.cpp` or block assembler
   - Change: Sum transaction fees, add to coinbase
   - Logic: `total_fees = sum(tx.input_value - tx.output_value)`

4. **Mempool State Management**
   - File: `src/mempool/mempool.cpp`
   - Change: Remove transactions after block acceptance
   - Logic: `RemoveBlockTransactions(block)`

5. **Mining RPC Wiring**
   - File: `src/rpc/mining_rpc.cpp` (or equivalent)
   - Change: Wire `generatetoaddress` to use mempool-aware assembler
   - Logic: Call BlockAssembler instead of coinbase-only path

6. **Test Infrastructure**
   - Files: New test scripts in `tests/wallet_tests/`
   - Change: Add block assembly integration tests
   - Logic: End-to-end wallet → mempool → block → confirmation

### ❌ OUT OF SCOPE (What Phase 34 Does NOT Include)

1. **No Wallet Changes**
   - `wallet_manager.cpp`: untouched
   - Coin selection: untouched
   - Signing: untouched
   - Ownership: untouched

2. **No Consensus Changes**
   - Subsidy rules: unchanged
   - Block validation: unchanged (uses existing)
   - Transaction validation: unchanged (uses existing)

3. **No Mempool Policy Changes**
   - Fee rate minimums: unchanged
   - RBF rules: unchanged
   - Descendant limits: unchanged

4. **No P2P Changes**
   - Block propagation: unchanged
   - Transaction relay: unchanged
   - Peer management: unchanged

5. **No Mining Algorithm Changes**
   - PoW: unchanged
   - Difficulty adjustment: unchanged
   - Block template: extended (not changed)

6. **No RPC Feature Additions**
   - Only fix existing RPCs to work correctly
   - No new RPC methods
   - No API changes beyond removing "pending" notes

---

## Files to Touch (Minimal Surface Area)

### Primary Changes (Core Integration)

1. **`src/mining/block_assembler.cpp`** (~150 lines added)
   - Add: `AssembleBlockWithMempool()`
   - Change: Existing coinbase-only path → mempool-aware path
   - Logic: Transaction selection, ordering, fee summing

2. **`src/mempool/mempool.cpp`** (~80 lines added)
   - Add: `GetTransactionsForBlock(max_weight, max_sigops)`
   - Add: `RemoveBlockTransactions(block)`
   - Logic: Query sorted mempool, dependency resolution

3. **`src/rpc/mining_rpc.cpp`** (~30 lines changed)
   - Change: `generatetoaddress` implementation
   - Before: Coinbase-only
   - After: Call BlockAssembler with mempool

4. **`src/consensus/subsidy.cpp`** (~20 lines changed)
   - Add: Fee calculation in `GetBlockSubsidy()` or caller
   - Logic: Subsidy + fees → coinbase output value

### Secondary Changes (Wiring)

5. **`src/rpc/methods_wallet_context.cpp`** (~5 lines changed)
   - Remove: `"note": "pending Phase 34"` from `sendtoaddress` response
   - No other changes

6. **`tests/wallet_tests/test_block_assembly_mempool.sh`** (NEW, ~250 lines)
   - Add: Comprehensive block assembly test suite

7. **`tests/wallet_tests/test_wallet_confirmation_flow.sh`** (NEW, ~180 lines)
   - Add: Wallet confirmation lifecycle tests

### No Changes Required

- ❌ `wallet_manager.cpp` - frozen
- ❌ `coin_selection.cpp` - frozen
- ❌ `hd_wallet.cpp` - frozen
- ❌ Any consensus validation files (already correct)
- ❌ Any scriptPubKey ownership files (frozen in Phase 1)

**Total Estimated Changes**: ~500-700 lines across 7 files (4 modified, 3 new)

---

## Implementation Plan (Step-by-Step)

### Step 1: Mempool Query Interface (~2 hours)

**File**: `src/mempool/mempool.cpp`

**Add Method**:
```cpp
std::vector<MempoolEntry> Mempool::GetTransactionsForBlock(
    size_t max_weight,
    size_t max_sigops
) {
    // 1. Get all mempool entries
    // 2. Sort by fee rate (descending)
    // 3. Topological sort (parents before children)
    // 4. Filter by weight/sigops limits
    // 5. Return ordered list
}
```

**Test**: Unit test that verifies ordering and limits

### Step 2: Block Assembler Integration (~3 hours)

**File**: `src/mining/block_assembler.cpp`

**Add Method**:
```cpp
std::unique_ptr<Block> BlockAssembler::AssembleBlockWithMempool(
    const std::string& coinbase_address,
    Mempool& mempool
) {
    // 1. Create coinbase transaction (existing code)
    // 2. Get mempool transactions
    // 3. Calculate total fees
    // 4. Update coinbase output value (subsidy + fees)
    // 5. Add transactions to block
    // 6. Validate block (existing validation)
    // 7. Return block
}
```

**Test**: Unit test with mock mempool

### Step 3: Fee Calculation (~1 hour)

**File**: `src/consensus/subsidy.cpp` or block assembler

**Add Helper**:
```cpp
uint64_t CalculateBlockFees(const std::vector<Transaction>& txs) {
    uint64_t total_fees = 0;
    for (const auto& tx : txs) {
        // Skip coinbase
        if (tx.IsCoinbase()) continue;

        // Fee = sum(inputs) - sum(outputs)
        uint64_t input_value = GetInputValue(tx);
        uint64_t output_value = GetOutputValue(tx);
        total_fees += (input_value - output_value);
    }
    return total_fees;
}
```

**Test**: Unit test with known fee values

### Step 4: Mining RPC Wiring (~2 hours)

**File**: `src/rpc/mining_rpc.cpp`

**Change**:
```cpp
// BEFORE (coinbase-only)
auto block = BlockAssembler::CreateCoinbaseBlock(address);

// AFTER (mempool-aware)
auto block = BlockAssembler::AssembleBlockWithMempool(address, mempool);
```

**Test**: Integration test via RPC call

### Step 5: Mempool State Management (~1 hour)

**File**: `src/mempool/mempool.cpp`

**Add Method**:
```cpp
void Mempool::RemoveBlockTransactions(const Block& block) {
    for (const auto& tx : block.transactions) {
        if (tx.IsCoinbase()) continue;
        Remove(tx.GetTxid());
    }
}
```

**Hook**: Call from block acceptance path

**Test**: Verify mempool size decreases

### Step 6: Remove "Pending Phase 34" Note (~5 minutes)

**File**: `src/rpc/methods_wallet_context.cpp`

**Change**:
```cpp
// DELETE THIS LINE:
result["note"] = "Transaction signing/broadcast pending Phase 34 integration";
```

**Test**: Verify RPC response clean

### Step 7: Integration Testing (~3 hours)

**Create**: `tests/wallet_tests/test_block_assembly_mempool.sh`

**Tests**:
1. Empty mempool → coinbase-only block
2. 1 tx in mempool → block with 1 tx
3. 10 txs in mempool → block with 10 txs
4. Parent + child tx → correct order
5. Fees calculated correctly
6. Mempool cleared after block

**Create**: `tests/wallet_tests/test_wallet_confirmation_flow.sh`

**Tests**:
1. Send tx → 0 confirmations
2. Mine 1 block → 1 confirmation
3. Mine 2nd block → 2 confirmations
4. Balance updates match expected
5. UTXO set correct

### Step 8: Verify All Tests Pass (~1 hour)

**Run**:
1. All Phase 1 tests (73 assertions) - must still pass
2. `test_rpc_spending_integration.sh` - now 13/13 passing
3. New Phase 34 tests - all passing

---

## Success Markers (How We Know We're Done)

### Primary Markers

1. ✅ **`test_rpc_spending_integration.sh` goes from 10/13 → 13/13**
   - Test 11 (tx confirmation) now passes
   - No more "transaction still in mempool" errors

2. ✅ **RPC response is clean**
   - `wallet.sendtoaddress` no longer says "pending Phase 34"
   - Response shows actual confirmation status

3. ✅ **Mempool dynamics work**
   - Transaction enters mempool: `getrawmempool` shows it
   - Mine block: `getrawmempool` no longer shows it
   - Transaction confirmed: `gettransaction` shows conf count

### Secondary Markers

4. ✅ **Coinbase amounts correct**
   - Empty block: coinbase = subsidy only
   - Block with txs: coinbase = subsidy + fees
   - Fee calculation matches expected

5. ✅ **All Phase 1 tests still pass**
   - 73 core assertions unchanged
   - No regressions in frozen wallet logic

6. ✅ **New Phase 34 tests pass**
   - Block assembly tests: 15+ assertions
   - Wallet confirmation tests: 10+ assertions

---

## Risk Assessment

### Risk Level: **LOW-MEDIUM**

**Why Low Risk:**
- ✅ No consensus changes
- ✅ No wallet refactoring
- ✅ Uses existing validation logic
- ✅ Clear integration points

**Why Medium Risk:**
- ⚠️ Block assembly is consensus-adjacent
- ⚠️ Fee calculation must be exact
- ⚠️ Transaction ordering must be correct
- ⚠️ Mempool state management is stateful

### Mitigation Strategies

1. **Use Existing Bitcoin Core Patterns**
   - Study Bitcoin Core's `BlockAssembler`
   - Copy fee calculation logic exactly
   - Use topological sort for dependencies

2. **Test Extensively**
   - Unit tests for each component
   - Integration tests for full flow
   - Edge cases (empty mempool, full blocks, conflicts)

3. **Incremental Implementation**
   - Step 1-3: Pure logic (no RPC changes yet)
   - Step 4-5: Wire to RPC (reversible)
   - Step 6-7: Polish and test

4. **Rollback Plan**
   - All changes in one commit
   - Tag: `phase34-block-assembly-complete`
   - Can revert to `phase1-coin-selection-boundary-complete` if needed

---

## Commit Strategy

### Single Commit (Recommended)

**Commit Message**:
```
phase34: integrate mempool into block assembly

Problem:
- Mining creates coinbase-only blocks
- Wallet transactions never confirm
- Mempool is ignored by block assembler

Solution:
- BlockAssembler now queries mempool for transactions
- Transactions ordered by fee rate + dependencies
- Coinbase includes subsidy + fees
- Mempool cleared after block acceptance

Impact:
- wallet.sendtoaddress transactions now confirm
- test_rpc_spending_integration.sh: 10/13 → 13/13 passing
- New tests: test_block_assembly_mempool.sh (15 assertions)
- New tests: test_wallet_confirmation_flow.sh (10 assertions)

All Phase 1 invariants unchanged (73 assertions still passing).

🧊 Generated with [Claude Code](https://claude.com/claude-code)

Co-Authored-By: Claude Sonnet 4.5 <noreply@anthropic.com>
```

**Tag**:
```
phase34-block-assembly-complete

Mempool → block assembly integration complete.
Wallet transactions now confirm in mined blocks.

Verified:
✅ All Phase 1 tests passing (73 assertions)
✅ All Phase 34 tests passing (25+ assertions)
✅ test_rpc_spending_integration.sh: 13/13 passing
✅ Coinbase fee calculation correct
✅ Transaction ordering correct
```

---

## Dependencies on Other Systems

### Requires (Must Already Exist)

1. **Mempool Service**
   - `Mempool::AddTransaction()` - exists
   - `Mempool::GetAllTransactions()` - likely exists
   - `Mempool::Remove()` - likely exists

2. **Block Validation**
   - `ValidateBlock()` - exists
   - `CheckBlockWeight()` - exists
   - `CheckBlockSigOps()` - exists

3. **Transaction Validation**
   - `CheckTransaction()` - exists
   - `CheckTransactionInputs()` - exists

4. **Subsidy Calculation**
   - `GetBlockSubsidy(height)` - exists

### Provides (For Future Work)

1. **Full Node Functionality**
   - Enables future P2P work
   - Enables future mining optimization
   - Enables wallet UX improvements (confirmations visible)

2. **Testing Infrastructure**
   - Confirmation flow tests reusable
   - Block assembly tests reusable
   - Integration test patterns established

---

## Verification Checklist (Before Phase 34 Commit)

### Code Quality

- [ ] No wallet logic changes
- [ ] No consensus rule changes
- [ ] All new code has comments
- [ ] No magic numbers (use constants)
- [ ] No memory leaks (valgrind clean)

### Testing

- [ ] All Phase 1 tests pass (73 assertions)
- [ ] `test_rpc_spending_integration.sh`: 13/13 passing
- [ ] `test_block_assembly_mempool.sh`: 15+ assertions passing
- [ ] `test_wallet_confirmation_flow.sh`: 10+ assertions passing
- [ ] Manual test: send tx → mine → verify confirmation

### Integration

- [ ] Compilation successful
- [ ] No new warnings
- [ ] RPC responses clean (no "pending" notes)
- [ ] Mempool state correct after mining
- [ ] Coinbase amounts match expected

### Documentation

- [ ] This spec file updated with actual results
- [ ] Any deviations from plan documented
- [ ] Known limitations noted
- [ ] Tag created with summary

---

## Timeline Estimate

**Total**: ~12-15 hours of focused work

| Step | Description | Time |
|------|-------------|------|
| 1 | Mempool query interface | 2h |
| 2 | Block assembler integration | 3h |
| 3 | Fee calculation | 1h |
| 4 | Mining RPC wiring | 2h |
| 5 | Mempool state management | 1h |
| 6 | Remove "pending" note | 5min |
| 7 | Integration testing | 3h |
| 8 | Verify all tests pass | 1h |
| **Buffer** | Debugging & polish | 2-3h |

**Realistic**: 2-3 focused work sessions

---

## Post-Phase 34 World

### What Works After Phase 34

1. **Full transaction lifecycle**
   - Create → Sign → Broadcast → Confirm → Spend

2. **Mining rewards**
   - Miners earn subsidy + fees
   - Fee market can function

3. **Wallet UX**
   - Users see confirmations
   - Balances update correctly
   - UTXO set reflects reality

4. **Testing**
   - Integration tests meaningful
   - Can test multi-block scenarios
   - Can test reorgs (future)

### What Still Needs Work (Beyond Phase 34)

1. **P2P Networking**
   - Block propagation to peers
   - Transaction relay
   - Peer discovery

2. **Wallet Features**
   - UTXO consolidation
   - Coin control
   - Better fee estimation

3. **RPC Completeness**
   - `listunspent`
   - `getbalance` improvements
   - Transaction introspection

4. **Mining Optimization**
   - Better fee selection
   - RBF support
   - CPFP support

**None of these touch Phase 1 frozen core.**

---

## Final Note: Why This Is The Right Next Step

Phase 34 is the **keystone** that turns DineroCoin from:

❌ "A correct transaction constructor with a mempool"

Into:

✅ **"A functioning blockchain node"**

After Phase 34:
- Every subsystem works end-to-end
- The node can operate independently
- All future work builds on a complete foundation

**This is not optional. This is the next mandatory milestone.**
