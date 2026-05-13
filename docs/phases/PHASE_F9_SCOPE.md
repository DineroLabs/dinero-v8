# Phase F.9: Wallet ↔ Mempool Interaction

**Status**: In Progress
**Priority**: P0 (Critical Path)
**Dependencies**: Phase F.8 (Wallet Spending Rules)

---

## Overview

Phase F.9 validates the **complete spending workflow** from wallet transaction creation through mempool acceptance/rejection and balance updates. This phase tests the **interaction** between wallet and mempool layers, ensuring they work together correctly.

**Key Principle**: The wallet and mempool must maintain **consistent state** - balances, UTXO availability, and transaction status must be synchronized.

---

## Objectives

1. **Validate successful spending path**: Wallet creates valid transaction → Mempool accepts → Balance updates correctly
2. **Validate consensus rejection path**: Wallet prevents creation of consensus-invalid transactions
3. **Validate policy rejection path**: Wallet creates transaction → Mempool rejects for policy reasons → Balance remains unchanged
4. **Validate balance consistency**: All balance updates reflect actual mempool state
5. **Validate UTXO state management**: Spent/unspent state synchronized between wallet and mempool

---

## Invariants

### I.9.1: Successful Spend Consistency

**Invariant**:
> "When wallet creates a valid transaction and mempool accepts it, the wallet MUST mark the spent UTXOs as unavailable and update the spendable balance to reflect the pending transaction."

**Validation**:
- Before: Wallet has mature UTXO, spendable balance = 50 DIN
- Create transaction spending 30 DIN (20 DIN change)
- Submit to mempool → MempoolAcceptResult::OK
- After: Spent UTXO unavailable, spendable balance reflects change output (20 DIN if confirmed, 0 if unconfirmed pending)

### I.9.2: Consensus Rejection at Wallet Layer

**Invariant**:
> "Wallet MUST refuse to create transactions that violate consensus rules (e.g., spending immature coinbase). Such transactions MUST fail at wallet layer, never reaching mempool."

**Validation**:
- Wallet has immature coinbase (50 confirmations)
- Attempt to create transaction spending it
- Expected: `createTransaction()` fails with error "Insufficient funds" or "No spendable UTXOs"
- Mempool is never called

### I.9.3: Policy Rejection Consistency

**Invariant**:
> "When mempool rejects a transaction for policy reasons (insufficient fee, conflict, etc.), wallet MUST NOT update balances or mark UTXOs as spent."

**Validation**:
- Wallet creates transaction with fee rate below minimum (0.1 sat/byte instead of 1.0 sat/byte)
- Submit to mempool → MempoolAcceptResult::INSUFFICIENT_FEE
- After: UTXO still available, spendable balance unchanged

### I.9.4: Balance Consistency After Rejection

**Invariant**:
> "After any mempool rejection (consensus or policy), wallet balances and UTXO availability MUST remain unchanged."

**Validation**:
- Record initial state: UTXOs, balances
- Create and submit transaction → mempool rejects
- Verify: UTXOs still available, balances match initial state

### I.9.5: Transaction Chaining (Advanced)

**Invariant**:
> "Wallet MUST be able to spend outputs of unconfirmed transactions (0-conf spending), and mempool MUST accept both parent and child if valid."

**Validation**:
- Create transaction A (spend mature coinbase → outputs)
- Submit transaction A → mempool accepts
- Create transaction B spending output of A (unconfirmed)
- Submit transaction B → mempool accepts (if policy allows)

### I.9.6: Double Spend Prevention

**Invariant**:
> "Wallet MUST prevent double-spending the same UTXO. If a UTXO is spent in a transaction already in mempool, wallet MUST mark it as unavailable for new transactions."

**Validation**:
- Create transaction A spending UTXO X
- Submit transaction A → mempool accepts
- Attempt to create transaction B also spending UTXO X
- Expected: `createTransaction()` excludes UTXO X (already spent)

---

## Test Cases

### T17: Successful Spend Path - Wallet → Mempool → Balance Update

**Setup**:
1. Wallet has mature coinbase UTXO (101 confirmations, 50 DIN)
2. Mempool empty, blockchain at height 101

**Execution**:
1. Get initial spendable balance (should be 50 DIN)
2. Create transaction: send 30 DIN to recipient, 19.99 DIN change (0.01 DIN fee)
3. Verify transaction created successfully
4. Submit transaction to mempool
5. Verify mempool acceptance (MempoolAcceptResult::OK)
6. Get updated spendable balance
7. Verify UTXO marked as spent

**Expected Results**:
- ✅ Transaction created successfully
- ✅ Mempool accepts transaction (MempoolAcceptResult::OK)
- ✅ Original UTXO marked as spent in wallet
- ✅ Spendable balance updated (depends on wallet policy: may show change as unconfirmed)
- ✅ Transaction appears in mempool (getCount() == 1)

**Invariants Validated**: I.9.1

---

### T18: Consensus Rejection - Immature Coinbase Spend Blocked at Wallet

**Setup**:
1. Wallet has immature coinbase UTXO (50 confirmations, 50 DIN)
2. Mempool empty, blockchain at height 50

**Execution**:
1. Attempt to create transaction spending immature coinbase
2. Verify wallet rejects creation (insufficient spendable funds)
3. Verify mempool never called

**Expected Results**:
- ❌ `createTransaction()` fails (no spendable UTXOs)
- ✅ Mempool never called (defense in depth - Layer 1 blocks it)
- ✅ Balance unchanged
- ✅ UTXO remains available but not spendable

**Invariants Validated**: I.9.2

---

### T19: Policy Rejection - Insufficient Fee

**Setup**:
1. Wallet has mature coinbase UTXO (101 confirmations, 50 DIN)
2. Mempool min fee rate: 1.0 sat/byte
3. Blockchain at height 101

**Execution**:
1. Create transaction with very low fee (0.1 sat/byte)
2. Submit to mempool
3. Verify mempool rejection (MempoolAcceptResult::INSUFFICIENT_FEE)
4. Verify UTXO still available
5. Verify balance unchanged

**Expected Results**:
- ✅ Transaction created successfully (wallet doesn't enforce mempool policy)
- ❌ Mempool rejects (MempoolAcceptResult::INSUFFICIENT_FEE)
- ✅ UTXO still available for spending
- ✅ Spendable balance unchanged (50 DIN)

**Invariants Validated**: I.9.3, I.9.4

---

### T20: Transaction Chaining - Spend Unconfirmed Output (0-conf)

**Setup**:
1. Wallet has mature coinbase UTXO (101 confirmations, 50 DIN)
2. Mempool empty, blockchain at height 101

**Execution**:
1. Create transaction A: spend 50 DIN → 25 DIN to recipient, 24.99 DIN change
2. Submit transaction A → mempool accepts
3. Create transaction B: spend 24.99 DIN change from A (unconfirmed) → 20 DIN to recipient, 4.98 DIN change
4. Submit transaction B → mempool should accept (if policy allows)
5. Verify mempool contains both transactions

**Expected Results**:
- ✅ Transaction A accepted
- ✅ Transaction B accepted (spends unconfirmed output)
- ✅ Mempool count == 2
- ✅ Transaction B has parent A (dependency tracking)

**Invariants Validated**: I.9.5

**Note**: This test validates whether wallet can track unconfirmed outputs and mempool can accept child transactions.

---

### T21: Double Spend Prevention - Same UTXO Twice

**Setup**:
1. Wallet has mature coinbase UTXO (101 confirmations, 50 DIN)
2. Mempool empty, blockchain at height 101

**Execution**:
1. Create transaction A spending the UTXO → 30 DIN to recipient A
2. Submit transaction A → mempool accepts
3. Verify UTXO marked as spent in wallet
4. Attempt to create transaction B also spending the same UTXO → 40 DIN to recipient B
5. Verify wallet prevents creation (UTXO not available)

**Expected Results**:
- ✅ Transaction A accepted by mempool
- ✅ UTXO marked as spent
- ❌ Transaction B creation fails (insufficient funds / UTXO unavailable)
- ✅ Only transaction A in mempool

**Invariants Validated**: I.9.6

---

## Implementation Approach

### Test Architecture

**Pattern**: Each test is a standalone executable following the T12-T16 pattern.

**Files**:
- `tests/wallet_persistence/standalone_test_t17.cpp` - Successful spend path
- `tests/wallet_persistence/standalone_test_t18.cpp` - Consensus rejection
- `tests/wallet_persistence/standalone_test_t19.cpp` - Policy rejection
- `tests/wallet_persistence/standalone_test_t20.cpp` - Transaction chaining (optional - advanced)
- `tests/wallet_persistence/standalone_test_t21.cpp` - Double spend prevention (optional - advanced)

**Priority**:
- **P0 (Required)**: T17, T18, T19 (core spending workflow)
- **P1 (Recommended)**: T20, T21 (advanced scenarios)

### Components Needed

1. **Wallet Transaction Creation**:
   - `wallet->createTransaction(recipients, fee_rate)` or similar API
   - Returns: Transaction object + metadata

2. **Mempool Submission**:
   - `mempool.submitTransaction(tx, view, height, time, mode)`
   - Returns: `MempoolAcceptResult`

3. **Balance Queries**:
   - `wallet->getSpendableBalance()`
   - `wallet->listUnspentUTXOs(min_confirmations)`

4. **UTXO State Tracking**:
   - `wallet->markUTXOSpent(txid, vout)` (internal)
   - `wallet->isUTXOAvailable(txid, vout)` (query)

### Testing Strategy

**Workflow**:
1. Setup: Create wallet, add UTXOs, set blockchain height
2. Pre-state: Query balances, UTXO availability
3. Action: Create transaction (may fail here for consensus violations)
4. Submit: Submit to mempool (may fail here for policy violations)
5. Post-state: Query balances, UTXO availability, mempool state
6. Verify: Compare pre/post state against expected behavior

**Key Validations**:
- ✅ Balance consistency (before/after)
- ✅ UTXO state consistency (available → spent)
- ✅ Mempool state (transaction present/absent)
- ✅ Error codes match expected reasons

---

## Success Criteria

**Phase F.9 is COMPLETE when**:
1. ✅ T17 passes (successful spend path validated)
2. ✅ T18 passes (consensus rejection at wallet layer)
3. ✅ T19 passes (policy rejection with state consistency)
4. ✅ All invariants (I.9.1 - I.9.4) validated
5. ✅ Documentation complete (test results, invariant validation)

**Optional (P1)**:
6. T20 passes (transaction chaining works)
7. T21 passes (double spend prevention works)

---

## Dependencies

### From Phase F.8
- ✅ Wallet UTXO selection respects maturity (T12)
- ✅ Wallet balance calculation segregates immature (T14)
- ✅ Mempool enforces coinbase maturity (T15)
- ✅ Dynamic maturity recalculation (T16)

### Required for F.9
- Wallet transaction creation API (`createTransaction()`)
- Wallet balance update after mempool submission
- Wallet UTXO tracking (spent/unspent state)

### APIs to Verify/Implement

**Wallet APIs** (may already exist):
```cpp
// Transaction creation
std::optional<Transaction> createTransaction(
    const std::vector<Recipient>& recipients,
    double fee_rate_sat_per_byte
);

// Balance queries
uint64_t getSpendableBalance(uint32_t min_confirmations = 1);

// UTXO management
void markUTXOSpent(const uint256& txid, uint32_t vout);
bool isUTXOAvailable(const uint256& txid, uint32_t vout);
```

**Mempool APIs** (already validated in F.8):
```cpp
MempoolAcceptResult submitTransaction(
    const Transaction& tx,
    const consensus::ChainStateView& view,
    uint32_t current_height,
    uint64_t current_time,
    MempoolSubmitMode mode
);
```

---

## Risk Assessment

### Risks

1. **Wallet transaction creation API may not exist**
   - Mitigation: Manually construct transactions in tests if needed
   - Fallback: Test only mempool layer (partial F.9 coverage)

2. **Wallet may not track mempool-spent UTXOs**
   - Impact: Double spend prevention (I.9.6) may fail
   - Mitigation: Add UTXO state tracking if needed

3. **Balance updates may not reflect mempool state**
   - Impact: Balance consistency tests (I.9.1, I.9.4) may reveal bugs
   - Mitigation: Document bugs, add to fix backlog

### Unknowns

- Does wallet have `createTransaction()` API?
- Does wallet track UTXOs in mempool (spent but unconfirmed)?
- Does wallet update balances after mempool submission?

**Approach**: Explore wallet codebase to determine API availability, then design tests accordingly.

---

## Timeline

**Phase F.9 Estimated Effort**:
- API exploration: 1 session
- T17 implementation: 1 session
- T18 implementation: 1 session
- T19 implementation: 1 session
- Documentation: 1 session
- **Total**: ~5 sessions

**With optional tests (T20, T21)**:
- Additional: 2-3 sessions

---

## Related Phases

**Previous**: Phase F.8 - Wallet Spending Rules (certified)
**Current**: Phase F.9 - Wallet ↔ Mempool Interaction
**Next**: Mining/Block Assembly Spending Paths
**Future**: Consensus Hardening (hostile tests)

---

## References

- Phase F.8: Wallet Spending Rules (certified)
- Bitcoin Core: `wallet/spend.cpp` (CreateTransaction)
- Bitcoin Core: `validation.cpp` (AcceptToMemoryPool)
- Mempool API: `src/mempool/mempool.cpp`
- Wallet API: `src/wallet/wallet.cpp` (to be explored)

---

**Created**: 2025-12-29
**Status**: Planning complete, ready for implementation
**Next Step**: Explore wallet transaction creation APIs
