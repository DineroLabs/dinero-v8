# Phase F.11: Reorg Safety Across Mining & Wallet

**Status**: Planning
**Priority**: P0 (Critical Path - Consensus Safety)
**Dependencies**: Phase F.10 (Mining/Block Assembly)

---

## Overview

Phase F.11 validates **blockchain reorganization safety** across the full mining and wallet stack. This phase tests that reorgs correctly unwind mined blocks, restore UTXO state, update wallet balances, and handle coinbase maturity transitions.

**Key Principle**: Reorgs MUST safely unwind all state changes without leaving the system in an inconsistent state. This is the final consensus-critical test before production.

**Gap Identified:**
- ✅ F.8 T16: Validated wallet handles reorg height changes
- ✅ F.10 T22: Validated BlockValidator rejects invalid blocks
- ❌ **Missing**: Reorg safety for mined blocks with BlockValidator.DisconnectBlock()

---

## Objectives

1. **Validate block disconnection**: DisconnectBlock() correctly reverses ConnectBlock()
2. **Validate UTXO restoration**: All inputs/outputs correctly restored from BlockUndo
3. **Validate wallet state rollback**: Balances and spendability reflect pre-reorg state
4. **Validate mempool repopulation**: Transactions from orphaned blocks return to mempool (or marked conflicted)
5. **Validate coinbase maturity regression**: Coinbase maturity flips correctly during reorg

---

## Invariants

### I.11.1: Block Disconnection Atomicity

**Invariant**:
> "DisconnectBlock() MUST completely reverse all state changes made by ConnectBlock(). After disconnection, the UTXO set MUST match the state before the block was connected."

**Validation**:
- Connect block with transactions
- Record UTXO set state before connection
- Disconnect block using BlockUndo
- Verify UTXO set matches pre-connection state
- Verify all spent coins restored, all created coins removed

### I.11.2: Wallet Balance Reorg Stability

**Invariant**:
> "When a block is disconnected, wallet balances MUST revert to the state before that block. Transactions in the disconnected block MUST be either returned to mempool or marked as conflicted."

**Validation**:
- Create wallet with transactions in block
- Record wallet balance before block connection
- Connect block (wallet balance changes)
- Disconnect block (reorg)
- Verify wallet balance matches pre-connection state
- Verify transaction status updated correctly

### I.11.3: Coinbase Maturity Reorg Transition

**Invariant**:
> "When a reorg moves the chain tip below a coinbase's maturity threshold (100 confirmations), that coinbase MUST become immature again. Transactions spending it MUST be rejected."

**Validation**:
- Mine coinbase at height H
- Advance chain to H + 100 (coinbase mature)
- Create transaction spending mature coinbase
- Reorg back to height H + 50 (coinbase immature)
- Verify coinbase spendability = false
- Verify mempool rejects spend transaction

### I.11.4: Mempool Repopulation After Reorg

**Invariant**:
> "Transactions from disconnected blocks MUST be re-validated against the new chain tip. Valid transactions MUST return to mempool; conflicted transactions MUST be marked accordingly."

**Validation**:
- Create block with valid transactions
- Connect block (transactions leave mempool)
- Disconnect block (reorg)
- Verify non-conflicted transactions return to mempool
- Verify conflicted transactions excluded
- Verify mempool state consistent with new chain tip

### I.11.5: Mining Continuity After Reorg

**Invariant**:
> "After a reorg, block assembly MUST use the new chain tip. Old block templates MUST be invalidated. Mining can resume immediately on the new tip."

**Validation**:
- Start block assembly on chain A
- Reorg to chain B
- Verify old block template invalid
- Verify new block assembly uses chain B tip
- Verify mining can proceed without errors

---

## Test Cases

### T23: Reorg Unwinds Mined Block (P0 - CRITICAL)

**Setup**:
1. Blockchain at height 200
2. Create 3 transactions spending mature coinbase
3. Mine block A at height 201 containing these transactions
4. Connect block A (UTXOs updated, wallet balances change)
5. Record UTXO set state, wallet balances, mempool state

**Execution**:
1. Call BlockValidator.DisconnectBlock(block A, height 201, undo)
2. Verify UTXO set restored:
   - All inputs spent in block A are restored
   - All outputs created in block A are removed
3. Verify wallet balances restored to pre-block state
4. Verify mempool repopulated (or transactions marked conflicted)
5. Verify blockchain tip = height 200

**Expected Results**:
- ✅ DisconnectBlock() succeeds
- ✅ UTXO set matches state before block A
- ✅ Wallet balances match pre-block state
- ✅ Mempool state consistent
- ✅ System ready for new block on height 201

**Invariants Validated**: I.11.1, I.11.2, I.11.4

---

### T24: Reorg + Coinbase Maturity Regression (P0 - CRITICAL)

**Setup**:
1. Blockchain at height 1
2. Mine coinbase at height 1 (50 DIN)
3. Advance chain to height 101 (coinbase MATURE - 101 confirmations)
4. Create transaction spending mature coinbase
5. Submit to mempool → ACCEPTED (coinbase is mature)

**Execution**:
1. Simulate reorg back to height 51
   - Disconnect blocks 101 → 52 using DisconnectBlock()
2. Verify coinbase at height 1 now has 51 confirmations (IMMATURE)
3. Re-submit spend transaction to mempool
4. Verify mempool REJECTS (coinbase immature)
5. Verify wallet marks coinbase as unspendable

**Expected Results**:
- ✅ Reorg successfully disconnects 50 blocks
- ✅ Coinbase confirmations = 51 (< 100 required)
- ✅ Coinbase spendability flips: mature → immature
- ✅ Mempool rejects spend transaction
- ✅ Wallet balance reflects unspendable coinbase

**Invariants Validated**: I.11.3, I.11.2

---

### T25: Reorg During Mining (P1 - RECOMMENDED)

**Setup**:
1. Blockchain at height 200
2. Mempool contains 10 transactions
3. Start block assembly for height 201
4. Block template created with mempool transactions

**Execution**:
1. Mid-assembly, simulate reorg:
   - New chain tip arrives at height 201 (different block)
2. Verify old block template invalidated
3. Request new block template
4. Verify new template uses updated chain tip
5. Verify new template re-validates mempool transactions

**Expected Results**:
- ✅ Old block template marked invalid
- ✅ New template generated successfully
- ✅ Mempool transactions re-validated against new tip
- ✅ Mining can proceed without errors

**Invariants Validated**: I.11.5

---

### T26: Multi-Block Reorg (P1 - RECOMMENDED)

**Setup**:
1. Blockchain at height 200
2. Mine chain A: blocks 201-205 (5 blocks)
3. Each block contains transactions
4. Connect all 5 blocks (chain tip = 205)

**Execution**:
1. Simulate reorg to chain B (fork at height 200)
2. Disconnect blocks 205 → 201 using DisconnectBlock()
3. Connect blocks 201'-205' from chain B
4. Verify UTXO set consistent
5. Verify wallet balances correct
6. Verify mempool state correct

**Expected Results**:
- ✅ All 5 blocks disconnected successfully
- ✅ UTXO set matches state at height 200
- ✅ Chain B blocks connect successfully
- ✅ Wallet state reflects chain B
- ✅ No UTXO leaks or double-spends

**Invariants Validated**: I.11.1, I.11.2, I.11.4

---

## Implementation Approach

### Test Architecture

**Pattern**: Following F.10 pattern - standalone executables with real consensus components.

**Files**:
- `tests/mining/standalone_test_t23.cpp` - Reorg unwinds mined block
- `tests/mining/standalone_test_t24.cpp` - Reorg + coinbase maturity regression
- `tests/mining/standalone_test_t25.cpp` - Reorg during mining (P1)
- `tests/mining/standalone_test_t26.cpp` - Multi-block reorg (P1)

**Priority**:
- **P0 (Required)**: T23, T24 (core reorg safety)
- **P1 (Recommended)**: T25, T26 (advanced scenarios)

### Components Needed

**BlockValidator APIs** (already exist):
```cpp
class BlockValidator {
public:
    // Connect block (already tested in F.10)
    bool ConnectBlock(const Block& block, uint32_t height,
                     const std::string& block_hash,
                     BlockUndo& undo, std::string& error);

    // Disconnect block (NEW - to be tested in F.11)
    bool DisconnectBlock(const Block& block, uint32_t height,
                        const BlockUndo& undo, std::string& error);
};
```

**BlockUndo Structure** (to be explored):
```cpp
// Stores information needed to reverse a block
struct BlockUndo {
    // Spent coins (to be restored on disconnect)
    std::vector<UTXOEntry> spent_coins;

    // Other undo information
    // ... (to be determined by exploration)
};
```

**Test Flow**:
1. **Setup**: Create blockchain, mine blocks, connect them
2. **Action**: Call DisconnectBlock() with BlockUndo data
3. **Validation**: Verify UTXO restoration, wallet state, mempool state
4. **Consensus Check**: Validate system consistency after reorg

### Testing Strategy

**T23 Workflow**:
1. Mine block with 3 transactions
2. ConnectBlock() → record BlockUndo
3. Verify block connected (UTXOs updated)
4. DisconnectBlock() using BlockUndo
5. Verify UTXO set restored
6. Verify wallet balances restored
7. Verify mempool repopulated

**T24 Workflow**:
1. Mine coinbase at height 1
2. Advance to height 101 (mature)
3. Create spend transaction → mempool accepts
4. Disconnect blocks 101 → 52 (50 blocks)
5. Verify coinbase immature (51 confirmations)
6. Re-submit spend → mempool rejects
7. Verify wallet marks coinbase unspendable

**Key Validations**:
- ✅ DisconnectBlock() reverses ConnectBlock()
- ✅ UTXO set consistency maintained
- ✅ Wallet state reflects current chain tip
- ✅ Mempool state consistent with chain tip
- ✅ Coinbase maturity follows chain height

---

## Success Criteria

**Phase F.11 is COMPLETE when**:
1. ✅ T23 passes (reorg unwinds mined block)
2. ✅ T24 passes (coinbase maturity regression)
3. ✅ All invariants (I.11.1 - I.11.5) validated
4. ✅ Documentation complete (test results, invariant validation)

**Optional (P1)**:
5. T25 passes (reorg during mining)
6. T26 passes (multi-block reorg)

---

## Dependencies

### From Phase F.10
- ✅ BlockValidator.ConnectBlock() validated
- ✅ Block assembly works correctly
- ✅ BlockValidator rejects invalid blocks

### Required for F.11
- BlockValidator.DisconnectBlock() implementation
- BlockUndo data structure
- Mempool repopulation logic
- Wallet reorg handling

### APIs to Verify/Implement

**Reorg Infrastructure**:
- Does DisconnectBlock() exist and work correctly?
- What does BlockUndo contain?
- How does mempool handle reorgs?
- How does wallet handle reorgs?

**Exploration Required**: Investigate BlockValidator, BlockUndo, and reorg handling.

---

## Risk Assessment

### Risks

1. **DisconnectBlock() may be incomplete**
   - Impact: Core reorg functionality missing
   - Mitigation: Tests will reveal gaps, document for fixes

2. **BlockUndo may not capture all state**
   - Impact: UTXO restoration incomplete
   - Mitigation: Validate UTXO set integrity thoroughly

3. **Mempool repopulation may be manual**
   - Impact: Tests may need to simulate mempool logic
   - Mitigation: Document expected behavior, test what exists

### Unknowns

- Does DisconnectBlock() exist?
- What data does BlockUndo store?
- Does mempool auto-repopulate after reorg?
- Does wallet auto-update after reorg?

**Approach**: Explore codebase first, then adapt test design to actual architecture.

---

## Exploration Phase

### Step 1: Search for Reorg Infrastructure

**Search Patterns**:
```bash
# Search for DisconnectBlock
rg -i "DisconnectBlock"

# Search for BlockUndo
rg -i "BlockUndo|block_undo"

# Search for reorg handling
rg -i "reorg|reorganize|reorganization"

# Search for mempool repopulation
rg -i "repopulate|revalidate.*mempool"
```

### Step 2: Identify Key Components

**Questions to Answer**:
1. Where is DisconnectBlock() implemented?
2. What does BlockUndo contain?
3. How are spent coins restored?
4. How does mempool handle reorgs?
5. How does wallet handle reorgs?

### Step 3: Design Tests Based on Findings

**Scenarios**:
- **If DisconnectBlock exists**: Test actual reorg functionality
- **If DisconnectBlock missing**: Document gap, design validation tests
- **If BlockUndo incomplete**: Test what exists, document gaps
- **If mempool manual**: Simulate expected behavior in tests

---

## Timeline

**Phase F.11 Estimated Effort**:
- Exploration: 1 session (locate reorg infrastructure)
- Test design: 1 session (adapt to actual architecture)
- T23 implementation: 2 sessions (complex - full reorg flow)
- T24 implementation: 1 session (coinbase maturity regression)
- Documentation: 1 session
- **Total**: ~6 sessions

**With optional tests (T25, T26)**:
- Additional: 2-3 sessions

---

## Related Phases

**Previous**: Phase F.10 - Mining/Block Assembly Spending Paths (certified)
**Current**: Phase F.11 - Reorg Safety Across Mining & Wallet
**Next**: Hostile Testing / Production Hardening

---

## References

- Phase F.8: Wallet Spending Rules - T16 (wallet reorg handling)
- Phase F.10: Mining/Block Assembly (certified)
- Bitcoin Core: `validation.cpp` (DisconnectBlock, ConnectBlock)
- Bitcoin Core: `undo.h` (BlockUndo structure)
- Consensus rules: Coinbase maturity (100 confirmations)

---

**Created**: 2025-12-29
**Status**: Planning - ready for exploration
**Next Step**: Explore codebase for reorg infrastructure (DisconnectBlock, BlockUndo)

