# T9 Test Results: Orphaned Mining Reward Disappears

**Test**: T9 - Orphaned Mining Reward Disappears
**Invariant**: W.5 - Mining Reward Attribution
**Date**: 2025-12-29
**Status**: ✅ **PASS** (after bug fix)

---

## Test Summary

Validates that orphaned coinbase outputs are removed from the wallet when a blockchain reorganization occurs.

## Test Method

**Approach**: Standalone C++ test (GoogleTest workaround)
**Binary**: `build/bin/standalone_test_t9`
**Source**: `tests/wallet_persistence/standalone_test_t9.cpp`

## Test Procedure

1. **Setup**: Create temporary test directory
2. **Create wallet**: Initialize new wallet via WalletManager
3. **Create test scriptPubKey**: Generate P2PKH-like scriptPubKey
4. **Add address to wallet**: Insert address with scriptPubKey into wallet database
5. **Record initial balance**: Query wallet balance (should be 0)
6. **Mine block 101**: Create block with coinbase to wallet address
7. **Record balance after mining**: Verify coinbase UTXO added
8. **Trigger reorg**: Call `onBlockDisconnected(block_101, 101)`
9. **Record balance after reorg**: Verify coinbase UTXO removed
10. **Verify**: UTXO removed and balance reverted to 0

## Test Results

```
[T9.4] Recording initial balance...
✓ Initial balance:
    Confirmed: 0 DIN (0 una)
    UTXO count: 0

[T9.5] Mining block 101 with coinbase to wallet...
[WalletManager] Added UTXO amount: 50.000000 DIN
✓ Block 101 connected

[T9.6] Recording balance after mining...
✓ Balance after mining:
    Confirmed: 50 DIN (5000000000 una)
    UTXO count: 1

✓ Prerequisite satisfied: Coinbase UTXO added to wallet

[T9.7] Simulating blockchain reorg (orphaning block 101)...
[WalletManager] 🔄 Processing block disconnect at height 101
[WalletManager] ✅ Block 101 disconnected - removed 1 UTXOs, restored 0 UTXOs
✓ Block 101 disconnected (orphaned)

[T9.8] Recording balance after reorg...
✓ Balance after reorg:
    Confirmed: 0 DIN (0 una)
    UTXO count: 0

[T9.9] Verifying orphaned coinbase removed (W.5)...
✅ PASS - Orphaned coinbase removed (W.5 validated)

Verification:
  Initial state:
    Balance:     0 una
    UTXO count:  0

  After mining block 101:
    Balance:     5000000000 una
    UTXO count:  1
    ✓ Coinbase added

  After reorg (block 101 orphaned):
    Balance:     0 una
    UTXO count:  0
    ✓ Coinbase removed

W.5 Invariant Satisfied:
"Orphaned coinbase outputs must be removed from the wallet
 when a blockchain reorganization occurs."
```

## Validation

✅ **Coinbase UTXO added during mining**:
- Initial balance: 0 DIN, UTXO count: 0
- After mining: 50 DIN, UTXO count: 1
- **Prerequisite met** ✓

✅ **Coinbase UTXO removed during reorg**:
- Before reorg: 50 DIN, UTXO count: 1
- After reorg: 0 DIN, UTXO count: 0
- **Orphaned UTXO deleted** ✓

✅ **Balance reverted to initial state**:
- Initial: 0 una
- After reorg: 0 una
- **No phantom balance** ✓

✅ **Invariant W.5 satisfied**:
> "Orphaned coinbase outputs must be removed from the wallet
>  when a blockchain reorganization occurs."

## Bug Found and Fixed

### Initial Test Failure

**First run result**: ❌ FAIL - Orphaned coinbase NOT removed

**Log output**:
```
[WalletManager] Block 101 disconnected - removed 0 UTXOs, restored 0 UTXOs
Balance after reorg: 0 DIN confirmed, 50 DIN unconfirmed, UTXO count: 1
```

**Problem**: DELETE query didn't match any rows

### Root Cause Analysis

The `onBlockDisconnected` implementation had incorrect SQL queries referencing a `wallet_id` column that doesn't exist in the per-wallet database schema.

**Buggy SQL** (line 3715):
```sql
DELETE FROM utxos WHERE wallet_id = ? AND height = ?
```

**Per-wallet database schema** (line 3688):
```cpp
// Schema columns: id, txid, vout, amount, address, script_pubkey, height, is_coinbase, is_spent, ...
// NOTE: No wallet_id column (each wallet has separate database)
```

**Why it failed**:
- Query referenced non-existent `wallet_id` column
- SQLite silently failed to match any rows (no error, just 0 rows deleted)
- UTXO remained in database after reorg

### Bug Fix Applied

**Fixed** 4 SQL queries in `/src/wallet/wallet_manager.cpp`:

**1. onBlockDisconnected - DELETE orphaned UTXOs** (line 4716):
```cpp
// OLD (broken)
const char* sql = "DELETE FROM utxos WHERE wallet_id = ? AND height = ?";

// NEW (fixed)
// Note: Per-wallet database - no wallet_id column needed
const char* sql = "DELETE FROM utxos WHERE height = ?";
```

**2. onBlockDisconnected - UPDATE restore spent UTXOs** (line 4749):
```cpp
// OLD (broken)
const char* sql = "UPDATE utxos SET is_spent = 0 WHERE wallet_id = ? AND txid = ? AND vout = ? AND is_spent = 1";

// NEW (fixed)
// Note: Per-wallet database - no wallet_id column needed
const char* sql = "UPDATE utxos SET is_spent = 0 WHERE txid = ? AND vout = ? AND is_spent = 1";
```

**3. spendUTXO** (line 3731):
```cpp
// OLD (broken)
const char* sql = "UPDATE utxos SET is_spent = 1 WHERE wallet_id = ? AND txid = ? AND vout = ?";

// NEW (fixed)
// Note: Per-wallet database - no wallet_id column needed
const char* sql = "UPDATE utxos SET is_spent = 1 WHERE txid = ? AND vout = ?";
```

**4. removeUTXO** (line 3755):
```cpp
// OLD (broken)
const char* sql = "DELETE FROM utxos WHERE wallet_id = ? AND txid = ? AND vout = ?";

// NEW (fixed)
// Note: Per-wallet database - no wallet_id column needed
const char* sql = "DELETE FROM utxos WHERE txid = ? AND vout = ?";
```

### Post-Fix Test Result

**Second run result**: ✅ PASS - Orphaned coinbase removed

**Log output**:
```
[WalletManager] Block 101 disconnected - removed 1 UTXOs, restored 0 UTXOs
Balance after reorg: 0 DIN, UTXO count: 0
```

**Verification**:
- DELETE query now matches correctly (removed 1 UTXO)
- Balance reverted to 0
- UTXO count back to 0
- W.5 invariant satisfied

## Technical Details

### Reorg Simulation Flow

**Test simulates real-world reorg**:
```
Block 101 mined:
  Coinbase → Wallet address (50 DIN)
  Wallet processes: onBlockConnected(block_101, 101)
  UTXO added to database (height=101)
  Balance: 50 DIN

Competing chain wins (reorg):
  Block 101 orphaned
  Wallet processes: onBlockDisconnected(block_101, 101)
  DELETE FROM utxos WHERE height = 101
  1 row deleted (the coinbase UTXO)
  Balance: 0 DIN
```

### onBlockDisconnected Implementation

**What the function does** (from `wallet_manager.cpp:4696`):

```cpp
void WalletManager::onBlockDisconnected(const Block& block, uint32_t height) {
    // Step 1: Remove all UTXOs created in this block
    //         Identifies them by height column
    DELETE FROM utxos WHERE height = ?

    // Step 2: Restore spent UTXOs (mark as unspent)
    //         For each tx input in the block, mark UTXO as unspent
    UPDATE utxos SET is_spent = 0 WHERE txid = ? AND vout = ?

    // Step 3: Update wallet blockchain height
    setBlockchainHeight(height - 1)
}
```

**Critical for W.5**: Step 1 removes coinbase UTXOs from orphaned blocks

### Why This Test Is Important

**Without this invariant** (broken reorg handling):
```
User mines block 101 → 50 DIN reward appears
Reorg happens (block 101 orphaned)
Wallet keeps 50 DIN UTXO (phantom balance)
User sees 50 DIN but UTXO doesn't exist on canonical chain
User tries to spend → transaction invalid (references non-existent UTXO)
```

**With W.5** (correct reorg handling):
```
User mines block 101 → 50 DIN reward appears
Reorg happens (block 101 orphaned)
Wallet removes 50 DIN UTXO (no phantom balance)
Balance: 0 DIN (correct)
User sees accurate balance matching canonical chain
```

## Real-World Scenario

**Bitcoin/DineroCoin Reorg Example**:

```
Scenario: Two miners find block 12345 simultaneously

Chain A (User's node):
  Block 12344 → Block 12345 (coinbase to user)

Chain B (Network majority):
  Block 12344 → Block 12345' (coinbase to someone else)

If Chain B has more work:
  1. Node switches to Chain B (reorg)
  2. Block 12345 orphaned
  3. Wallet must remove user's coinbase UTXO
  4. Otherwise: phantom balance (UTXO doesn't exist on canonical chain)
```

**Real Impact**:
- Reorgs common during normal operation (1-2 blocks deep)
- Especially important for miners (coinbase outputs frequently orphaned)
- Without proper handling: wallet shows incorrect balance
- Users could attempt to spend non-existent UTXOs

## Comparison to Other Tests

### T9 vs T7 (Mining Reward Appears)

| Aspect | T7 (Reward Appears) | T9 (Orphaned Reward) |
|--------|---------------------|----------------------|
| **Focus** | Coinbase UTXO detection | Coinbase UTXO removal |
| **Test Flow** | Mine → Verify UTXO added | Mine → Reorg → Verify UTXO removed |
| **Validates** | onBlockConnected works | onBlockDisconnected works |
| **W.5 Aspect** | "must appear" | "must be removed when orphaned" |

### T9 vs T5 (Chain Reorg Safety)

| Aspect | T5 (Reorg Safety) | T9 (Orphaned Reward) |
|--------|-------------------|----------------------|
| **Focus** | Reorg event handling | Orphaned coinbase removal |
| **Test Flow** | Connect → Disconnect → Verify | Mine coinbase → Disconnect → Verify |
| **UTXO Source** | Test block (no wallet match) | Coinbase to wallet address |
| **Verification** | Height tracking correct | UTXO actually removed |

**T9 = T7 + T5**: Combines coinbase mining (T7) with reorg handling (T5)

## Code Evidence

### DELETE Query Execution

**From test logs**:
```
[WalletManager] 🔄 Processing block disconnect at height 101
[WalletManager] ✅ Block 101 disconnected - removed 1 UTXOs, restored 0 UTXOs
```

**Confirms**:
- DELETE query executed successfully
- 1 UTXO removed (the coinbase at height 101)
- 0 UTXOs restored (coinbase has no inputs to restore)

### Balance Queries

**Before reorg**:
```
getBalance row: conf=5000000000.000000 unconf=0.000000 imm=0.000000 sp=1 imm_n=0 tot=1
getBalance result: confirmed=50.000000 ... utxo_count=1
```

**After reorg**:
```
getBalance row: conf=0.000000 unconf=0.000000 imm=0.000000 sp=0 imm_n=0 tot=0
getBalance result: confirmed=0.000000 ... utxo_count=0
```

**Confirms**:
- UTXO count: 1 → 0 (removed)
- Confirmed balance: 50 DIN → 0 DIN (reverted)
- All balance fields zeroed out

## Known Limitations

### 1. Single Block Reorg

**Current**: Only tests depth-1 reorg (one block orphaned)
**Impact**: Doesn't validate multi-block reorg handling
**Future**: Could test deeper reorgs (2-3 blocks)

### 2. Coinbase Only

**Not Tested**: Regular transactions in orphaned blocks
**Reason**: T9 focuses specifically on mining rewards (W.5)
**Coverage**: Regular transactions covered by other tests

### 3. No Spend-Then-Reorg

**Not Tested**: Spending coinbase before reorg orphans it
**Complexity**: Requires transaction creation + reorg
**Future**: Could add advanced reorg scenario tests

## Lessons Learned

### Bug Discovery Through Testing

**This test revealed a real bug**:
- Tests are not just validation - they find implementation issues
- SQL schema mismatch (legacy multi-wallet vs current per-wallet)
- Silent failures (0 rows affected, no error thrown)

**Value of test-driven development**:
- Bug found before production deployment
- Clear reproduction case (test fails → fix → test passes)
- Regression prevention (test now catches this bug forever)

### Importance of Logging

**Logs were critical for diagnosis**:
```
[WalletManager] Block 101 disconnected - removed 0 UTXOs, restored 0 UTXOs
```

This one line revealed the bug immediately:
- Expected: "removed 1 UTXOs"
- Actual: "removed 0 UTXOs"
- Root cause: SQL query didn't match any rows

## Future Enhancements

### Multi-Block Reorg Test

Test deeper reorganizations:
```cpp
// Mine blocks 100, 101, 102 with coinbase to wallet
// Trigger reorg orphaning blocks 101, 102
// Verify both coinbase UTXOs removed
```

### Reorg During Active Spending

Test edge case:
```cpp
// Mine block with coinbase
// Create transaction spending coinbase
// Reorg orphans the block
// Verify:
//   - Coinbase UTXO removed
//   - Spending transaction becomes invalid
//   - Wallet doesn't crash
```

### Mempool Impact

Test mempool interaction:
```cpp
// Mine block with coinbase
// Broadcast transaction spending coinbase
// Reorg orphans the block
// Verify transaction removed from mempool
```

## Conclusion

**T9 PASSED** ✅ (after bug fix)

Orphaned coinbase removal verified:
1. ✅ Coinbase UTXO added when block mined
2. ✅ Reorg triggered (onBlockDisconnected called)
3. ✅ Coinbase UTXO removed from wallet database
4. ✅ Balance reverted to initial state (0 DIN)
5. ✅ No phantom balance from orphaned blocks
6. ✅ Invariant W.5 validated (orphaned rewards removed)

**Bug Fix**:
- Found: SQL queries referenced non-existent `wallet_id` column
- Fixed: Removed `wallet_id` from 4 SQL queries
- Result: Reorg handling now works correctly

**Impact**:
This test (and bug fix) ensures DineroCoin wallets correctly handle blockchain reorganizations, preventing phantom balances from orphaned coinbase outputs. Critical for miners and all users who may encounter reorgs.

**Certification**:
Phase F.7 now complete - **9/9 P0 tests passing (100%)** 🎉

---

**Test Execution Time**: < 1 second
**Exit Code**: 0 (success)
**Test File**: `tests/wallet_persistence/standalone_test_t9.cpp`
**Build**: `standalone_test_t9` target in CMake

**Phase F.7 Progress**: 9/9 P0 tests passing (**100%** COMPLETE) ✅
