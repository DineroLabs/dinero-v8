# Ring 1: Formal Verification Test Suite Design

## Overview

Ring 1 provides **mathematical proofs** of consensus-critical invariants using property-based testing. Unlike Ring 2 (wallet correctness with specific fixtures), Ring 1 uses **generative testing** to prove properties hold for ALL inputs.

## Design Philosophy

- **Property-Based**: Generate random inputs, verify invariants always hold
- **Mathematical Proofs**: Prove ∀x, property(x) = true (not just specific cases)
- **Consensus-Critical**: Focus on chain validity, supply, and UTXO set
- **Generative Testing**: QuickCheck/Hypothesis style (thousands of random inputs)
- **Orthogonal Tests**: Each test proves a different fundamental invariant

## Ring 1 Test Suite (3 Core Tests)

### Test 1: Supply Invariant (Mathematical Proof)
**Priority**: FIRST (most critical economic guarantee)

**Invariants to Prove**:
```
∀ height ∈ ℕ:
  1. totalSupply(height) ≤ MAX_SUPPLY (265,428,000 DIN)
  2. totalSupply(height) = genesis + premine + Σ(subsidy(epoch))
  3. totalSupply(height+1) ≥ totalSupply(height) (monotonic)
  4. subsidy(epoch) = initialSubsidy / 2^epoch (halving correctness)
```

**Property-Based Tests**:
- Generate 100,000 random heights → verify supply ≤ cap
- Generate random block sequences → verify cumulative supply matches formula
- Generate all halving boundaries → verify subsidy calculation exact
- Generate overflow heights (> final halving) → verify subsidy = 0

**Pass Criteria**:
- ✅ No height exceeds MAX_SUPPLY (tested on 100k random samples)
- ✅ Supply formula exact at all halving boundaries (33 halvings)
- ✅ Subsidy calculation exact (no floating point drift)
- ✅ Post-halving supply = 0 (tail behavior correct)

**Failure Modes**:
- ❌ Supply exceeds cap → Inflation bug (CRITICAL)
- ❌ Supply formula mismatch → Subsidy calculation wrong
- ❌ Non-monotonic supply → Block reward calculation bug

---

### Test 2: UTXO Set Invariant (State Machine Proof)
**Priority**: SECOND (ensures coins can't be created/destroyed)

**Invariants to Prove**:
```
∀ block B, utxo_set U:
  1. inputs(B) ⊆ U (can only spend existing UTXOs)
  2. U' = (U - inputs(B)) ∪ outputs(B) (state transition correct)
  3. value(inputs(B)) ≥ value(outputs(B)) + fee (no value creation)
  4. ∀ reorg: utxo_set can be reconstructed from genesis
```

**Property-Based Tests**:
- Generate 1,000 random transaction sequences → verify UTXO set consistency
- Generate random double-spends → verify rejection
- Generate random reorgs (2-10 blocks) → verify UTXO set reconstruction
- Generate random invalid inputs → verify rejection

**Pass Criteria**:
- ✅ UTXO set consistent after all random transaction sequences
- ✅ Double-spends always rejected
- ✅ UTXO set reconstructs correctly after reorgs
- ✅ Invalid inputs always rejected

**Failure Modes**:
- ❌ UTXO set inconsistent → State machine bug (CRITICAL)
- ❌ Double-spend accepted → Consensus failure
- ❌ Reorg reconstruction fails → Database corruption

---

### Test 3: Chain Selection Invariant (Ordering Proof)
**Priority**: THIRD (ensures canonical chain selection is deterministic)

**Invariants to Prove**:
```
∀ chain C:
  1. chainwork(C) = Σ(difficulty(block)) (cumulative work correct)
  2. canonical(C₁, C₂) = argmax(chainwork(C)) (most work wins)
  3. timestamp(block) ≥ median(prev_11_timestamps) (time ordering)
  4. difficulty(block) = adjustDifficulty(prev_2016_blocks) (retarget correct)
```

**Property-Based Tests**:
- Generate 100 random fork scenarios → verify most-work chain selected
- Generate random timestamp violations → verify rejection
- Generate random difficulty sequences → verify retarget calculation
- Generate random equal-work forks → verify tie-breaking deterministic

**Pass Criteria**:
- ✅ Most-work chain always selected (100 fork scenarios)
- ✅ Timestamp violations always rejected
- ✅ Difficulty retarget exact (no rounding errors)
- ✅ Equal-work ties broken deterministically (first-seen rule)

**Failure Modes**:
- ❌ Wrong chain selected → Fork choice bug (CRITICAL)
- ❌ Timestamp violation accepted → Time warp attack
- ❌ Difficulty calculation wrong → Mining exploitation

---

## Implementation Plan

### Phase 1: Property-Based Testing Framework
1. Create `PropertyTest` helper class (random input generation)
2. Implement `forAll<T>(generator, property)` pattern
3. Add statistical validation (run 1000+ iterations per property)

### Phase 2: Implement Test 1 (Supply Invariant)
1. Implement supply calculation property tests
2. Verify against existing `test_supply_cap.cpp` (integration)
3. Add overflow/underflow detection

### Phase 3: Implement Test 2 (UTXO Set Invariant)
1. Implement UTXO state machine property tests
2. Random transaction generator
3. Reorg simulation property tests

### Phase 4: Implement Test 3 (Chain Selection Invariant)
1. Implement fork choice property tests
2. Random fork generator (2-100 blocks)
3. Difficulty retarget property tests

## Comparison: Ring 1 vs Ring 2

| Aspect | Ring 1 (Formal Verification) | Ring 2 (Wallet Correctness) |
|--------|------------------------------|------------------------------|
| **Goal** | Prove consensus invariants hold ∀ inputs | Prove wallet state correct for specific cases |
| **Method** | Property-based (generative testing) | Fixture-based (specific test vectors) |
| **Coverage** | Thousands of random inputs | Deterministic test cases (1 UTXO) |
| **Scope** | Consensus-critical (chain, supply, UTXO) | Wallet features (restore, persistence, derivation) |
| **Priority** | MANDATORY (consensus failure = chain split) | OPTIONAL (wallet bugs = user impact only) |

## Success Criteria (Ring 1 Complete)

When all 3 Ring 1 tests pass with 1000+ random inputs each:
- ✅ Supply invariant mathematically proven
- ✅ UTXO set invariant mathematically proven
- ✅ Chain selection invariant mathematically proven
- ✅ No consensus bugs found in 100,000+ random scenarios

## Build & Run

```bash
cmake .. -DENABLE_CONSENSUS_FORMAL_VERIFICATION=ON
cmake --build build --target test_consensus_formal_verification
./build/test_consensus_formal_verification
```

Expected output:
```
[Test 1] Supply Invariant
  [✓] 100,000 random heights tested - supply ≤ cap
  [✓] 33 halving boundaries - exact subsidy calculation
  [✓] Overflow heights - subsidy = 0 (tail behavior correct)

[Test 2] UTXO Set Invariant
  [✓] 1,000 random transaction sequences - UTXO set consistent
  [✓] 500 random reorgs - reconstruction successful
  [✓] 10,000 random double-spends - all rejected

[Test 3] Chain Selection Invariant
  [✓] 100 random forks - most-work chain selected
  [✓] 1,000 timestamp tests - violations rejected
  [✓] 50 difficulty retargets - calculation exact

Ring 1 Complete: All consensus invariants proven ✅
```

---

**Next Steps**: Shall I implement Ring 1 Test 1 (Supply Invariant)?
