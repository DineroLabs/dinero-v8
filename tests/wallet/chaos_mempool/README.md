# DineroCoin Wallet Mempool Eviction Chaos Testing

## Overview

This directory contains production-grade chaos tests that prove **wallet safety during mempool eviction events** when:
- Transactions are evicted due to mempool size limits
- Low-fee transactions are replaced by higher-fee ones
- Transactions expire from mempool after timeout
- Mempool is cleared during daemon restart
- Conflicting transactions cause evictions

This is **exchange-grade mempool resilience** testing.

## Core Safety Principle

**Every test is invariant-based, never expectation-based.**

- No assumptions about mempool capacity
- No assumptions about eviction policy
- No assumptions about transaction lifetime
- Only hard assertions about fund tracking during mempool churn

## What These Tests Prove

When complete, this framework empirically demonstrates:

✅ **No Fund Loss During Eviction** - Funds tracked correctly when tx evicted
✅ **Balance Reflects Reality** - Unconfirmed balance updates on eviction
✅ **Transaction State Tracking** - Wallet knows when tx is evicted
✅ **Rebroadcast Handling** - Evicted txs can be rebroadcast
✅ **UTXO Locking** - UTXOs released when tx evicted
✅ **No Ghost Transactions** - Evicted txs don't appear as confirmed

---

## Mempool Eviction Scenarios

Each test cycle executes exactly **one randomly selected eviction scenario**, then validates wallet state.

### Scenario A — Size-Based Eviction

**Setup:**
- Fill mempool to near capacity
- Create wallet tx1 with standard fee
- Create many higher-fee transactions
- **Trigger:** Mempool full, tx1 evicted (lowest fee)

**Invariant checks after eviction:**
- ✅ tx1 marked as evicted/abandoned
- ✅ Balance no longer includes tx1
- ✅ UTXOs from tx1 are relocked
- ✅ Wallet can recreate tx1 with higher fee

### Scenario B — Fee-Based Eviction (RBF)

**Setup:**
- Create tx1 with low fee (1 sat/byte)
- Broadcast to mempool
- Create tx2 (same inputs, higher fee)
- **Trigger:** tx2 replaces tx1 (RBF)

**Invariants:**
- ✅ tx1 marked as conflicted
- ✅ tx2 in mempool
- ✅ Balance reflects tx2, not tx1
- ✅ Only one tx tracked per UTXO

### Scenario C — Expiry-Based Eviction

**Setup:**
- Create tx1 with standard fee
- Broadcast to mempool
- Wait for mempool expiry (default: 2 weeks)
- **Trigger:** tx1 expires from mempool

**Invariants:**
- ✅ tx1 marked as abandoned
- ✅ Balance updated (tx1 no longer pending)
- ✅ UTXOs available for new spend
- ✅ Wallet doesn't wait indefinitely for confirmation

### Scenario D — Restart-Based Eviction

**Setup:**
- Create tx1, broadcast to mempool
- tx1 unconfirmed in mempool
- **Trigger:** Restart daemon (mempool cleared)

**Invariants:**
- ✅ Wallet detects tx1 missing from mempool
- ✅ Optionally rebroadcasts tx1
- ✅ Balance consistent with mempool state
- ✅ No corruption from mempool loss

### Scenario E — Conflict-Based Eviction

**Setup:**
- Create tx1 spending UTXO1
- Broadcast tx1 to mempool
- Create conflicting tx2 (different outputs, same input)
- Mine tx2 in a block
- **Trigger:** tx1 evicted (conflicts with confirmed tx2)

**Invariants:**
- ✅ tx1 marked as conflicted
- ✅ tx2 confirmed
- ✅ Balance reflects tx2
- ✅ No double-spend confusion

### Scenario F — Cascade Eviction (Parent Evicted)

**Setup:**
- Create tx1 (parent) in mempool
- Create tx2 (child) spending tx1 output
- Both in mempool, unconfirmed
- **Trigger:** tx1 evicted → tx2 must be evicted too

**Invariants:**
- ✅ tx2 evicted when tx1 evicted (dependency)
- ✅ No orphaned child transactions
- ✅ Balance updated for both
- ✅ UTXOs correctly tracked

---

## Mempool State Model

After any eviction, wallet must reconcile to **exactly ONE state** per transaction:

| State | Description |
|-------|-------------|
| **IN_MEMPOOL** | Transaction in mempool, unconfirmed |
| **EVICTED** | Transaction evicted from mempool |
| **EXPIRED** | Transaction expired (timeout) |
| **CONFLICTED** | Transaction conflicts with another (RBF/double-spend) |
| **CONFIRMED** | Transaction mined in a block |

❌ **Any ambiguous state = test failure**

---

## Extended Wallet Oracle for Mempool

`wallet_mempool_oracle.cpp` provides mempool-specific validation:

### New Truth Assertions

```cpp
assert_no_fund_loss_after_eviction();    // Balance conservation
assert_no_ghost_confirmations();         // Evicted tx not confirmed
assert_utxo_lock_released();             // UTXOs freed on eviction
assert_conflicting_tx_handled();         // Only one tx per UTXO
assert_child_evicted_with_parent();      // Dependency tracking
assert_balance_matches_mempool();        // Reconciliation
```

### Usage

```bash
# Capture wallet state before eviction
wallet_mempool_oracle snapshot wallet.db > before_eviction.json

# Trigger eviction (various methods)
# ... eviction happens ...

# Capture wallet state after eviction
wallet_mempool_oracle snapshot wallet.db > after_eviction.json

# Validate eviction handling
wallet_mempool_oracle validate_eviction before.json after.json <txid>
```

---

## Test Scripts

### `wallet_mempool_quick_test.sh` - CI-Friendly Test

**Purpose:** Fast validation for CI/CD pipelines

**Configuration:**
- **Duration:** ~10-15 minutes
- **Eviction Cycles:** 5
- **Scenarios:** Random selection from A-F

**Usage:**
```bash
./tests/wallet/chaos_mempool/wallet_mempool_quick_test.sh
```

---

### `wallet_mempool_soak_test.sh` - Production Validation

**Purpose:** Comprehensive proof of mempool eviction safety

**Configuration:**
- **Duration:** ~1-2 hours
- **Eviction Cycles:** 25
- **Scenarios:** Random selection from A-F
- **Mempool stress:** Fill and evict repeatedly

**Usage:**
```bash
./tests/wallet/chaos_mempool/wallet_mempool_soak_test.sh
```

---

## Cycle Structure

```bash
for cycle in 1..25:
  ensure_wallet_funded()           # Ensure spendable balance
  snapshot_wallet_state()          # Capture pre-eviction state
  choose_random_scenario(A–F)      # Select ONE scenario
  create_mempool_tx()              # Create transaction
  trigger_eviction()               # Force eviction event
  wait_for_eviction_detection()   # Wallet detects eviction
  snapshot_wallet_state()          # Capture post-eviction state
  oracle.validate_all()            # Run all assertions
```

---

## Forbidden Behaviors (Immediate Failure)

If **any** of these occur, stop and investigate:

❌ Evicted transaction still shows as pending
❌ Balance includes evicted transaction
❌ UTXOs locked by evicted transaction
❌ Evicted transaction appears as confirmed
❌ Child transaction survives parent eviction
❌ Wallet crashes during eviction handling
❌ SQLite corruption

---

## Success Criteria (Binary)

You may claim success **only if:**

✅ **25/25 eviction cycles complete with zero fund loss, zero confusion, zero corruption.**

Anything less = not proven.

---

## What This Unlocks If It Passes

If this framework passes 25/25 cycles, you can credibly state:

> **"DineroCoin wallets correctly handle mempool evictions and maintain accurate balance tracking under mempool stress."**

That is **exchange-listing language** for mempool resilience.

---

## Eviction Trigger Methods

### Method 1: Fill Mempool (Size-Based)

```bash
# Get mempool info
getmempoolinfo

# Create many txs to fill mempool
for i in {1..1000}; do
  createrawtransaction ...
  sendrawtransaction ...
done

# Wallet tx with low fee gets evicted
```

### Method 2: RBF Replacement

```bash
# Create tx with RBF flag
createrawtransaction ... (with BIP125 flag)

# Broadcast
sendrawtransaction <tx1>

# Replace with higher fee
createrawtransaction ... (same inputs, higher fee)
sendrawtransaction <tx2>

# tx1 evicted, tx2 in mempool
```

### Method 3: Expiry (Time-Based)

```bash
# Set low mempool expiry time (for testing)
# bitcoind -mempoolexpiry=1 (1 hour)

# Create tx, wait for expiry
# tx automatically evicted after timeout
```

### Method 4: Daemon Restart

```bash
# Create tx in mempool
sendrawtransaction <tx>

# Restart daemon (non-persistent mempool)
stop daemon
start daemon

# Mempool cleared, tx lost (unless rebroadcast)
```

---

## Comparison to Other Wallet Chaos Tests

| Framework | Focus | Status |
|-----------|-------|--------|
| `tests/wallet/chaos/` | Address generation, balance checks | ✅ PROVEN (30 crashes) |
| `tests/wallet/chaos_funds/` | Spending operations under SIGKILL | ✅ PROVEN (25 crashes) |
| `tests/wallet/chaos_reorg/` | Blockchain reorganizations | ✅ COMMITTED |
| `tests/wallet/chaos_mempool/` | **Mempool evictions** | 🔨 THIS FRAMEWORK |

**Why separate frameworks?**
- Different risk surfaces
- Mempool is non-consensus layer
- Eviction semantics vs confirmation semantics
- Transaction lifecycle tracking

---

## CI/CD Integration

### Quick Test (Recommended for CI)

```yaml
# .github/workflows/wallet-mempool-chaos-test.yml
name: Wallet Mempool Chaos Testing
on: [push, pull_request]

jobs:
  wallet-mempool-chaos-quick:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build DineroCoin
        run: |
          g++ -std=c++17 -o tests/wallet/chaos_mempool/wallet_mempool_oracle \
              tests/wallet/chaos_mempool/wallet_mempool_oracle.cpp -lsqlite3
      - name: Run Quick Wallet Mempool Chaos Test
        run: ./tests/wallet/chaos_mempool/wallet_mempool_quick_test.sh
      - name: Archive test logs
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: wallet-mempool-chaos-logs
          path: /tmp/wallet_mempool_chaos_*/
```

---

## Development Workflow

### Before Committing Mempool Changes

```bash
# Quick validation (10-15 min)
./tests/wallet/chaos_mempool/wallet_mempool_quick_test.sh
```

### Before Mempool-Related Releases

```bash
# Full soak test (1-2 hours)
./tests/wallet/chaos_mempool/wallet_mempool_soak_test.sh

# Archive evidence
tar czf wallet_mempool_chaos_evidence.tgz ~/dinero_wallet_mempool_soak_*.tgz
```

---

## Failure Mode Examples

### Evicted Transaction Still Pending

```
❌ FATAL: Evicted transaction still marked as pending!
  TXID: abc123...
  Mempool status: NOT FOUND
  Wallet status: PENDING (WRONG!)
  Expected: EVICTED or ABANDONED
```

**Action:** Check transaction state tracking on eviction

### Balance Includes Evicted Tx

```
❌ FATAL: Balance includes evicted transaction!
  TXID: def456... (evicted)
  Amount: 10 DIN
  Balance still shows +10 DIN
  Mempool: does not contain tx
```

**Action:** Investigate balance update logic

### UTXOs Still Locked

```
❌ FATAL: UTXOs locked by evicted transaction!
  TXID: abc123... (evicted)
  UTXOs: still marked as spent
  UTXOs should be released for new spend
```

**Action:** Check UTXO locking/unlocking on eviction

### Orphaned Child Transaction

```
❌ FATAL: Child tx remains after parent evicted!
  Parent: abc123... (evicted)
  Child:  def456... (still in mempool - INVALID)
  Child should cascade evict with parent
```

**Action:** Investigate transaction dependency tracking

---

## Technical Notes

### Mempool Eviction Detection

Wallet must detect evictions via:
- Periodic mempool sync
- Transaction missing from `getrawmempool`
- Conflict detection (RBF)
- Daemon restart detection

### Eviction Response

Proper wallet behavior:
1. Detect transaction no longer in mempool
2. Mark transaction as evicted/abandoned
3. Update balance (remove unconfirmed amount)
4. Release locked UTXOs
5. Optionally attempt rebroadcast with higher fee

### Edge Cases

- **RBF replacement:** One tx confirmed, conflicting tx rejected
- **Cascade eviction:** Parent evicted → children evicted
- **Restart eviction:** Mempool cleared, need rebroadcast
- **Expiry eviction:** Old txs cleaned up automatically

---

## Future Enhancements

Potential additions:

1. **Mempool + SIGKILL** - Crash during eviction handling
2. **Mempool + Reorg** - Combined scenarios
3. **Fee Bumping** - CPFP/RBF strategies
4. **Mempool Prioritization** - Ancestor/descendant limits
5. **Network Partition** - Mempool divergence across nodes

---

## Credits

**Design:** Based on Bitcoin Core mempool policy + production chaos engineering
**Implementation:** DineroCoin development team
**Inspired by:** Wallet crash/spending/reorg chaos frameworks
**Related:** Exchange integration safety requirements

---

## License

This testing framework is part of DineroCoin and follows the same license terms.
