# DineroCoin Wallet Reorg Chaos Testing

## Overview

This directory contains production-grade chaos tests that prove **wallet safety during blockchain reorganizations (reorgs)** when:
- Confirmed transactions become unconfirmed
- Transactions disappear from the chain
- New competing transactions appear
- Block depths change
- UTXOs change confirmation status

This is **exchange-grade reorg resilience** testing.

## Core Safety Principle

**Every test is invariant-based, never expectation-based.**

- No assumptions about reorg depth
- No assumptions about timing
- No assumptions about which chain wins
- Only hard assertions about fund conservation across reorgs

## What These Tests Prove

When complete, this framework empirically demonstrates:

✅ **No Fund Loss During Reorgs** - Funds preserved across chain switches
✅ **No Fund Duplication** - Reorgs don't create phantom coins
✅ **Transaction State Tracking** - Wallet correctly updates tx confirmations
✅ **UTXO Maturity Handling** - Coinbase maturity respected after reorgs
✅ **Balance Consistency** - Balance reflects actual spendable funds
✅ **No Orphaned Spends** - Transactions built on reorg'd blocks handled correctly

---

## Reorg Scenarios

Each test cycle executes exactly **one randomly selected reorg scenario**, then validates wallet state.

### Scenario A — Simple Reorg (1-Block)

**Setup:**
- Mine block N with tx1 (wallet receives funds)
- Confirm tx1 (1 confirmation)
- **Trigger:** Mine competing chain that excludes tx1
- Reorg depth: 1 block

**Invariant checks after reorg:**
- ✅ tx1 returns to mempool OR disappears
- ✅ Balance reflects unconfirmed status
- ✅ No fund duplication
- ✅ Wallet recognizes reorg

### Scenario B — Deep Reorg (6+ Blocks)

**Setup:**
- Mine 6 blocks with tx1 in block N
- tx1 has 6 confirmations
- **Trigger:** Mine competing 7-block chain without tx1
- Reorg depth: 6 blocks

**Invariants:**
- ✅ tx1 unconfirmed or back in mempool
- ✅ All dependent transactions invalidated
- ✅ Balance updated correctly
- ✅ No stuck UTXOs

### Scenario C — Coinbase Reorg (Maturity Test)

**Setup:**
- Mine block N with coinbase (100 DIN reward)
- Mine 100+ blocks for maturity
- Spend coinbase in tx1
- **Trigger:** Reorg before coinbase block
- Coinbase disappears

**Invariants:**
- ✅ Coinbase UTXO removed
- ✅ Spending tx1 becomes invalid
- ✅ Balance decreased by 100 DIN
- ✅ No orphaned child transactions
- ✅ Wallet handles gracefully

### Scenario D — Double-Spend via Reorg

**Setup:**
- Mine block N with tx1 (sends 10 DIN to addressA)
- Confirm tx1 (3 confirmations)
- **Trigger:** Mine competing chain with tx2 (sends same UTXO to addressB)
- Reorg: tx1 disappears, tx2 confirmed

**Invariants:**
- ✅ Wallet recognizes tx1 is invalid
- ✅ Only tx2 is confirmed
- ✅ No duplicate spends
- ✅ Balance reflects tx2, not tx1
- ✅ Exactly one transaction wins

### Scenario E — Reorg with Mempool Tx

**Setup:**
- Create tx1, broadcast to mempool (unconfirmed)
- Mine competing blocks that exclude tx1
- **Trigger:** Reorg causes chain switch
- tx1 remains unconfirmed

**Invariants:**
- ✅ tx1 still in mempool
- ✅ Balance unchanged (was already unconfirmed)
- ✅ tx1 eventually mines or expires
- ✅ No state corruption

### Scenario F — Cascading Reorg (Child Transactions)

**Setup:**
- Mine block N with tx1 (parent)
- tx1 confirmed, creates UTXO1
- Create tx2 spending UTXO1 (child)
- Mine tx2 in block N+1
- **Trigger:** Reorg removes block N
- Both tx1 and tx2 become invalid

**Invariants:**
- ✅ tx2 invalidated (parent missing)
- ✅ tx1 returns to mempool
- ✅ No orphaned child transactions
- ✅ Balance reflects only confirmed UTXOs
- ✅ Wallet chain dependencies respected

---

## Reorg State Model

After any reorg, wallet must reconcile to **exactly ONE state** per transaction:

| State | Description |
|-------|-------------|
| **CONFIRMED** | Transaction in active chain |
| **UNCONFIRMED** | Transaction in mempool, 0 confirmations |
| **CONFLICTED** | Transaction conflicts with chain (double-spend) |
| **ABANDONED** | Transaction will never confirm (orphaned) |

❌ **Any ambiguous state = test failure**

---

## Extended Wallet Oracle for Reorgs

`wallet_reorg_oracle.cpp` provides reorg-specific validation:

### New Truth Assertions

```cpp
assert_no_fund_loss_across_reorg();     // Balance conservation
assert_no_fund_duplication();           // No phantom coins
assert_tx_state_consistent();           // Exactly one state
assert_utxo_maturity_respected();       // Coinbase maturity rules
assert_no_orphaned_spends();            // Child tx handling
assert_balance_equals_utxo_sum();       // Reconciliation check
```

### Usage

```bash
# Capture wallet state before reorg
wallet_reorg_oracle snapshot wallet.db > before_reorg.json

# Trigger reorg
invalidateblock <block_hash>
reconsiderblock <block_hash>

# Capture wallet state after reorg
wallet_reorg_oracle snapshot wallet.db > after_reorg.json

# Validate reorg handling
wallet_reorg_oracle validate_reorg before_reorg.json after_reorg.json
```

---

## Test Scripts

### `wallet_reorg_quick_test.sh` - CI-Friendly Test

**Purpose:** Fast validation for CI/CD pipelines

**Configuration:**
- **Duration:** ~10-15 minutes
- **Reorg Cycles:** 5
- **Scenarios:** Random selection from A-F
- **Reorg Depths:** 1-6 blocks

**Usage:**
```bash
./tests/wallet/chaos_reorg/wallet_reorg_quick_test.sh
```

---

### `wallet_reorg_soak_test.sh` - Production Validation

**Purpose:** Comprehensive proof of reorg safety

**Configuration:**
- **Duration:** ~1-2 hours
- **Reorg Cycles:** 25
- **Scenarios:** Random selection from A-F
- **Reorg Depths:** 1-10 blocks
- **Deep Reorgs:** At least 5 cycles with depth > 6

**Usage:**
```bash
./tests/wallet/chaos_reorg/wallet_reorg_soak_test.sh
```

---

## Cycle Structure

```bash
for cycle in 1..25:
  mine_initial_chain()           # Create base chain
  fund_wallet()                  # Ensure spendable balance
  snapshot_wallet_state()        # Capture pre-reorg state
  choose_random_scenario(A–F)    # Select ONE scenario
  setup_competing_chain()        # Create alternate chain
  trigger_reorg()                # invalidateblock + longer chain
  wait_for_reorg_completion()    # Ensure chain switch complete
  snapshot_wallet_state()        # Capture post-reorg state
  oracle.validate_all()          # Run all assertions
```

---

## Forbidden Behaviors (Immediate Failure)

If **any** of these occur, stop and investigate:

❌ Balance changes without matching tx state change
❌ Confirmed tx with 0 confirmations
❌ Transaction in BOTH confirmed and unconfirmed states
❌ Orphaned child tx still marked as spendable
❌ Wallet crashes during reorg
❌ SQLite corruption
❌ Fund duplication (same UTXO counted twice)

---

## Success Criteria (Binary)

You may claim success **only if:**

✅ **25/25 reorg cycles complete with zero fund loss, zero duplication, zero corruption, zero crashes.**

Anything less = not proven.

---

## What This Unlocks If It Passes

If this framework passes 25/25 cycles, you can credibly state:

> **"DineroCoin wallets are provably safe during blockchain reorganizations of arbitrary depth."**

That is **exchange-listing language** for reorg resilience.

---

## Reorg Trigger Methods

### Method 1: `invalidateblock` + Competing Chain

```bash
# Get current tip
tip=$(bitcoin-cli getbestblockhash)

# Invalidate tip to trigger reorg
bitcoin-cli invalidateblock $tip

# Mine competing chain (longer)
bitcoin-cli generatetoaddress 2 <address>

# Reconsider (optional - automatic in most cases)
bitcoin-cli reconsiderblock $tip
```

### Method 2: Parallel Mining (Natural Reorg)

```bash
# Mine on isolated node A
node_a: mine blocks 1000-1005

# Mine competing chain on isolated node B
node_b: mine blocks 1000-1007 (longer)

# Connect nodes - node_a reorgs to node_b chain
connect node_a to node_b
```

---

## Comparison to Other Wallet Chaos Tests

| Framework | Focus | Status |
|-----------|-------|--------|
| `tests/wallet/chaos/` | Address generation, balance checks | ✅ PROVEN (30 crashes) |
| `tests/wallet/chaos_funds/` | Spending operations under SIGKILL | ✅ PROVEN (25 crashes) |
| `tests/wallet/chaos_reorg/` | **Blockchain reorganizations** | 🔨 THIS FRAMEWORK |

**Why separate frameworks?**
- Different risk surfaces
- Reorgs require multi-chain setup
- Transaction confirmation semantics
- Temporal reasoning (confirmations decrease)

---

## CI/CD Integration

### Quick Test (Recommended for CI)

```yaml
# .github/workflows/wallet-reorg-chaos-test.yml
name: Wallet Reorg Chaos Testing
on: [push, pull_request]

jobs:
  wallet-reorg-chaos-quick:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build DineroCoin
        run: |
          g++ -std=c++17 -o tests/wallet/chaos_reorg/wallet_reorg_oracle \
              tests/wallet/chaos_reorg/wallet_reorg_oracle.cpp -lsqlite3
      - name: Run Quick Wallet Reorg Chaos Test
        run: ./tests/wallet/chaos_reorg/wallet_reorg_quick_test.sh
      - name: Archive test logs
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: wallet-reorg-chaos-logs
          path: /tmp/wallet_reorg_chaos_*/
```

---

## Development Workflow

### Before Committing Wallet/Chain Changes

```bash
# Quick validation (10-15 min)
./tests/wallet/chaos_reorg/wallet_reorg_quick_test.sh
```

### Before Wallet-Related Releases

```bash
# Full soak test (1-2 hours)
./tests/wallet/chaos_reorg/wallet_reorg_soak_test.sh

# Archive evidence
tar czf wallet_reorg_chaos_evidence.tgz ~/dinero_wallet_reorg_soak_*.tgz
```

---

## Failure Mode Examples

### Fund Loss During Reorg

```
❌ FATAL: Fund loss after reorg!
  Before reorg: 250.0 DIN
  After reorg:  240.0 DIN
  Loss:         10.0 DIN
  Missing UTXO: abc123...:0
```

**Action:** Investigate transaction tracking across reorg

### Duplicate Transaction State

```
❌ FATAL: Transaction in multiple states!
  TXID: def456...
  State 1: CONFIRMED (3 confirmations)
  State 2: CONFLICTED
  This violates state model
```

**Action:** Critical - transaction state machine broken

### Orphaned Child Transaction

```
❌ FATAL: Child tx still spendable after parent reorg'd!
  Parent TXID: abc123... (CONFLICTED)
  Child TXID:  def456... (CONFIRMED - INVALID)
  Child spent parent UTXO that no longer exists
```

**Action:** Check transaction dependency tracking

### Confirmation Count Anomaly

```
❌ FATAL: Confirmation count increased after reorg!
  Before reorg: tx1 has 5 confirmations
  After reorg:  tx1 has 7 confirmations
  Confirmations should decrease or reset to 0
```

**Action:** Investigate block height tracking

---

## Future Enhancements

Potential additions:

1. **Multi-Reorg Chaos** - Multiple reorgs in succession
2. **Reorg + SIGKILL** - Crash during reorg handling
3. **Reorg + RBF** - Replace-by-fee during reorg
4. **Reorg + Mempool Eviction** - Combined scenarios
5. **Deep Reorg Stress** - 100+ block reorgs

---

## Technical Notes

### Reorg Detection

Wallet must detect reorgs via:
- Block height decrease
- Best block hash change
- Transaction confirmation count decrease
- ChainActive tip change

### Reorg Response

Proper wallet behavior:
1. Scan reorg'd blocks for wallet transactions
2. Update transaction confirmations
3. Re-evaluate UTXO spendability
4. Update balance
5. Rebroadcast affected mempool txs

### Edge Cases

- **Coinbase reorg:** All dependent spends invalidated
- **Deep reorg:** Multiple transactions affected
- **Double-spend:** One tx confirmed, conflicting tx rejected
- **Mempool churn:** Transactions re-entering mempool

---

## Credits

**Design:** Based on Bitcoin Core reorg handling + production chaos engineering
**Implementation:** DineroCoin development team
**Inspired by:** Wallet crash/spending chaos frameworks
**Related:** Exchange integration safety requirements

---

## License

This testing framework is part of DineroCoin and follows the same license terms.
