# Ring 4 Phase 4a: Mining Correctness & Liveness Formal Design

**Date**: 2026-01-02
**Status**: DESIGN (No Implementation Yet)
**Purpose**: Define what "correct mining" means before touching mining code

---

## Executive Summary

Ring 4 extends Dinero's formal verification from consensus and protocol to **mining correctness and liveness**.

**Previous rings proved**:
- ✅ Ring 1: Consensus rules (supply, UTXO, chain selection)
- ✅ Ring 2: Wallet correctness (balance, spending, persistence)
- ✅ Ring 3: P2P protocol (handshake, message ordering, state machine)

**Ring 4 will prove** (future work):
- Mining never violates consensus
- Mining never creates inflation
- Mining remains live under failure
- Mining behaves deterministically

**Phase 4a Goal**: Define what these properties mean, without touching mining code yet.

---

## 1. Definitions

### 1.1 What Is "Correct Mining"?

**Definition**: Mining is **correct** if and only if:
```
∀ blocks B produced by miner:
  1. B satisfies all consensus rules
  2. B.coinbase.subsidy = consensus_subsidy(B.height)
  3. B.coinbase.fees ≤ sum(tx.fee for tx in B)
  4. B.transactions are valid in context of B.prev_block
```

**Informal**: A correct miner never produces an invalid block (modulo PoW).

**Critical insight**: Correctness is **independent of whether the block is mined**.
- A valid template that never finds PoW is still correct
- An invalid template that finds PoW immediately is still incorrect

### 1.2 What Is "Safe Mining"?

**Definition**: Mining is **safe** if and only if:
```
∀ events E (restart, reorg, crash, race):
  1. No inflation is created
  2. No duplicate subsidy across blocks
  3. No invalid transaction inclusion
  4. No consensus rule bypassed
```

**Informal**: Safety means "nothing bad happens", even under adversarial conditions.

**Key safety scenarios**:
- Restart while assembling block → no double-subsidy
- Reorg during mining → old template discarded, no stale inclusion
- Concurrent mining threads → no race on mempool state
- Crash during coinbase creation → no partial state persisted

### 1.3 What Is "Live Mining"?

**Definition**: Mining is **live** if and only if:
```
∀ valid chain states S:
  1. Miner can produce a valid block template
  2. Template production completes in bounded time
  3. Mining proceeds even if mempool is empty
  4. Mining resumes after transient failures
```

**Informal**: Liveness means "something good eventually happens".

**Non-liveness examples** (must be prevented):
- Miner deadlocks waiting for mempool lock
- Miner stalls with empty mempool (no coinbase-only block)
- Miner loops forever on invalid transaction
- Miner refuses to start after crash

### 1.4 Relationship: Correctness ∧ Safety ∧ Liveness

**All three are required**:
```
Correct but unsafe:   Valid blocks, but creates inflation on restart
Safe but not live:    Never violates rules, but also never mines
Live but incorrect:   Always produces blocks, but they're invalid
```

**Ring 4 goal**: Prove all three simultaneously.

---

## 2. Mining Model (Abstract)

### 2.1 Mining Function (Formal)

**Abstract specification**:
```
mine_block: (ChainTip, MempoolSnapshot, ConsensusParams) → CandidateBlock

Where:
  ChainTip          = (height, prev_hash, prev_timestamp, prev_difficulty)
  MempoolSnapshot   = Set<Transaction>
  ConsensusParams   = (subsidy_schedule, maturity, max_block_size, ...)
  CandidateBlock    = (header, coinbase, transactions)
```

**Preconditions**:
```
∀ inputs (tip, mempool, params):
  1. tip is valid chain tip (verified by consensus)
  2. mempool contains only valid transactions
  3. params match network (mainnet vs testnet)
```

**Postconditions**:
```
∀ outputs B:
  1. B is valid (modulo PoW)
  2. B.prev_hash = tip.hash
  3. B.height = tip.height + 1
  4. B.transactions ⊆ mempool (or empty if mempool empty)
  5. B.coinbase.subsidy = subsidy_schedule(B.height)
```

### 2.2 Mining Lifecycle States

**State machine**:
```
STOPPED
  ↓ (start mining)
IDLE
  ↓ (new tip or mempool update)
ASSEMBLING
  ↓ (template ready)
MINING
  ↓ (solution found OR new tip arrives)
STOPPED / IDLE
```

**State invariants**:
```
STOPPED:      No mining thread running
IDLE:         Thread running, waiting for work
ASSEMBLING:   Building block template (holds locks briefly)
MINING:       Hashing (no locks, can be interrupted)
```

### 2.3 Explicit Non-Goals (Out of Scope for Ring 4)

**Ring 4 does NOT cover**:
- ❌ Pool coordination (stratumv2, job distribution)
- ❌ Stratum protocol correctness
- ❌ Hashrate economics (profitability, fee optimization)
- ❌ GPU/ASIC mining optimizations
- ❌ Performance tuning (template assembly speed)
- ❌ Network propagation strategy

**Ring 4 ONLY covers**:
- ✅ Subsidy correctness
- ✅ Block validity
- ✅ Safety under restart/reorg
- ✅ Liveness guarantees
- ✅ Determinism properties

**Rationale**: Ring 4 is about **correctness**, not **efficiency** or **economics**.

---

## 3. Correctness Invariants (C*)

### C1: Subsidy Correctness

**Invariant**:
```
∀ blocks B produced by miner:
  B.coinbase.output[0].amount = consensus_subsidy(B.height) + sum(tx.fee for tx in B)
```

**What this prevents**:
- Inflation (subsidy > consensus)
- Fee theft (fees not included in coinbase)
- Negative fees (coinbase < subsidy)

**Test strategy** (future):
```
Property MC1 (Subsidy Correctness):
  ∀ heights h ∈ [0, MAX_HEIGHT]:
    template = mine_block(tip_at(h), mempool, params)
    assert template.coinbase.subsidy == expected_subsidy(h)
```

### C2: Coinbase Structure Validity

**Invariant**:
```
∀ blocks B produced by miner:
  1. B.coinbase is transaction index 0
  2. B.coinbase has exactly 1 input (coinbase input)
  3. B.coinbase.input[0].prev_out = null
  4. B.coinbase.input[0].script_sig contains height (BIP34)
  5. B.coinbase outputs are standard
```

**What this prevents**:
- Non-coinbase transaction at index 0
- Multiple coinbase inputs
- Spending existing UTXOs in coinbase
- Missing height commitment (consensus rule)

**Test strategy** (future):
```
Property MC2 (Coinbase Structure):
  ∀ templates T:
    assert T.transactions[0].is_coinbase()
    assert T.transactions[0].inputs.size() == 1
    assert T.transactions[0].inputs[0].is_null_outpoint()
```

### C3: Block Template Validity

**Invariant**:
```
∀ candidate blocks B:
  is_valid_template(B) → is_valid_block(B) (modulo PoW)

Where:
  is_valid_template(B) checks:
    - Coinbase valid
    - All transactions valid
    - No double-spends within block
    - Size limits respected
    - Timestamp bounds respected
```

**What this prevents**:
- Mining invalid blocks that would be rejected
- Wasting hashrate on templates that can never be valid

**Test strategy** (future):
```
Property MC3 (Template Validity):
  ∀ templates T:
    if validate_template(T) == true:
      block = add_valid_pow(T)
      assert validate_block(block) == true
```

### C4: Transaction Validity in Context

**Invariant**:
```
∀ blocks B, transactions tx ∈ B:
  1. tx is valid against B.prev_block UTXO set
  2. tx.inputs are mature (if coinbase spends)
  3. tx.locktime is satisfied at B.height
  4. tx does not conflict with other tx in B
```

**What this prevents**:
- Including transactions that spend non-existent UTXOs
- Including immature coinbase spends
- Including timelocked transactions too early
- Double-spending within the same block

**Test strategy** (future):
```
Property MC4 (Transaction Context Validity):
  ∀ templates T:
    utxo_set = get_utxo_set_at(T.prev_hash)
    for tx in T.transactions[1:]:  // Skip coinbase
      assert tx.is_valid_in_context(utxo_set, T.height)
```

### C5: No Consensus Rule Bypass

**Invariant**:
```
∀ blocks B produced by miner:
  validate_block_consensus(B) must not skip any consensus check
```

**What this prevents**:
- Accidentally mining blocks that violate future soft forks
- Skipping signature validation
- Ignoring script execution limits

**Test strategy** (future):
```
Property MC5 (Full Consensus Validation):
  ∀ templates T:
    result = validate_full_consensus(T)
    assert result.all_checks_executed == true
    assert result.skipped_checks.is_empty()
```

---

## 4. Safety Invariants (S*)

### S1: No Inflation Under Restart

**Invariant**:
```
∀ restart events R:
  total_subsidy_after(R) = total_subsidy_before(R)
```

**Scenario**: Miner crashes while assembling block at height H:
- Before crash: Subsidy for H not yet claimed
- After restart: Subsidy for H still available
- **Violation**: Miner claims subsidy twice (once in crashed state, once after restart)

**Prevention**:
```
On restart:
  1. Discard any partial block state
  2. Rebuild template from current chain tip
  3. Do not reuse cached coinbase amounts
```

**Test strategy** (future):
```
Property MS1 (Restart Safety):
  subsidy_before = total_chain_subsidy()
  crash_during_mining()
  restart_miner()
  mine_block()
  subsidy_after = total_chain_subsidy()
  assert subsidy_after == subsidy_before + consensus_subsidy(new_height)
```

### S2: No Duplicate Subsidy Across Blocks

**Invariant**:
```
∀ blocks B₁, B₂ with B₁.height = B₂.height:
  if B₁ and B₂ are both on valid chains:
    total_subsidy_claimed = 1 × consensus_subsidy(height)
  (even if reorg causes both to exist temporarily)
```

**Scenario**: Reorg causes two blocks at same height:
- Block A at height 100 (subsidy: 50 DIN)
- Block B at height 100 (after reorg, subsidy: 50 DIN)
- **Violation**: Total subsidy is 100 DIN, but only 50 DIN should exist

**Prevention**:
```
This is NOT a violation IF:
  - Only one block ends up on the main chain
  - The other block's subsidy becomes unspendable (maturity requirement)

This IS a violation IF:
  - Miner creates both blocks on different forks
  - Both subsidy outputs become spendable
```

**Test strategy** (future):
```
Property MS2 (No Duplicate Subsidy):
  fork_at_height(100)
  mine_block_on_fork_a()  // Creates subsidy A
  mine_block_on_fork_b()  // Creates subsidy B
  assert only_one_is_spendable_after_maturity()
```

### S3: No Invalid Transaction Inclusion

**Invariant**:
```
∀ blocks B, transactions tx ∈ B:
  is_valid_transaction(tx, B.prev_block.utxo_set) = true
```

**Scenario**: Race condition in mempool update:
- Transaction T enters mempool
- Transaction T' spends same input
- Miner includes both in same block

**Prevention**:
```
During template assembly:
  1. Lock mempool snapshot
  2. Validate each transaction against current UTXO + prior tx in template
  3. Reject conflicts immediately
  4. Release snapshot before mining
```

**Test strategy** (future):
```
Property MS3 (No Invalid Inclusion):
  add_conflicting_transactions_to_mempool()
  template = mine_block()
  assert no_double_spends_in_template(template)
```

### S4: No Consensus Bypass Under Any Condition

**Invariant**:
```
∀ exceptional conditions E (restart, reorg, crash, race):
  validate_block(B) executes ALL consensus checks
```

**What this prevents**:
- Skipping PoW validation during restart
- Skipping signature checks under time pressure
- Accepting blocks with invalid coinbase during reorg

**Test strategy** (future):
```
Property MS4 (Consensus Always Enforced):
  for condition in [restart, reorg, crash, concurrent]:
    trigger_condition(condition)
    template = mine_block()
    assert full_consensus_validation_ran(template)
```

### S5: No Stale Block Acceptance After Reorg

**Invariant**:
```
∀ reorg events R:
  if new_tip arrives while mining block B:
    B must be discarded if B.prev_hash ≠ new_tip.hash
```

**Scenario**: Miner finds PoW solution for stale template:
- Mining on tip A
- Tip changes to B (reorg)
- Miner finds PoW for block extending A
- **Violation**: Miner submits block extending stale chain

**Prevention**:
```
Before submitting block:
  1. Re-check tip.hash == template.prev_hash
  2. If mismatch, discard solution
  3. Rebuild template on new tip
```

**Test strategy** (future):
```
Property MS5 (No Stale Submission):
  start_mining_on_tip(A)
  reorg_to_tip(B)
  solution = find_pow()  // For old template
  assert submission_rejected(solution)
```

---

## 5. Liveness Invariants (L*)

### L1: Mining Proceeds With Empty Mempool

**Invariant**:
```
∀ chain states S:
  if mempool.is_empty():
    miner can still produce valid block (coinbase-only)
```

**What this prevents**:
- Miner stalling when no transactions available
- Requiring mempool to bootstrap mining

**Test strategy** (future):
```
Property ML1 (Empty Mempool Liveness):
  clear_mempool()
  template = mine_block()
  assert template.transactions.size() == 1  // Coinbase only
  assert template.is_valid()
```

### L2: Mining Resumes After Transient Failures

**Invariant**:
```
∀ transient failures F (mempool lock timeout, RPC error, brief network loss):
  miner returns to IDLE or MINING state within bounded time
```

**What this prevents**:
- Permanent stall after transient error
- Infinite retry loops
- Deadlocks on failed locks

**Test strategy** (future):
```
Property ML2 (Transient Failure Recovery):
  inject_transient_failure()
  wait_for_recovery(max_time = 10s)
  assert miner_state == MINING or miner_state == IDLE
```

### L3: No Deadlock or Permanent Stall

**Invariant**:
```
∀ states S:
  miner does not deadlock on:
    - mempool_mutex
    - chain_tip_mutex
    - mining_thread_mutex

  and miner does not permanently stall on:
    - invalid transaction loop
    - template assembly infinite loop
```

**What this prevents**:
- Lock ordering violations causing deadlock
- Infinite loops on edge cases

**Test strategy** (future):
```
Property ML3 (No Deadlock):
  stress_test_concurrent_operations()
  assert no_thread_blocked_longer_than(threshold = 5s)
```

### L4: Template Production Completes in Bounded Time

**Invariant**:
```
∀ valid inputs (tip, mempool):
  time_to_assemble_template < MAX_TEMPLATE_TIME

Where:
  MAX_TEMPLATE_TIME = 1 second (recommended)
```

**What this prevents**:
- Template assembly taking so long that tip changes
- Miner never starting to hash because assembly is too slow

**Test strategy** (future):
```
Property ML4 (Bounded Template Time):
  for i in 1..1000:
    start = now()
    template = mine_block()
    elapsed = now() - start
    assert elapsed < 1.0 seconds
```

### L5: Mining Resumes on New Tip

**Invariant**:
```
∀ new tips T:
  if tip changes from A to B:
    miner discards old template
    miner rebuilds template on B
    time_to_resume < MAX_RESUME_TIME

Where:
  MAX_RESUME_TIME = 100ms (recommended)
```

**What this prevents**:
- Mining on stale tip for extended period
- Wasting hashrate on orphaned blocks

**Test strategy** (future):
```
Property ML5 (Quick Tip Switch):
  start_mining_on_tip(A)
  switch_to_tip(B)
  assert time_until_mining_on(B) < 100ms
```

---

## 6. Restart & Crash Semantics

### 6.1 Cold Start (Clean Startup)

**Preconditions**:
- No prior mining state exists
- Chain is synced to tip
- Mempool is empty or populated

**Expected behavior**:
```
1. Load chain tip from database
2. Load mempool snapshot
3. Build initial template
4. Enter MINING state
```

**Invariants**:
```
CS1: Subsidy equals consensus_subsidy(tip.height + 1)
CS2: No stale state from previous run
CS3: Template is valid
```

### 6.2 Warm Restart (After Clean Shutdown)

**Preconditions**:
- Miner was stopped gracefully (stop command)
- No partial state persisted
- Chain may have advanced during downtime

**Expected behavior**:
```
1. Reload chain tip (may be newer)
2. Rebuild mempool snapshot
3. Discard any cached templates
4. Build fresh template
5. Enter MINING state
```

**Invariants**:
```
WR1: Template uses current tip (not cached tip)
WR2: Mempool snapshot is current
WR3: No subsidy duplication
```

### 6.3 Crash Recovery (Unclean Shutdown)

**Preconditions**:
- Miner crashed during ASSEMBLING or MINING
- Partial block state may exist in memory (lost)
- Database state is consistent (write-ahead log)

**Expected behavior**:
```
1. Detect crash (via lock files, PID checks)
2. Discard all in-memory state
3. Rebuild from database only
4. Do NOT attempt to resume partial template
5. Build fresh template from current tip
```

**Invariants**:
```
CR1: No partial state persisted
CR2: No subsidy claimed for abandoned template
CR3: Database is consistent (no corruption)
CR4: Mining resumes within bounded time
```

**Critical**: Crash must never leave the system in a state where:
- Subsidy is claimed but block is not mined
- Invalid transaction is persisted in mempool
- Locks are held permanently

### 6.4 Interrupted Block Assembly

**Scenario**: New tip arrives while assembling template

**Expected behavior**:
```
1. Abort current assembly immediately
2. Release all locks
3. Discard partial template
4. Start assembly on new tip
```

**Invariants**:
```
IA1: Old template is never completed
IA2: No transactions from old template leak into new template (unless still in mempool)
IA3: Transition time < 100ms
```

### 6.5 Reorg During Mining

**Scenario**: Chain reorgs while miner is hashing

**Expected behavior**:
```
1. Detect tip change via block notification
2. Interrupt mining thread (non-blocking)
3. Discard current template (prev_hash is now stale)
4. Rebuild template on new tip
5. Resume mining
```

**Invariants**:
```
RD1: No block submitted on stale chain
RD2: Subsidy for new tip is correct
RD3: Transactions from orphaned blocks re-enter mempool (if valid)
RD4: No duplicate work (old template discarded, not reused)
```

### 6.6 Restart State Transitions

**State machine**:
```
CRASHED
  ↓ (detect crash)
RECOVERING
  ↓ (rebuild state from DB)
STOPPED
  ↓ (start command)
IDLE
  ↓ (build template)
MINING
```

**Forbidden transitions**:
```
❌ CRASHED → MINING (without recovery)
❌ RECOVERING → MINING (without rebuild)
❌ ASSEMBLING → ASSEMBLING (infinite loop)
```

---

## 7. Determinism Expectations

### 7.1 Deterministic Inputs → Deterministic Template

**Invariant**:
```
∀ inputs I₁, I₂:
  if I₁ == I₂:
    mine_block(I₁) == mine_block(I₂)

Where inputs I = (tip, mempool, timestamp_range, params)
```

**What this means**:
- Same chain tip
- Same mempool snapshot
- Same timestamp bounds
- Same consensus params
- → Produces identical template (except nonce)

**Non-deterministic fields (allowed)**:
- `header.nonce` (randomized)
- `header.timestamp` (within consensus bounds)
- `coinbase.script_sig extra data` (optional miner ID)

**Deterministic fields (required)**:
- `coinbase.subsidy`
- `coinbase.fees`
- `transactions` (order and contents)
- `header.prev_hash`
- `header.merkle_root` (function of transactions)

**Test strategy** (future):
```
Property MD1 (Deterministic Template):
  snapshot = capture_mining_inputs()
  template1 = mine_block(snapshot)
  template2 = mine_block(snapshot)
  assert template1.coinbase == template2.coinbase (modulo nonce)
  assert template1.transactions == template2.transactions
```

### 7.2 Transaction Selection Determinism

**Invariant**:
```
∀ mempool snapshots M:
  select_transactions(M) is deterministic
  (given same fee-rate comparator)
```

**What this prevents**:
- Non-deterministic transaction ordering (causes different merkle roots)
- Race conditions in transaction selection
- Unpredictable block contents

**Allowed non-determinism**:
- Mempool arrival order (affects which snapshot is taken)
- Time-based cutoffs (template assembly deadline)

**Test strategy** (future):
```
Property MD2 (Transaction Selection):
  mempool = create_fixed_mempool()
  selected1 = select_transactions(mempool)
  selected2 = select_transactions(mempool)
  assert selected1 == selected2
```

### 7.3 Subsidy Calculation Determinism

**Invariant**:
```
∀ heights h:
  consensus_subsidy(h) is deterministic
  (pure function of height)
```

**What this prevents**:
- Floating-point errors causing subsidy variance
- Time-based subsidy changes (not Dinero's model)
- Random subsidy amounts

**Test strategy** (future):
```
Property MD3 (Subsidy Determinism):
  for h in [0, 1000000]:
    s1 = consensus_subsidy(h)
    s2 = consensus_subsidy(h)
    assert s1 == s2
```

### 7.4 Timestamp Bounds

**Invariant**:
```
∀ blocks B:
  B.timestamp > median(last 11 blocks)
  B.timestamp ≤ network_time + 2 hours (consensus rule)

Within these bounds, timestamp is non-deterministic (allowed)
```

**What this means**:
- Timestamp must advance monotonically (relative to median)
- Timestamp cannot be too far in future
- Exact value is not deterministic (depends on wall clock)

**Test strategy** (future):
```
Property MD4 (Timestamp Bounds):
  template = mine_block()
  median = get_median_time_past()
  network_time = get_network_adjusted_time()
  assert template.timestamp > median
  assert template.timestamp <= network_time + 7200
```

### 7.5 Nonce Handling

**Invariant**:
```
∀ templates T:
  T.nonce is randomized (non-deterministic by design)
  Miner may choose any nonce in [0, 2^32 - 1]
```

**What this means**:
- Nonce is explicitly non-deterministic (mining randomness)
- Two miners can work on same template with different nonces
- Nonce does not affect block validity (only PoW)

**Test strategy** (future):
```
Property MD5 (Nonce Randomness):
  template1 = mine_block()
  template2 = mine_block()
  assert template1.nonce != template2.nonce (with high probability)
```

---

## 8. Future Properties (Not Implemented Yet)

### 8.1 Correctness Properties (MC*)

**MC1: Subsidy Correctness**
```
∀ heights h:
  template = mine_block_at_height(h)
  assert template.coinbase.subsidy == consensus_subsidy(h)
```

**MC2: Coinbase Structure**
```
∀ templates T:
  assert T.transactions[0].is_coinbase()
  assert T.transactions[0].inputs[0].is_null_outpoint()
  assert T.transactions[0].contains_height_commitment()
```

**MC3: Template Validity**
```
∀ templates T:
  if validate_template(T):
    block = add_valid_pow(T)
    assert validate_block(block)
```

**MC4: Transaction Context Validity**
```
∀ templates T:
  for tx in T.transactions[1:]:
    assert tx.is_valid_in_context(T.prev_utxo_set, T.height)
```

**MC5: No Consensus Bypass**
```
∀ templates T:
  validation_result = validate_full_consensus(T)
  assert validation_result.all_checks_ran()
```

### 8.2 Safety Properties (MS*)

**MS1: Restart Safety**
```
subsidy_before = total_chain_subsidy()
crash_and_restart()
mine_block()
subsidy_after = total_chain_subsidy()
assert subsidy_after - subsidy_before == consensus_subsidy(new_height)
```

**MS2: No Duplicate Subsidy**
```
fork_chain_at(height)
mine_on_fork_a()
mine_on_fork_b()
assert only_one_subsidy_spendable()
```

**MS3: No Invalid Inclusion**
```
add_conflicting_txs()
template = mine_block()
assert no_double_spends(template)
```

**MS4: Consensus Always Enforced**
```
for condition in [restart, reorg, crash]:
  trigger(condition)
  template = mine_block()
  assert full_validation_ran(template)
```

**MS5: No Stale Submission**
```
start_mining_on(tip_A)
reorg_to(tip_B)
solution = find_pow()
assert submission_rejected()
```

### 8.3 Liveness Properties (ML*)

**ML1: Empty Mempool Liveness**
```
clear_mempool()
template = mine_block()
assert template.transactions.size() == 1  // Coinbase only
```

**ML2: Transient Failure Recovery**
```
inject_failure()
wait(10s)
assert miner_state in [IDLE, MINING]
```

**ML3: No Deadlock**
```
stress_test_concurrent()
assert no_thread_blocked_longer_than(5s)
```

**ML4: Bounded Template Time**
```
for i in 1..1000:
  start = now()
  template = mine_block()
  assert now() - start < 1s
```

**ML5: Quick Tip Switch**
```
start_mining_on(tip_A)
switch_to(tip_B)
assert time_until_mining_on(tip_B) < 100ms
```

### 8.4 Determinism Properties (MD*)

**MD1: Deterministic Template**
```
snapshot = capture_inputs()
t1 = mine_block(snapshot)
t2 = mine_block(snapshot)
assert t1.coinbase == t2.coinbase (modulo nonce)
```

**MD2: Transaction Selection**
```
mempool = fixed_mempool()
s1 = select_transactions(mempool)
s2 = select_transactions(mempool)
assert s1 == s2
```

**MD3: Subsidy Determinism**
```
for h in [0, 1M]:
  assert consensus_subsidy(h) == consensus_subsidy(h)
```

**MD4: Timestamp Bounds**
```
template = mine_block()
assert template.timestamp > median_time_past()
assert template.timestamp <= network_time + 7200
```

**MD5: Nonce Randomness**
```
t1 = mine_block()
t2 = mine_block()
assert t1.nonce != t2.nonce (high probability)
```

### 8.5 Restart Properties (MR*)

**MR1: Cold Start**
```
clean_startup()
template = mine_block()
assert template.subsidy == consensus_subsidy(current_height + 1)
```

**MR2: Warm Restart**
```
graceful_shutdown()
restart()
template = mine_block()
assert template.prev_hash == current_tip()
```

**MR3: Crash Recovery**
```
crash_during_mining()
restart()
assert no_partial_state_persisted()
assert mining_resumed()
```

**MR4: Interrupted Assembly**
```
start_assembly()
new_tip_arrives()
assert old_template_discarded()
assert new_template_built()
```

**MR5: Reorg During Mining**
```
start_mining_on(tip_A)
reorg_to(tip_B)
assert block_not_submitted_on_stale_chain()
```

---

## 9. Explicit Non-Coverage

### 9.1 Out of Scope for Ring 4

**Ring 4 does NOT prove or verify**:

#### 9.1.1 Pool Coordination
- ❌ Stratum protocol correctness
- ❌ Job distribution to miners
- ❌ Share validation
- ❌ Payout calculation
- ❌ Hashrate measurement

**Rationale**: Pools are external to consensus. Ring 4 proves the miner produces valid blocks; pool software is separate.

#### 9.1.2 Economic Optimization
- ❌ Fee maximization strategies
- ❌ Transaction prioritization algorithms
- ❌ MEV (miner extractable value)
- ❌ Block space allocation

**Rationale**: Economics are policy, not correctness. Ring 4 proves blocks are valid, not profitable.

#### 9.1.3 Performance Tuning
- ❌ Template assembly speed
- ❌ Mempool indexing efficiency
- ❌ GPU kernel optimization
- ❌ Network propagation latency

**Rationale**: Performance is orthogonal to correctness. Slow but correct is acceptable; fast but wrong is not.

#### 9.1.4 Hashrate Distribution
- ❌ Mining centralization risks
- ❌ 51% attack prevention
- ❌ Selfish mining detection
- ❌ Network hashrate estimation

**Rationale**: Network-level game theory is outside Ring 4 scope.

#### 9.1.5 Hardware Compatibility
- ❌ CPU vs GPU vs ASIC differences
- ❌ Driver compatibility
- ❌ Firmware bugs
- ❌ Hardware failures

**Rationale**: Ring 4 assumes mining hardware works; it does not verify hardware correctness.

### 9.2 Dependencies (Assumed Correct)

**Ring 4 assumes these are already proven** (by previous rings):

- ✅ **Ring 1**: Consensus rules are correct
  - Subsidy schedule is correct
  - Block validation is correct
  - UTXO set updates are correct

- ✅ **Ring 2**: Wallet correctness
  - Coinbase outputs are spendable after maturity
  - Fee calculation is correct

- ✅ **Ring 3**: P2P protocol correctness
  - Block propagation works
  - Peer discovery works
  - (Note: Ring 3 threading bugs do NOT affect mining correctness)

**Ring 4 proves**: Given correct consensus and wallet, mining produces valid blocks.

### 9.3 Future Rings (Not Ring 4)

**These may be covered in future rings**:

- **Ring 5**: Smart contract mining (if Dinero adds scripting)
- **Ring 6**: Privacy-preserving mining (if Dinero adds confidential transactions to mining)
- **Ring 7**: Cross-chain mining (if Dinero adds merge mining)

**Ring 4 scope**: Baseline mining correctness only.

---

## 10. Ring 4 Phase Roadmap

### Phase 4a: Design (THIS DOCUMENT) ✅

**Status**: Complete (2026-01-02)

**Deliverable**: `docs/RING4_MINING_FORMAL_SPECIFICATION.md`

**What was defined**:
- [x] Mining correctness, safety, liveness definitions
- [x] Mining model (abstract)
- [x] Correctness invariants (C1-C5)
- [x] Safety invariants (S1-S5)
- [x] Liveness invariants (L1-L5)
- [x] Restart & crash semantics
- [x] Determinism expectations
- [x] Future properties (MC*, MS*, ML*, MD*, MR*)
- [x] Explicit non-coverage

**NO CODE CHANGED**.

### Phase 4b: Property Test Framework ⏸️ NOT STARTED

**Goal**: Implement infrastructure to test Ring 4 properties

**Tasks**:
- [ ] Create `tests/mining/property_test_framework.h`
- [ ] Implement `MiningSequenceGenerator` (random scenarios)
- [ ] Implement `MiningInvariantChecker`
- [ ] Add deterministic mining simulator (like Ring 3 peer simulator)
- [ ] Add crash/restart injection framework

**Exit criteria**: All infrastructure tests pass (no properties tested yet)

### Phase 4c: Correctness Properties ⏸️ NOT STARTED

**Goal**: Implement and prove correctness properties (MC1-MC5)

**Tasks**:
- [ ] Implement MC1 (subsidy correctness) test
- [ ] Implement MC2 (coinbase structure) test
- [ ] Implement MC3 (template validity) test
- [ ] Implement MC4 (transaction context) test
- [ ] Implement MC5 (no consensus bypass) test

**Exit criteria**: All MC* properties pass

### Phase 4d: Safety Properties ⏸️ NOT STARTED

**Goal**: Implement and prove safety properties (MS1-MS5)

**Tasks**:
- [ ] Implement MS1 (restart safety) test
- [ ] Implement MS2 (no duplicate subsidy) test
- [ ] Implement MS3 (no invalid inclusion) test
- [ ] Implement MS4 (consensus always enforced) test
- [ ] Implement MS5 (no stale submission) test

**Exit criteria**: All MS* properties pass

### Phase 4e: Liveness Properties ⏸️ NOT STARTED

**Goal**: Implement and prove liveness properties (ML1-ML5)

**Tasks**:
- [ ] Implement ML1 (empty mempool) test
- [ ] Implement ML2 (transient failure) test
- [ ] Implement ML3 (no deadlock) test
- [ ] Implement ML4 (bounded time) test
- [ ] Implement ML5 (quick tip switch) test

**Exit criteria**: All ML* properties pass

### Phase 4f: Determinism Properties ⏸️ NOT STARTED

**Goal**: Implement and prove determinism properties (MD1-MD5)

**Tasks**:
- [ ] Implement MD1 (deterministic template) test
- [ ] Implement MD2 (transaction selection) test
- [ ] Implement MD3 (subsidy determinism) test
- [ ] Implement MD4 (timestamp bounds) test
- [ ] Implement MD5 (nonce randomness) test

**Exit criteria**: All MD* properties pass

### Phase 4g: Restart Properties ⏸️ NOT STARTED

**Goal**: Implement and prove restart properties (MR1-MR5)

**Tasks**:
- [ ] Implement MR1 (cold start) test
- [ ] Implement MR2 (warm restart) test
- [ ] Implement MR3 (crash recovery) test
- [ ] Implement MR4 (interrupted assembly) test
- [ ] Implement MR5 (reorg during mining) test

**Exit criteria**: All MR* properties pass

### Phase 4h: Production Code Refactor ⏸️ NOT STARTED

**Goal**: Make production mining code satisfy Ring 4 invariants

**Tasks**:
- [ ] Refactor `src/mining/miner.cpp` to satisfy C*, S*, L* invariants
- [ ] Add invariant assertions to production code
- [ ] Fix any violations discovered by property tests
- [ ] Add restart safety guarantees
- [ ] Add determinism guarantees

**Exit criteria**: All Ring 4 properties pass with production code

### Phase 4i: Stress Testing ⏸️ NOT STARTED

**Goal**: Verify Ring 4 properties under adversarial conditions

**Tasks**:
- [ ] Run 10,000+ restart scenarios
- [ ] Run concurrent mining stress tests
- [ ] Run reorg stress tests
- [ ] Run crash injection tests
- [ ] Verify no property violations

**Exit criteria**: 10,000+ iterations, 0 failures

### Phase 4j: Close Ring 4 ⏸️ NOT STARTED

**Goal**: Mark Ring 4 complete

**Tasks**:
- [ ] Update `RING4_MINING_FORMAL_SPECIFICATION.md` with "PROVEN" status
- [ ] Document which properties prove which invariants
- [ ] Tag final Ring 4 completion
- [ ] Add to frozen core

**Exit criteria**: Ring 4 complete, all mining properties proven

---

## Summary: Phase 4a Deliverable

### What This Document Defines

**1. Definitions** (Section 1):
- Correct mining: Never produces invalid blocks
- Safe mining: No inflation under any condition
- Live mining: Always makes progress

**2. Mining Model** (Section 2):
- Abstract function: (ChainTip, Mempool, Params) → CandidateBlock
- Lifecycle states: STOPPED → IDLE → ASSEMBLING → MINING
- Explicit non-goals: pools, economics, performance

**3. Correctness Invariants** (Section 3):
- C1: Subsidy correctness
- C2: Coinbase structure validity
- C3: Block template validity
- C4: Transaction context validity
- C5: No consensus bypass

**4. Safety Invariants** (Section 4):
- S1: No inflation under restart
- S2: No duplicate subsidy
- S3: No invalid transaction inclusion
- S4: Consensus always enforced
- S5: No stale block acceptance

**5. Liveness Invariants** (Section 5):
- L1: Empty mempool liveness
- L2: Transient failure recovery
- L3: No deadlock
- L4: Bounded template time
- L5: Quick tip switch

**6. Restart Semantics** (Section 6):
- Cold start behavior
- Warm restart behavior
- Crash recovery behavior
- Interrupted assembly behavior
- Reorg during mining behavior

**7. Determinism** (Section 7):
- Deterministic template generation
- Transaction selection determinism
- Subsidy calculation determinism
- Timestamp bounds
- Nonce randomness (explicitly non-deterministic)

**8. Future Properties** (Section 8):
- MC*: Correctness properties
- MS*: Safety properties
- ML*: Liveness properties
- MD*: Determinism properties
- MR*: Restart properties

**9. Explicit Non-Coverage** (Section 9):
- Pools, economics, performance, hardware, game theory
- Dependencies on Ring 1-3

**10. Phase Roadmap** (Section 10):
- Phase 4a: Design (complete)
- Phases 4b-4j: Implementation (deferred)

### What This Document Does NOT Do

- ❌ Change production code
- ❌ Review mining diffs
- ❌ Add tests
- ❌ Fix bugs
- ❌ Optimize performance

### How This Preserves Frozen Core

**Ring 4 discipline**:
```
Design → Properties → Implementation → Verification
```

**Current status**:
```
Ring 4 Phase 4a: ✅ Design (this document)
Ring 4 Phase 4b+: ⏸️  Implementation (deferred)
```

**Frozen core integrity**: ✅ Preserved (documentation only, no code changes)

---

## Document Metadata

- **Created**: 2026-01-02
- **Author**: Claude Sonnet 4.5 (via Claude Code)
- **Purpose**: Formal specification of mining correctness & liveness (Ring 4 Phase 4a)
- **Status**: Design complete, awaiting implementation (Phase 4b)

**Next Action**: Commit this document, tag `v1.4.0-ring4-phase4a-design`

**DO NOT**: Touch production mining code until Ring 4 Phase 4b begins.
