# Privacy Lanes: Implementation Plan

**Status:** Implemented and regression-tested — `66010834f` (Apr 7, 2026)  
**Design doc:** `PRIVACY_LANES_MEMPOOL_AND_FEE_POLICY.md`  
**Branch:** p2p-fix  
**Date:** April 7, 2026

This is a deferred implementation plan. Build it when ZK covenant txs are
expected on mainnet (i.e., after GPU proving reduces Lane 3 prove time to
acceptable UX). All changes are internal policy — no consensus changes.

**Governing constraint (see §0 of design doc):** ZK is additive. Ring must
not depend on ZK plumbing. The ZK lane must not degrade transparent or ring
in cost, latency, or UX. Every step below must be verified against this
before merge.

---

## Sequencing (implement in this order)

### Step 1 — Transaction classification and weight (foundation)

**`include/primitives/transaction.h`**  
Add two inline methods after `GetWeight()` (~line 403):
- `GetLaneClass() const -> int` — returns 0/1/2 from version + covenant inputs
- `GetLaneWeight() const -> size_t` — implements the design doc weight formula

**`src/primitives/transaction.cpp`**  
Implement `GetLaneWeight()`:
```
tx_weight = bytes + CLASS_WEIGHT[lane] × 1000
CLASS_WEIGHT: {0→1, 1→100, 2→2000}
```

Classification logic (§5.3 of design doc — classify at highest lane present):
- Class 2: `tx.IsRingCovenantTransaction()` AND any input `IsCovenantInput()`
- Class 1: `version == TX_VERSION_RING`
- Class 0: everything else

Use the existing helpers rather than raw field checks (`tapscript_zk_proof` non-empty).
This avoids drift if covenant input semantics evolve — the helpers are the single source of truth.

**Risk:** `GetLaneWeight()` calls `GetSize()` which calls `Serialize()`. This is
acceptable for admission-time checking but may need caching if it appears in
hot loops. Measure before optimizing.

---

### Step 2 — Fix version guards (unblocks ring/covenant txs at policy layer)

Two separate gates must be updated:

**`src/policy/mempool_policy.cpp` line ~14**  
The `checkStandardness` function currently rejects `version > 2`. Change to
accept versions 1–4. Without this fix, all ring and covenant txs are silently
rejected before any fee check runs.

**`src/validation/validation_mempool.cpp`**  
A second version check `tx.version > Transaction::TX_VERSION_RING` rejects v4
(TX_VERSION_RING_COVENANT) independently of `checkStandardness`. Update this
guard to also accept TX_VERSION_RING_COVENANT (version 4). Fixing only
`mempool_policy.cpp` is insufficient — v4 txs are still blocked here.

---

### Step 3 — Wire lane weight into the secondary mempool

**`src/mempool/mempool.cpp` line ~1218 — `calculateVirtualSize()`**  
Replace stub `return tx.Serialize().size()` with `return tx.GetLaneWeight()`.

This single change propagates through: fee rate check (line 212), MempoolEntry
storage (line 243), eviction sort (line 770), CPFP score (lines 790–793).

**`include/mempool/mempool.h`**  
Add to `MempoolConfig`:
- `size_t max_zk_txs = 16;`

Add to `MempoolAcceptResult` enum:
- `ZK_SLOTS_FULL`

---

### Step 4 — ZK slot admission in secondary mempool

**`src/mempool/mempool.cpp` — `acceptTransaction()`**  
Add ZK slot counter. Before fee check:
```cpp
if (tx.GetLaneClass() == 2 && zk_count_.load() >= config_.max_zk_txs)
    return MempoolAcceptResult::ZK_SLOTS_FULL;
```
Increment on admission, decrement in remove paths.

---

### Step 5 — Wire lane weight into daemon mempool

**`src/daemon/mempool.cpp`**

1. Fee rate check (~line 637–641): replace `tx.Serialize().size()` with
   `tx.GetLaneWeight()`.

2. ZK slot counter: add `std::atomic<size_t> m_zk_tx_count_{0}` as member.
   Check and increment in `submitTransaction` (before fee check). Decrement
   in `removeConfirmedTransactions` and `removeTransaction`. Wire decrement
   into reorg handling path to prevent starvation after block disconnect.

3. Remove CT-specific fee adjustment block (lines ~650–679). Its logic is
   superseded by lane weight. Leaving both active double-penalizes CT txs.
   **Do not remove until Step 3 and 5 fee checks are confirmed working.**

---

### Step 6 — Block template ZK tx count limit

Define constant in a shared location (new `include/policy/lane_policy.h` or
inline):
```cpp
constexpr size_t MAX_ZK_TX_PER_BLOCK = 4;
```

**`src/daemon/mempool.cpp` — `selectTransactionsForBlock` (~line 1909)**  
Add `size_t zk_tx_count = 0` before the selection loop. In the loop body,
before adding a tx:
```cpp
if (entry.tx.GetLaneClass() == 2) {
    if (zk_tx_count >= MAX_ZK_TX_PER_BLOCK) continue;
    zk_tx_count++;
}
```

**`src/mining/block_template.cpp` — `selectTransactions` (~line 515)**  
Same pattern. Add `zk_count` to `SelectionState`, increment in
`addTransaction` for Class 2 txs, check in the `canAddTransaction` gate.

---

### Step 7 — Fee estimator feerate recording

**`src/daemon/mempool.cpp` — `recordTxEntry` call (~line 880 area)**  
Change feerate passed to fee estimator from `fee / tx_size` (una/byte) to
`fee / tx.GetLaneWeight()` (una/wu). This makes estimator history lane-aware
without changing the estimator's data structure.

---

## Risk Register

| Risk | Severity | Mitigation |
|------|----------|------------|
| Version guard in `mempool_policy.cpp` rejects v3/v4 silently | High | Fix both gates in Step 2; add test that v4 reaches fee check |
| Second version guard in `validation_mempool.cpp` rejects v4 | High | Update `tx.version > TX_VERSION_RING` check alongside mempool_policy fix |
| Two mempools diverge on weight | Medium | Both call `GetLaneWeight()` from same method |
| `GetLaneWeight()` expensive on hot path | Medium | Measure; cache `GetSize()` result if needed |
| Old CT adjusted_fee_rate and new lane weight both applied | High | Remove CT block only after lane weight confirmed working |
| ZK slot counter not decremented on reorg | Medium | Wire decrement into disconnect path explicitly |
| CPFP ancestor scoring makes ZK child-pays very expensive | Intended | Document; this is correct behavior |

---

## What NOT to change

- Consensus validation rules (which txs are valid)
- Ring signature or ZK proof format
- Transaction serialization format
- Block size limits (only add ZK count limit on top)
- Fee estimator bucket structure

All changes are internal node policy. A node with this code and a node without
it will accept and reject the same transactions at consensus level. They will
differ only in which txs they relay and include in templates.

---

## Test status (as of 66010834f, Apr 7, 2026)

**Regression suite: 24/24 pass** (`tests/policy/test_privacy_lane_policy.cpp`)

- `GetLaneClass()` correct for v1/v2/v3/v4-no-proof/v4-with-proof ✓
- `GetLaneWeight()` formula verified for all three lanes ✓
- Version bounds predicate: v1–v4 in range, v5/v0/negative out of range ✓
- `checkStandardness` rejects v5 and v0 at the version gate ✓
- `SelectionState.zk_count` initializes at 0; transparent/ring unaffected by ZK cap ✓

**Still pending:**
- Mempool rejects 17th Class 2 tx (requires full mempool integration test with MockChainStateView)
- Block template includes at most 4 Class 2 txs (requires BlockTemplateBuilder integration test)
- Integration test: ring tx and ZK covenant tx both admitted and mined on regtest

**Bug caught by tests:** `GetLaneClass()` initially used `version == TX_VERSION_RING` (v3 only);
corrected to `IsRingTransaction()` (v3 and v4) so v4 without ZK proof lands at Class 1, not Class 0.

---

## Trigger for implementation

Implement when **any** of these is true:
- ZK covenant prove time drops below 5s (GPU MSM landed)
- First ZK covenant tx appears on mainnet
- Fee market shows transparent txs priced same as ring/covenant (policy gap visible)
