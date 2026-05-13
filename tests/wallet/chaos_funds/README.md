# DineroCoin Wallet-With-Funds Chaos Testing

## Overview

This directory contains production-grade chaos tests that prove **real funds cannot be lost, duplicated, or corrupted** when the daemon is SIGKILL'd during:
- Transaction creation
- Transaction signing
- Transaction broadcast
- Mempool acceptance
- Confirmation tracking

This is **exchange-grade risk surface** testing.

## Core Safety Principle

**Every test is invariant-based, never expectation-based.**

- No assumptions about timing
- No assumptions about mempool behavior
- No assumptions about ordering
- Only hard assertions about fund conservation

## What These Tests Prove

When complete, this framework empirically demonstrates:

✅ **No Fund Loss** - Funds cannot vanish during crashes
✅ **No Fund Duplication** - Funds cannot be created from nothing
✅ **No Partial Spends** - UTXOs cannot get "stuck" in limbo
✅ **Transaction State Consistency** - Txs resolve to exactly ONE state
✅ **UTXO Conservation** - Inputs always accounted for

---

## Test Phases (One Per Crash Cycle)

Each crash cycle executes exactly **one randomly selected phase**, then SIGKILLs the daemon.

### Phase A — Spend Construction

**Crash injected during:**
- Coin selection
- Change output calculation
- Fee estimation

**Invariant checks after restart:**
- ✅ No balance decrease
- ✅ No UTXO removed
- ✅ No "half-spend" markers
- ✅ Transaction does not exist anywhere

### Phase B — Signing

**Crash injected during:**
- Private key access
- scriptSig / witness generation
- PSBT finalization

**Invariants:**
- ✅ Inputs remain unspent
- ✅ No partial signatures stored
- ✅ No invalid tx in wallet DB
- ✅ Keys intact

### Phase C — Broadcast (Pre-RPC)

**Crash injected:**
- Just before `sendrawtransaction`

**Invariants:**
- ✅ No mempool tx
- ✅ Inputs unspent
- ✅ Wallet history unchanged

### Phase D — Broadcast (Post-RPC)

**Crash injected:**
- Immediately after broadcast

**Invariants:**
- ✅ Exactly one of:
  - tx exists in mempool
  - tx does not exist at all
- ✅ Never both
- ✅ Wallet balance reflects exactly one state

### Phase E — Confirmation Window

**Crash injected:**
- After tx enters mempool
- Before confirmation

**Invariants:**
- ✅ If confirmed: balance updated exactly once
- ✅ If unconfirmed: inputs locked, not lost
- ✅ No duplicate tx entries
- ✅ No rescan

---

## Transaction State Model

Every transaction must resolve to **exactly ONE state**:

| State | Description |
|-------|-------------|
| **ABSENT** | Transaction does not exist |
| **MEMPOOL** | In mempool only |
| **CONFIRMED** | In blockchain |
| **FAILED** | Explicitly rejected/abandoned |

❌ **Any hybrid state = test failure**

---

## Funding Strategy (Critical)

Uses deterministic, isolated funds:

- ✅ Dedicated test wallet
- ✅ Single UTXO per cycle
- ✅ Known amount (e.g., 10 DIN)
- ✅ One spend per crash

**Why?**
- Eliminates ambiguity
- Makes loss mathematically provable
- Easy reconciliation

---

## Wallet Oracle Extensions

`wallet_funds_oracle.cpp` provides transaction-specific validation:

### New Truth Assertions

```cpp
assert_no_fund_loss();          // Balance conservation
assert_no_fund_duplication();   // No phantom coins
assert_no_partial_spend();      // No stuck UTXOs
assert_utxo_conservation();     // Inputs accounted for
assert_tx_state_consistent();   // Exactly one state
```

### Usage

```bash
# Capture wallet state with transaction tracking
wallet_funds_oracle snapshot wallet.db > state.json

# Validate spend operation
wallet_funds_oracle validate_spend before.db after.db <txid>

# Check transaction state consistency
wallet_funds_oracle check_tx_state <txid>
```

---

## Test Scripts

### `wallet_spend_quick_test.sh` - CI-Friendly Test

**Purpose:** Fast validation for CI/CD pipelines

**Configuration:**
- **Duration:** ~10-15 minutes
- **Crash Cycles:** 5
- **Phases:** Random selection from A-E

**Usage:**
```bash
./tests/wallet/chaos_funds/wallet_spend_quick_test.sh
```

---

### `wallet_spend_soak_test.sh` - Production Validation

**Purpose:** Comprehensive proof of fund safety

**Configuration:**
- **Duration:** ~1-2 hours
- **Crash Cycles:** 25
- **Phases:** Random selection from A-E
- **Funding:** 250+ DIN initial balance (for 25 spends)

**Usage:**
```bash
./tests/wallet/chaos_funds/wallet_spend_soak_test.sh
```

---

## Cycle Structure

```bash
for cycle in 1..25:
  ensure_wallet_funded()      # Mine if needed
  snapshot_wallet_state()     # Capture pre-crash state
  choose_random_phase(A–E)    # Select ONE phase
  initiate_spend()            # Start transaction operation
  SIGKILL_daemon()            # Hard kill
  restart_daemon()            # Clean restart
  oracle.validate_all()       # Run all assertions
```

---

## Forbidden Behaviors (Immediate Failure)

If **any** of these occur, stop and investigate:

❌ Balance decreases without confirmed tx
❌ UTXO disappears
❌ Same tx appears twice
❌ Wallet rescans
❌ SQLite corruption
❌ Key index regression

---

## Success Criteria (Binary)

You may claim success **only if:**

✅ **25/25 SIGKILL crashes with real funds complete with zero fund loss, zero duplication, zero corruption, zero rescans.**

Anything less = not proven.

---

## What This Unlocks If It Passes

If this framework passes 25/25 cycles, you can credibly state:

> **"DineroCoin wallets are provably crash-safe during live fund spends under adversarial termination."**

That is **exchange-listing language**.

---

## Comparison to Basic Wallet Chaos Tests

| Framework | Focus | Status |
|-----------|-------|--------|
| `tests/wallet/chaos/` | Address generation, balance checks | ✅ PROVEN (30 crashes) |
| `tests/wallet/chaos_funds/` | **Spending real funds** | 🔨 THIS FRAMEWORK |

**Why separate frameworks?**
- Different risk surfaces
- Spending requires mature UTXOs (100+ blocks)
- Transaction state is more complex
- Fund loss is permanent

---

## CI/CD Integration

### Quick Test (Recommended for CI)

```yaml
# .github/workflows/wallet-funds-chaos-test.yml
name: Wallet Funds Chaos Testing
on: [push, pull_request]

jobs:
  wallet-funds-chaos-quick:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build DineroCoin
        run: |
          g++ -std=c++17 -o tests/wallet/chaos_funds/wallet_funds_oracle \
              tests/wallet/chaos_funds/wallet_funds_oracle.cpp -lsqlite3
      - name: Run Quick Wallet Funds Chaos Test
        run: ./tests/wallet/chaos_funds/wallet_spend_quick_test.sh
      - name: Archive test logs
        if: failure()
        uses: actions/upload-artifact@v3
        with:
          name: wallet-funds-chaos-logs
          path: /tmp/wallet_spending_chaos_*/
```

### Nightly Full Test

```yaml
# .github/workflows/wallet-funds-chaos-nightly.yml
name: Nightly Wallet Funds Soak Test
on:
  schedule:
    - cron: '0 4 * * *'  # 4 AM daily

jobs:
  wallet-funds-chaos-full:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build DineroCoin
        run: |
          g++ -std=c++17 -o tests/wallet/chaos_funds/wallet_funds_oracle \
              tests/wallet/chaos_funds/wallet_funds_oracle.cpp -lsqlite3
      - name: Run Full Wallet Funds Soak Test
        run: ./tests/wallet/chaos_funds/wallet_spend_soak_test.sh
      - name: Archive evidence
        uses: actions/upload-artifact@v3
        with:
          name: wallet-funds-soak-evidence
          path: ~/dinero_wallet_funds_soak_*.tgz
```

---

## Development Workflow

### Before Committing Wallet Changes

```bash
# Quick validation (10-15 min)
./tests/wallet/chaos_funds/wallet_spend_quick_test.sh
```

### Before Wallet-Related Releases

```bash
# Full soak test (1-2 hours)
./tests/wallet/chaos_funds/wallet_spend_soak_test.sh

# Archive evidence
tar czf wallet_funds_chaos_evidence.tgz ~/dinero_wallet_funds_soak_*.tgz
```

---

## Failure Mode Examples

### Fund Loss

```
❌ FATAL: Fund loss detected!
  Before: 25000000000 una (250 DIN)
  After:  24000000000 una (240 DIN)
  Loss:   1000000000 una (10 DIN)
```

**Action:** Investigate transaction logs, check for unaccounted spends

### Duplicate UTXO

```
❌ FATAL: Duplicate UTXO detected!
  UTXO: abc123...:0 appears twice in wallet DB
```

**Action:** Database corruption - investigate write path

### Hybrid Transaction State

```
❌ FATAL: Transaction in BOTH mempool and chain!
  TXID: def456...
  This is a consensus violation
```

**Action:** Critical - mempool / chain sync bug

### Partial Spend

```
❌ FATAL: Balance mismatch!
  Balance: 24000000000 una
  UTXO sum: 25000000000 una
  Difference indicates partial spend
```

**Action:** UTXO marking logic corrupted

---

## Future Enhancements

Potential additions:

1. **RBF Chaos** - Replace-by-fee during crashes
2. **CPFP Chaos** - Child-pays-for-parent during crashes
3. **Multi-Signature Spending** - Partial signature sets
4. **Batch Spending** - Multiple outputs, single crash
5. **Reorg + Spend Chaos** - Fork scenarios during spending

---

## Credits

**Design:** Based on production chaos engineering + Bitcoin Core wallet safety principles
**Implementation:** DineroCoin development team
**Inspired by:** `tests/wallet/chaos/` (address generation chaos framework)
**Related:** Wallet persistence milestone (v2.3.0+)

---

## License

This testing framework is part of DineroCoin and follows the same license terms.
