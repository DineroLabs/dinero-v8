# Phase F.10: Mining/Block Assembly Spending Paths

**Status**: Planning
**Priority**: P0 (Critical Path)
**Dependencies**: Phase F.9 (Wallet ↔ Mempool Interaction)

---

## Overview

Phase F.10 validates the **mining and block assembly workflow** from transaction selection in mempool through block creation and validation. This phase tests that block assembly respects all consensus rules, particularly coinbase maturity requirements and transaction validation.

**Key Principle**: Blocks MUST only include valid, mature transactions. The block assembly process is the **final enforcement point** before transactions are committed to the blockchain.

---

## Objectives

1. **Validate transaction selection from mempool**: Block assembly selects valid transactions
2. **Validate coinbase maturity enforcement**: Blocks reject transactions spending immature coinbase
3. **Validate block construction**: Blocks are properly formed with valid coinbase
4. **Validate block validation**: Assembled blocks pass consensus validation
5. **Validate fee calculation**: Coinbase reward correctly includes transaction fees

---

## Invariants

### I.10.1: Block Transaction Selection

**Invariant**:
> "Block assembly MUST only select transactions from mempool that pass consensus validation at current blockchain height. Transactions spending immature coinbase MUST be excluded."

**Validation**:
- Mempool contains mix of valid and invalid transactions
- Block assembly selects only valid transactions
- Transactions spending immature coinbase excluded from block
- Block contains only mature spends

### I.10.2: Coinbase Maturity in Assembled Blocks

**Invariant**:
> "Blocks MUST NOT include any transaction that spends a coinbase output with fewer than 100 confirmations, even if such transaction exists in mempool."

**Validation**:
- Create scenario where immature spend is in mempool (bypassed validation)
- Attempt block assembly
- Verify block does not include immature spend
- OR verify block assembly rejects immature spend

### I.10.3: Block Validation After Assembly

**Invariant**:
> "Every assembled block MUST pass full consensus validation before being considered valid. Invalid transactions MUST NOT be included."

**Validation**:
- Assemble block from mempool transactions
- Run full block validation
- Verify block passes validation
- Verify all transactions in block are valid

### I.10.4: Coinbase Reward Calculation

**Invariant**:
> "Coinbase transaction reward MUST equal block subsidy plus sum of transaction fees from included transactions."

**Validation**:
- Create block with multiple transactions
- Calculate total fees from included transactions
- Verify coinbase output = subsidy + fees
- Verify no inflation (reward not exceeded)

### I.10.5: Block Assembly Atomicity

**Invariant**:
> "Block assembly is atomic: either a valid block is produced, or assembly fails. Partial blocks MUST NOT be created."

**Validation**:
- Attempt block assembly with invalid transactions
- Verify assembly fails gracefully
- Verify no partial block state left behind

---

## Test Cases

### T20: Block Assembly with Mature Transactions

**Setup**:
1. Blockchain at height 201
2. Mempool contains 3 transactions:
   - Tx A: Spends coinbase from height 1 (201 confirmations - MATURE)
   - Tx B: Spends coinbase from height 100 (102 confirmations - MATURE)
   - Tx C: Regular transaction (non-coinbase spend)

**Execution**:
1. Call block assembly/mining function
2. Verify block created successfully
3. Verify block contains all 3 transactions
4. Verify coinbase reward = subsidy + (fee_A + fee_B + fee_C)
5. Verify block passes validation

**Expected Results**:
- ✅ Block assembled successfully
- ✅ All 3 mature transactions included
- ✅ Coinbase reward calculated correctly
- ✅ Block passes consensus validation

**Invariants Validated**: I.10.1, I.10.3, I.10.4

---

### T21: Block Assembly Excludes Immature Spend

**Setup**:
1. Blockchain at height 150
2. Mempool contains 3 transactions:
   - Tx A: Spends coinbase from height 1 (150 confirmations - MATURE)
   - Tx B: Spends coinbase from height 100 (51 confirmations - IMMATURE)
   - Tx C: Regular transaction (non-coinbase spend)

**Execution**:
1. Call block assembly/mining function
2. Verify block created
3. Verify block contains Tx A and Tx C only
4. Verify Tx B excluded (immature spend)
5. Verify coinbase reward = subsidy + (fee_A + fee_C), excludes fee_B
6. Verify block passes validation

**Expected Results**:
- ✅ Block assembled successfully
- ✅ Mature transactions (A, C) included
- ❌ Immature transaction (B) excluded
- ✅ Coinbase reward excludes fee from excluded transaction
- ✅ Block passes consensus validation

**Invariants Validated**: I.10.1, I.10.2, I.10.3, I.10.4

---

### T22: Block Validation Rejects Immature Spend

**Setup**:
1. Blockchain at height 150
2. Manually construct block containing:
   - Coinbase transaction (block reward)
   - Tx A: Spends coinbase from height 100 (51 confirmations - IMMATURE)

**Execution**:
1. Manually create block (bypass assembly)
2. Submit block to validation
3. Verify block validation fails
4. Verify error: "Transaction spends immature coinbase"

**Expected Results**:
- ❌ Block validation fails
- ✅ Error indicates immature coinbase spend
- ✅ Block not accepted to blockchain

**Invariants Validated**: I.10.3 (validation catches violations)

---

### T23: Block Assembly with Empty Mempool

**Setup**:
1. Blockchain at height 100
2. Empty mempool (no transactions)

**Execution**:
1. Call block assembly
2. Verify block created with only coinbase transaction
3. Verify coinbase reward = subsidy only (no fees)
4. Verify block passes validation

**Expected Results**:
- ✅ Block assembled successfully
- ✅ Block contains only coinbase transaction
- ✅ Coinbase reward = block subsidy (50 DIN)
- ✅ Block passes consensus validation

**Invariants Validated**: I.10.3, I.10.4, I.10.5

---

### T24: Block Assembly Fee Calculation

**Setup**:
1. Blockchain at height 200
2. Mempool contains 5 transactions:
   - Tx A: 0.01 DIN fee
   - Tx B: 0.02 DIN fee
   - Tx C: 0.005 DIN fee
   - Tx D: 0.1 DIN fee
   - Tx E: 0.001 DIN fee

**Execution**:
1. Call block assembly
2. Verify all transactions included
3. Calculate total fees: 0.01 + 0.02 + 0.005 + 0.1 + 0.001 = 0.136 DIN
4. Verify coinbase output = 50 DIN (subsidy) + 0.136 DIN (fees) = 50.136 DIN
5. Verify block passes validation

**Expected Results**:
- ✅ Block assembled successfully
- ✅ All 5 transactions included
- ✅ Coinbase reward = 50.136 DIN (exact calculation)
- ✅ Block passes consensus validation

**Invariants Validated**: I.10.4

---

## Implementation Approach

### Test Architecture

**Pattern**: Following F.9 pattern - standalone executables with real consensus components.

**Files**:
- `tests/mining/standalone_test_t20.cpp` - Block assembly with mature transactions
- `tests/mining/standalone_test_t21.cpp` - Block assembly excludes immature
- `tests/mining/standalone_test_t22.cpp` - Block validation rejects immature
- `tests/mining/standalone_test_t23.cpp` - Empty mempool block assembly
- `tests/mining/standalone_test_t24.cpp` - Fee calculation validation

**Priority**:
- **P0 (Required)**: T20, T21, T22 (core block assembly + validation)
- **P1 (Recommended)**: T23, T24 (edge cases + fee validation)

### Components Needed

**NOTE**: These components may not exist yet. Tests will validate requirements.

1. **Block Assembly/Mining API**:
   ```cpp
   // Potential API (to be validated)
   class BlockAssembler {
   public:
       BlockAssembler(Mempool& mempool, CoinsViewCache& view);

       // Assemble block from mempool transactions
       std::optional<Block> createBlock(
           const std::vector<uint8_t>& coinbase_script,
           uint32_t current_height
       );

       // Calculate total fees from transactions
       uint64_t calculateFees(const std::vector<Transaction>& txs);
   };
   ```

2. **Block Validation API**:
   ```cpp
   // Validate assembled block
   bool validateBlock(
       const Block& block,
       const CoinsViewCache& view,
       uint32_t current_height,
       uint64_t current_time
   );
   ```

3. **Mempool Transaction Selection**:
   ```cpp
   // Get transactions for block inclusion
   std::vector<Transaction> selectTransactions(
       Mempool& mempool,
       size_t max_block_size,
       const CoinsViewCache& view,
       uint32_t current_height
   );
   ```

### Testing Strategy

**Workflow**:
1. **Setup**: Create blockchain state (CoinsDB), mempool with transactions
2. **Action**: Call block assembly function
3. **Validation**: Verify block structure, transaction inclusion, coinbase reward
4. **Consensus Check**: Validate assembled block passes full validation

**Key Validations**:
- ✅ Block structure valid (header, coinbase, transactions)
- ✅ Transaction selection respects maturity rules
- ✅ Coinbase reward calculation correct
- ✅ Block validation passes
- ✅ Immature spends excluded or rejected

---

## Success Criteria

**Phase F.10 is COMPLETE when**:
1. ✅ T20 passes (mature transactions included)
2. ✅ T21 passes (immature transactions excluded)
3. ✅ T22 passes (block validation rejects immature)
4. ✅ All invariants (I.10.1 - I.10.5) validated
5. ✅ Documentation complete (test results, invariant validation)

**Optional (P1)**:
6. T23 passes (empty mempool handling)
7. T24 passes (fee calculation validation)

---

## Dependencies

### From Phase F.9
- ✅ Wallet creates valid transactions
- ✅ Mempool accepts/rejects transactions based on policy
- ✅ Consensus validation enforces coinbase maturity

### Required for F.10
- Block assembly infrastructure (may need to implement or locate)
- Block validation infrastructure
- Transaction selection from mempool
- Coinbase reward calculation

### APIs to Verify/Implement

**Mining/Block Assembly**:
- Does DineroCoin have a block assembler?
- How are blocks created for mining?
- Where is transaction selection implemented?

**Block Validation**:
- Does consensus validation check blocks?
- Where is block-level maturity validation?

**Exploration Required**: Investigate codebase for existing mining/block assembly infrastructure.

---

## Risk Assessment

### Risks

1. **No existing mining infrastructure**
   - Impact: May need to design tests around manual block construction
   - Mitigation: Focus on block validation tests if assembly doesn't exist

2. **Block assembly API unclear**
   - Impact: Test design depends on API discovery
   - Mitigation: Design tests to validate requirements, adapt to actual API

3. **Maturity validation may be incomplete**
   - Impact: Tests may reveal missing validation
   - Mitigation: Document bugs, add to fix backlog

### Unknowns

- Does DineroCoin have block assembly code?
- Where is block validation implemented?
- How are coinbase rewards calculated?
- Is there transaction selection logic?

**Approach**: Explore codebase first, then adapt test design to actual architecture.

---

## Exploration Phase

### Step 1: Search for Mining Infrastructure

**Search Patterns**:
```bash
# Search for mining/block assembly
rg -i "class.*miner|createblock|assembleblock|generateblock"

# Search for block validation
rg -i "validateblock|checkblock|acceptblock"

# Search for transaction selection
rg -i "selecttransaction|ordertransaction|prioritize"

# Search for coinbase creation
rg -i "createcoinbase|coinbasereward|blockreward"
```

### Step 2: Identify Key Components

**Questions to Answer**:
1. Where is block creation logic? (mining module? consensus module?)
2. How are transactions selected from mempool?
3. Where is block validation implemented?
4. How is coinbase reward calculated?

### Step 3: Design Tests Based on Findings

**Scenarios**:
- **If mining exists**: Test actual block assembly API
- **If mining missing**: Test manual block construction + validation
- **If validation exists**: Test validation catches maturity violations
- **If validation missing**: Document gap, design validation tests

---

## Timeline

**Phase F.10 Estimated Effort**:
- Exploration: 1 session (locate mining infrastructure)
- Test design: 1 session (adapt to actual architecture)
- T20 implementation: 1 session
- T21 implementation: 1 session
- T22 implementation: 1 session
- Documentation: 1 session
- **Total**: ~6 sessions

**With optional tests (T23, T24)**:
- Additional: 2-3 sessions

---

## Related Phases

**Previous**: Phase F.9 - Wallet ↔ Mempool Interaction (certified)
**Current**: Phase F.10 - Mining/Block Assembly Spending Paths
**Next**: Consensus Hardening (hostile tests)

---

## References

- Phase F.8: Wallet Spending Rules (certified)
- Phase F.9: Wallet ↔ Mempool Interaction (certified)
- Bitcoin Core: `mining/mining.cpp` (CreateNewBlock)
- Bitcoin Core: `validation.cpp` (CheckBlock, AcceptBlock)
- Consensus rules: Coinbase maturity (100 confirmations)

---

**Created**: 2025-12-29
**Status**: Planning - ready for exploration
**Next Step**: Explore codebase for mining/block assembly infrastructure

