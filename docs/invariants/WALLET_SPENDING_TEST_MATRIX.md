# Wallet Spending Rules - Test Specification Matrix

**Phase**: F.8 - Wallet Spending Rules
**Status**: Defined
**Created**: 2025-12-29

---

## Purpose

This document defines the complete test matrix for wallet spending rules validation. Every test here must pass before Phase F.8 can be certified complete.

**Invariants Under Test**: S.1 - S.5 (Spending Rules)

---

## Test Classification

- **P0**: Priority 0 - Blocking (must pass for certification)
- **P1**: Priority 1 - Important (should pass, but not blocking)
- **P2**: Priority 2 - Nice to have (future enhancement)

---

## P0 Tests (Blocking)

### T12: Immature Coinbase Rejection (S.1)

**Priority**: P0 (Consensus-critical)

**Validates**: Wallet refuses to spend immature coinbase

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Mine blocks 2-50 (advance to height 50)
4. Blockchain height now: 50
5. Coinbase confirmations: 50 (immature, needs 100)

**Test**:
1. Attempt: `createTransaction(recipient_address, 10.0)`
2. Expected: Function returns error/failure
3. Expected: Error message mentions "immature" or "maturity"

**Success Criteria**:
- createTransaction() returns false or throws exception
- Error message clear: "Cannot spend immature coinbase" or similar
- No transaction object created
- UTXO remains in database with is_spent=0

**Failure Modes**:
- Function succeeds (creates transaction) → **CRITICAL BUG**
- Generic error message (user confused)
- Wallet crashes
- UTXO incorrectly marked as spent

**Consensus Impact**: HIGH - prevents invalid transaction broadcast

---

### T13: Mature Coinbase Spending (S.2)

**Priority**: P0 (Consensus-critical)

**Validates**: Wallet allows spending mature coinbase

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Mine blocks 2-101 (advance to height 101)
4. Blockchain height now: 101
5. Coinbase confirmations: 101 (mature, >= 100)

**Test**:
1. Attempt: `createTransaction(recipient_address, 10.0)`
2. Expected: Function succeeds
3. Verify transaction structure

**Success Criteria**:
- createTransaction() returns true/success
- Transaction created with valid structure:
  - 1 input: coinbase UTXO from block 1
  - 2 outputs: 10 DIN to recipient, 39.9999 DIN change (minus fee)
- Transaction passes CheckTransaction() validation
- UTXO marked as spent in wallet database

**Failure Modes**:
- Function fails (rejects mature coinbase) → **CRITICAL BUG**
- Transaction malformed
- Wrong UTXO selected
- Change calculation incorrect

**Consensus Impact**: HIGH - must allow legitimate spends

---

### T14: Spendable Balance Calculation (S.3)

**Priority**: P0 (User-facing)

**Validates**: getBalance() correctly segregates immature coinbase

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet (immature)
3. Receive regular transaction: 25 DIN at height 50 (confirmed)
4. Blockchain height now: 50

**Test**:
1. Query: `balance = getBalance()`
2. Verify balance fields

**Success Criteria**:
- `balance.confirmed` = 25.0 DIN (regular UTXO only)
- `balance.immature` = 50.0 DIN (coinbase only)
- `balance.total` = 75.0 DIN
- `balance.utxo_count` = 2
- Spendable calculation uses `confirmed` (excludes immature)

**Failure Modes**:
- confirmed includes immature coinbase → **USER CONFUSION**
- immature shows 0 (coinbase not tracked)
- total incorrect
- User attempts spend, gets "insufficient funds"

**User Impact**: HIGH - direct balance display

---

### T15: Mempool Rejects Immature Spend (S.4)

**Priority**: P0 (Defense in depth)

**Validates**: Mempool validation catches immature spends

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Advance to height 50 (immature)
4. **Manually construct** transaction spending the coinbase
   (bypasses wallet's createTransaction validation)

**Test**:
1. Submit to mempool: `acceptToMemoryPool(tx)`
2. Expected: Rejection with specific error

**Success Criteria**:
- Mempool rejects transaction
- Rejection reason: "immature-coinbase-spend" or "bad-txns-premature-spend-of-coinbase"
- Transaction NOT in mempool
- Transaction NOT relayed to peers
- Wallet can still query the UTXO (not removed)

**Failure Modes**:
- Mempool accepts transaction → **CONSENSUS VIOLATION**
- Generic rejection (hard to debug)
- Node crashes
- UTXO removed from wallet

**Consensus Impact**: **CRITICAL** - last line of defense

---

### T16: Reorg Impact on Spendability (S.5)

**Priority**: P0 (Reorg safety)

**Validates**: Spendability recalculated after reorg

**Setup**:
1. Create wallet with address
2. Mine block 1 with 50 DIN coinbase to wallet
3. Mine blocks 2-101 (height 101, mature)
4. Verify can spend: `createTransaction(recipient, 10.0)` → succeeds
5. **Trigger reorg**: Disconnect blocks 51-101
6. Blockchain height now: 50 (coinbase immature again)

**Test**:
1. After reorg, attempt: `createTransaction(recipient, 10.0)`
2. Expected: Failure (coinbase immature)

**Success Criteria**:
- Before reorg: createTransaction() succeeds
- After reorg: createTransaction() fails with maturity error
- Balance recalculated:
  - Before: confirmed=50, immature=0
  - After: confirmed=0, immature=50
- UTXO still exists (not removed, just immature)

**Failure Modes**:
- After reorg, still allows spend → **CONSENSUS VIOLATION**
- Balance not updated
- UTXO removed (should persist)
- Wallet crashes during reorg

**Consensus Impact**: HIGH - reorg must maintain validity

---

## P1 Tests (Important, Not Blocking)

### T17: UTXO Selection Prefers Mature

**Priority**: P1

**Validates**: Wallet selects mature UTXOs over immature when possible

**Setup**:
1. Wallet has:
   - Coinbase UTXO: 50 DIN at height 1 (immature at height 50)
   - Regular UTXO: 30 DIN at height 40 (confirmed)
2. User requests: `createTransaction(recipient, 20.0)`

**Test**:
1. Transaction should use the 30 DIN regular UTXO
2. Should NOT attempt to use 50 DIN coinbase

**Success Criteria**:
- Transaction uses regular UTXO (not coinbase)
- Change output: ~9.9999 DIN (30 - 20 - fee)

**Failure Modes**:
- Selects coinbase (fails to create tx)
- "Insufficient funds" error (didn't consider regular UTXO)

---

### T18: Partial Spend with Change

**Priority**: P1

**Validates**: Spending mature coinbase creates correct change output

**Setup**:
1. Wallet has: 50 DIN mature coinbase
2. Request: `createTransaction(recipient, 10.0)`

**Test**:
1. Verify transaction structure:
   - Input: 50 DIN coinbase
   - Output 1: 10 DIN to recipient
   - Output 2: 39.9999 DIN change back to wallet

**Success Criteria**:
- Change output exists
- Change amount correct (input - output - fee)
- Change goes to wallet address

---

### T19: Fee Estimation with Mature UTXOs

**Priority**: P1

**Validates**: Fee estimation only considers spendable UTXOs

**Setup**:
1. Wallet has:
   - 50 DIN immature coinbase
   - 25 DIN confirmed regular UTXO
2. Request: Estimate fee for 20 DIN transaction

**Test**:
1. Fee estimation should use 25 DIN UTXO
2. Should NOT consider 50 DIN immature coinbase

**Success Criteria**:
- Fee calculated based on available UTXOs (25 DIN)
- Does not fail due to "insufficient funds"

---

## Test Execution Order

**Recommended sequence** (easiest to hardest):

1. **T14** - Spendable balance calculation (foundation)
   - Fixes getBalance() maturity logic
   - All other tests depend on this

2. **T12** - Immature rejection
   - Tests prevention logic
   - Validates error handling

3. **T13** - Mature spending success
   - Tests positive case
   - Validates transaction creation

4. **T15** - Mempool validation
   - Tests consensus layer
   - May require mempool integration

5. **T16** - Reorg spendability
   - Combines T12/T13 with reorg
   - Most complex

---

## Test Implementation Approach

Following F.7 pattern (standalone C++ tests):

**Test Structure**:
```cpp
// tests/wallet_spending/standalone_test_t12.cpp

bool test_t12_immature_rejection() {
    // Setup: Create wallet
    // Mine block 1 with coinbase to wallet
    // Advance to height 50

    // Test: Attempt createTransaction()
    auto result = wallet->createTransaction(recipient, 10.0);

    // Verify: Should fail
    if (result.success) {
        std::cout << "❌ FAIL - Created tx spending immature coinbase\n";
        return false;
    }

    if (result.error.find("immature") == std::string::npos) {
        std::cout << "❌ FAIL - Error message unclear\n";
        return false;
    }

    std::cout << "✅ PASS - Immature coinbase rejected\n";
    return true;
}
```

**CMake Registration**:
```cmake
add_executable(standalone_test_t12 standalone_test_t12.cpp)
target_link_libraries(standalone_test_t12 PRIVATE dinero_core ...)
add_test(NAME WalletSpending_T12_Immature COMMAND standalone_test_t12)
```

---

## Certification Criteria

Phase F.8 certified complete when:

- [x] T12: Immature coinbase rejection - PASS
- [x] T13: Mature coinbase spending - PASS
- [x] T14: Spendable balance calculation - PASS
- [x] T15: Mempool rejects immature - PASS
- [x] T16: Reorg spendability - PASS

**Required**: 5/5 P0 tests passing
**Current**: 0/5 (0%)

---

## Known Challenges

### Challenge 1: Transaction Creation API

**Issue**: Does `createTransaction()` exist in WalletManager?

**Investigation Needed**:
- Check if WalletManager has transaction creation
- May need to implement if missing
- Should return struct with success/error/transaction

### Challenge 2: Mempool Access

**Issue**: How to submit transactions to mempool in test?

**Options**:
1. Direct mempool API (if accessible)
2. RPC call (`sendrawtransaction`)
3. Mock mempool for testing

### Challenge 3: Mature UTXO Selection

**Issue**: Does UTXO selection consider maturity?

**Implementation** (if needed):
```cpp
std::vector<UTXO> selectUTXOs(uint64_t target_amount) {
    std::vector<UTXO> selected;
    uint64_t total = 0;

    for (const auto& utxo : getAllUTXOs()) {
        if (!isUTXOSpendable(utxo)) continue;  // Skip immature

        selected.push_back(utxo);
        total += utxo.amount;

        if (total >= target_amount) break;
    }

    return selected;
}
```

---

## Comparison to Bitcoin Core

**Bitcoin Core Behavior**:
```
$ bitcoin-cli getbalance
{
  "mine": {
    "trusted": 25.0,        # Confirmed, spendable
    "untrusted_pending": 0,
    "immature": 50.0        # Coinbase, not yet spendable
  }
}

$ bitcoin-cli sendtoaddress <addr> 10.0
Error: Insufficient funds (only 25.0 available, 50.0 immature)
```

**DineroCoin Should Match**:
- Clear separation of spendable vs immature
- Explicit error when attempting to spend immature
- Maturity enforced at 100 blocks

---

## Success Metrics

**Technical**:
- 5/5 P0 tests passing
- No invalid transactions created
- Mempool validation works

**User Experience**:
- Clear error messages
- Balance display accurate
- No confusion about spendable funds

**Network Health**:
- No invalid transactions relayed
- Consensus rules enforced
- Node reputation maintained

---

**Document Version**: 1.0
**Last Updated**: 2025-12-29
**Status**: Ready for implementation
