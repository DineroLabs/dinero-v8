# Consensus Confidence

This document describes what the DineroCoin cold-start consensus tests guarantee.

## Important: Both Tiers Are Consensus-Critical

**Utreexo is NOT optional in DineroCoin.** It is consensus-critical from genesis,
baked into the 128-byte header design, and enforced from block 0.

The test tiers are separated for **signal isolation** (diagnosability), not optionality.
A failure in either tier means consensus is broken.

| Tier | Purpose |
|------|---------|
| Tier-1 | Base consensus invariants (excluding accumulator semantics) |
| Tier-2 | Accumulator (Utreexo) invariants on top of the same consensus |

Both tiers are **required CI gates**. There is no "UTXO-only canonical chain" in DineroCoin.

## Test Tiers

### Tier 1: Base Consensus (`cold_start_test.sh`)

Validates fundamental consensus behavior (excluding accumulator-specific checks):

| Guarantee | Description |
|-----------|-------------|
| **P2P Header Sync** | `FindHeadersToSend()` correctly responds to `getheaders` messages |
| **Block Sync** | Blocks propagate correctly via P2P |
| **Tip Convergence** | All nodes converge to the same best chain |
| **Chainwork Consistency** | `GetNextWorkRequired()` produces identical results across nodes |
| **Restart Persistence** | Chain state survives daemon restart without corruption |
| **Reindex Equivalence** | Reindexed chain produces identical state to original |
| **Premine Verification** | Block 1 (premine) is validated during sync |
| **Signature Verification** | Transaction signatures are validated during sync |

**Run command:**
```bash
./tests/integration/cold_start_test.sh
```

### Tier 2: Accumulator Consensus (`cold_start_utreexo.sh`)

**CONSENSUS-CRITICAL** - Validates Utreexo accumulator invariants:

| Guarantee | Description |
|-----------|-------------|
| **Utreexo Root Equality** | All nodes compute identical accumulator roots |
| **Utreexo Persistence** | Accumulator state survives restart |
| **Utreexo Reindex Stability** | Reindex produces identical accumulator root |
| **Proof Verification** | Utreexo proofs are validated during sync |

This is NOT optional. Utreexo roots are header-committed in DineroCoin.
A node that diverges on Utreexo state is consensus-invalid.

**Run command:**
```bash
./tests/integration/cold_start_utreexo.sh
```

### Negative Tests

#### `test_bad_diffbits.sh`

Proves that blocks with invalid difficulty (nBits) are **always rejected**.

This permanently locks:
- Mining difficulty enforcement
- Stratum template difficulty
- Reindex difficulty validation
- Any refactors touching difficulty code

**Run command:**
```bash
./tests/functional/test_bad_diffbits.sh
```

## CI Integration

Both Tier-1 and Tier-2 are **required CI gates**:

| Test | Trigger | Blocks |
|------|---------|--------|
| Tier-1 (Base) | Every push/PR | 20 |
| Tier-2 (Utreexo) | Every push/PR | 30 |
| bad-diffbits | Every push/PR | N/A |
| Extended (Nightly) | Schedule | 110 |

**All failures are stop-the-line events.** No `allow_failures` on any consensus test.

## What These Tests Do NOT Cover

These tests focus on consensus correctness, not:

- ❌ Network chaos (packet loss, slow peers)
- ❌ Adversarial Utreexo proofs
- ❌ Performance benchmarks
- ❌ Memory/disk exhaustion
- ❌ Fuzzing

Those are Tier-3 / soak tests for later stages.

## Test Flow

```
[STEP 1] Start 3 nodes (A=miner, B/C=sync via P2P)
[STEP 2] Mine N blocks on A
[STEP 3] Wait for B and C to sync
[STEP 4] Restart node B
[STEP 5] Verify B state unchanged
[STEP 6] Reindex node C
[STEP 7] Verify C state matches A
         ↓
    ✅ TEST PASSED
```

## When to Run

| Event | Test to Run |
|-------|-------------|
| Any consensus code change | Tier 1 (mandatory) |
| Utreexo code change | Tier 1 + Tier 2 |
| Difficulty/mining change | Tier 1 + bad-diffbits |
| Pre-release | All tiers |

## Adding New Tests

When adding consensus tests:

1. Keep tiers separate (don't mix base + Utreexo)
2. Test one thing per assertion
3. Make failures obvious (clear error messages)
4. Ensure cleanup on failure (trap handlers)

## Contact

For consensus questions, see the codebase or reach out to the core team.
