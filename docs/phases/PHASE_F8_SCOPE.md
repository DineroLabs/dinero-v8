# Phase F.8: Wallet Spending Rules

**Phase**: F.8 - Wallet Spending Rules Implementation
**Status**: Planning
**Created**: 2025-12-29
**Depends On**: Phase F.7 (Wallet Persistence) ✅ COMPLETE

---

## Executive Summary

Phase F.8 implements wallet spending rules to ensure users cannot spend immature coinbase outputs and that spending validation integrates correctly with mempool and consensus layers.

**Scope**: Spending validation, coinbase maturity enforcement, mempool integration
**Approach**: Test-driven development following F.7 pattern
**Certification**: 100% of defined spending invariant tests must pass

---

## Motivation

**Why This Phase Exists**:

Phase F.7 validated that wallet persistence works correctly:
- UTXOs are tracked accurately
- Balances persist across restarts
- Reorgs remove orphaned UTXOs

**But F.7 did NOT validate**:
- Can users spend immature coinbase?
- Does wallet prevent invalid spending?
- Does mempool reject transactions spending immature outputs?

**Real-World Risk**:
```
User mines block 1 → 50 DIN coinbase
User sees balance: 50 DIN
User tries to spend immediately
Without F.8: Transaction created, broadcast, rejected by network
With F.8: Wallet prevents creation, clear error message
```

**Bitcoin Consensus Rule**:
> Coinbase outputs cannot be spent until they have 100 confirmations

This is a **consensus rule**, not just a policy. Transactions violating it are **invalid**, not just non-standard.

---

## Spending Invariants (S.1 - S.5)

### S.1: Immature Coinbase Spending Prevention

**Invariant**:
> The wallet MUST refuse to create transactions spending coinbase outputs with fewer than 100 confirmations.

**Why It Matters**:
- Prevents wasting fees on invalid transactions
- Protects users from confusion (balance shows but can't spend)
- Maintains network reputation (don't broadcast invalid txs)

**Test**: Attempt to spend coinbase at height 1 from height 50 → Must fail with clear error

---

### S.2: Mature Coinbase Spending Success

**Invariant**:
> The wallet MUST allow spending coinbase outputs with 100 or more confirmations.

**Why It Matters**:
- Users must be able to spend mining rewards after maturity
- Wallet must correctly calculate spendable balance
- Transaction creation must work for mature coinbase

**Test**: Spend coinbase at height 1 from height 101 → Must succeed

---

### S.3: Spendable Balance Calculation

**Invariant**:
> The wallet MUST exclude immature coinbase from spendable balance calculations.

**Why It Matters**:
- `getBalance()` already returns separate fields: confirmed, immature
- Spending logic must use **confirmed** balance (excludes immature)
- Users should not see "insufficient funds" when they have immature coinbase

**Test**: Verify `getBalance().confirmed` excludes immature coinbase

---

### S.4: Mempool Integration

**Invariant**:
> Transactions spending immature coinbase MUST be rejected by mempool validation.

**Why It Matters**:
- Defense in depth (wallet prevents, mempool also rejects)
- Network nodes reject invalid transactions
- Prevents relay spam

**Test**: Manually construct tx spending immature coinbase → Mempool rejects with specific error

---

### S.5: Reorg Impact on Spending

**Invariant**:
> After a reorg that orphans blocks, previously spendable coinbase MAY become unspendable again.

**Why It Matters**:
- Reorg from height 150 → 90 makes coinbase from block 1 still spendable
- Reorg from height 101 → 50 makes coinbase from block 1 unspendable again
- Wallet must recalculate spendable balance after reorg

**Test**: Create spendable coinbase, reorg to make it immature, verify can't spend

---

## Test Plan (Phase F.8)

### Priority 0 (P0) - Blocking Tests

| Test | Invariant | Description | Complexity |
|------|-----------|-------------|------------|
| **T12** | S.1 | Immature coinbase rejection | Medium |
| **T13** | S.2 | Mature coinbase spending | Medium |
| **T14** | S.3 | Spendable balance calculation | Low |
| **T15** | S.4 | Mempool rejects immature spend | High |
| **T16** | S.5 | Reorg impact on spendability | High |

### Priority 1 (P1) - Important but Not Blocking

| Test | Description |
|------|-------------|
| **T17** | Multiple UTXOs selection (prefer mature) |
| **T18** | Partial spend (change output) |
| **T19** | Fee estimation with mature UTXOs |

---

## Test Specifications

### T12: Immature Coinbase Rejection (S.1)

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Advance to height 50 (49 confirmations)

**Test**:
1. Attempt to create transaction spending the coinbase
2. `createTransaction(recipient, 10.0)` should fail

**Success Criteria**:
- Transaction creation returns error
- Error message mentions "immature" or "coinbase maturity"
- No transaction created
- UTXO remains unspent

**Failure Modes**:
- Transaction created successfully (violates S.1)
- Generic error (unclear to user)
- Wallet crashes

---

### T13: Mature Coinbase Spending (S.2)

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Advance to height 101 (100 confirmations)

**Test**:
1. Create transaction spending the coinbase
2. `createTransaction(recipient, 10.0)` should succeed

**Success Criteria**:
- Transaction created successfully
- Transaction has 1 input (the coinbase UTXO)
- Transaction has 2 outputs (payment + change)
- Transaction is valid (passes consensus checks)

**Failure Modes**:
- Transaction creation fails (violates S.2)
- Wrong inputs selected
- Invalid transaction structure

---

### T14: Spendable Balance Calculation (S.3)

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet (immature)
3. Receive 25 DIN regular transaction (confirmed)
4. Advance to height 50

**Test**:
1. Query `getBalance()`
2. Check confirmed vs immature fields

**Success Criteria**:
- `confirmed` = 25 DIN (excludes coinbase)
- `immature` = 50 DIN (the coinbase)
- `total` = 75 DIN
- Spendable calculation uses `confirmed` only

**Failure Modes**:
- Confirmed includes immature coinbase
- Spendable balance incorrect
- User confused about available funds

---

### T15: Mempool Rejects Immature Spend (S.4)

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Advance to height 50 (49 confirmations)
4. Manually construct transaction spending coinbase

**Test**:
1. Submit transaction to mempool
2. `acceptToMemoryPool(tx)` should reject

**Success Criteria**:
- Mempool rejects transaction
- Rejection reason: "immature-coinbase-spend" or similar
- Transaction NOT added to mempool
- Transaction NOT relayed to network

**Failure Modes**:
- Mempool accepts transaction (consensus violation!)
- Generic rejection (unclear why)
- Mempool crashes

---

### T16: Reorg Impact on Spendability (S.5)

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Advance to height 101 (mature, spendable)
4. Trigger reorg back to height 50

**Test**:
1. After reorg, attempt to spend coinbase
2. Should fail (now only 49 confirmations)

**Success Criteria**:
- Spendable balance recalculated after reorg
- Attempt to spend fails with maturity error
- UTXO still exists but marked immature

**Failure Modes**:
- Wallet allows spend after reorg (stale maturity)
- Balance not updated after reorg
- UTXO removed (should only be marked immature)

---

## Implementation Strategy

### Phase 1: Maturity Calculation (T14)

**Goal**: Fix `getBalance()` to properly segregate immature coinbase

**Current Issue** (from F.7):
- T8 showed coinbase appears in "confirmed" not "immature"
- SQL query needs maturity check: `current_height - utxo_height >= 100`

**Implementation**:
```sql
SELECT
  SUM(CASE
    WHEN is_spent = 0 AND
         (NOT is_coinbase OR (? - height >= 100))
    THEN amount ELSE 0 END) as confirmed,
  SUM(CASE
    WHEN is_spent = 0 AND
         is_coinbase AND
         (? - height < 100)
    THEN amount ELSE 0 END) as immature,
  ...
FROM utxos
```

**Validation**: T14 standalone test

---

### Phase 2: Spending Prevention (T12)

**Goal**: Make `createTransaction()` check UTXO maturity

**Implementation**:
```cpp
bool WalletManager::isUTXOSpendable(const UTXO& utxo) const {
    if (utxo.is_spent) return false;

    if (utxo.is_coinbase) {
        int confirmations = current_blockchain_height_ - utxo.height + 1;
        return confirmations >= 100;
    }

    return true;  // Regular UTXOs always spendable
}
```

**Validation**: T12 standalone test

---

### Phase 3: Successful Spending (T13)

**Goal**: Verify mature coinbase can be spent

**No new implementation needed** - just validation that existing transaction creation works with mature coinbase.

**Validation**: T13 standalone test

---

### Phase 4: Mempool Integration (T15)

**Goal**: Ensure mempool also validates coinbase maturity

**Implementation** (likely already exists in consensus):
```cpp
bool CheckTransaction(const Transaction& tx, const ChainState& chain) {
    for (const auto& input : tx.vin) {
        UTXO utxo = chain.getUTXO(input.prevout);
        if (utxo.is_coinbase) {
            int confirmations = chain.getHeight() - utxo.height + 1;
            if (confirmations < 100) {
                return error("immature-coinbase-spend");
            }
        }
    }
    return true;
}
```

**Validation**: T15 standalone test

---

### Phase 5: Reorg Spendability (T16)

**Goal**: Verify spendability recalculated after reorg

**Implementation**: Already handled by `setBlockchainHeight()` in wallet_manager

**Validation**: T16 standalone test

---

## Success Criteria

Phase F.8 is complete when:

1. ✅ All 5 P0 tests passing (T12-T16)
2. ✅ No immature coinbase can be spent
3. ✅ Mature coinbase can be spent successfully
4. ✅ Mempool rejects immature spends
5. ✅ Reorg correctly updates spendability

---

## Hard Rules (F.8 Certification)

Following F.7 pattern:

1. **No spending code without tests** - All spending logic must be validated by tests
2. **No release without T12-T16 passing** - 5/5 P0 tests required
3. **No exceptions** - Spending rules are consensus-critical
4. **Defense in depth** - Wallet AND mempool must validate

---

## Timeline

**This phase does NOT have a timeline.**

Per RELEASE_POLICY.md, phases are scope-based, not time-based.

F.8 is complete when:
- All in-scope tests passing
- Spending validation works correctly
- No known consensus violations

**Estimated complexity**: Medium (leverages F.7 foundation)

---

## Comparison to F.7

| Aspect | F.7 (Persistence) | F.8 (Spending) |
|--------|-------------------|----------------|
| Tests Defined | 9 P0 + 1 deferred | 5 P0 + 3 P1 |
| Complexity | Medium | Medium |
| Core Focus | Database correctness | Consensus rules |
| Bug Risk | Medium (phantom balance) | **High** (consensus violation) |
| User Impact | Balance display | **Transaction validity** |

**Key Difference**: F.8 is **consensus-critical**. A bug here means invalid transactions broadcast to network.

---

## Dependencies

**Requires F.7 Complete** ✅:
- UTXO tracking works
- Balance calculation works
- Reorg handling works

**Requires Consensus Layer**:
- Mempool validation (likely already exists)
- Transaction validation (likely already exists)
- May just need integration, not new implementation

---

## Next Steps

1. Create Phase F.8 test plan document (detailed test matrix)
2. Implement T14 (balance calculation fix) - foundation for all other tests
3. Implement T12 (immature rejection)
4. Implement T13 (mature spending)
5. Implement T15 (mempool validation)
6. Implement T16 (reorg spendability)
7. Certify F.8 complete (5/5 tests passing)

---

**Document Version**: 1.0
**Last Updated**: 2025-12-29
**Status**: Ready to begin implementation
