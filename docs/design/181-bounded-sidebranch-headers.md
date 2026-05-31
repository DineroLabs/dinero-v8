# Design spec — bounded side-branch header storage (4d-2 / #181)

**Status:** DESIGN (no consensus code to be written until this is reviewed).
**Supersedes:** the count-only cap prototype (rejected in review — see "Why count-only fails").
**Hard rule:** do **not** merge a count-only cap.

---

## 1. Problem & threat model

`HeaderChainSelector::AddHeader` stores **every** valid header unconditionally into
`header_index_` (in-memory `std::map`) and `header_store_` (RocksDB). A public node
therefore has unbounded header storage. Post #174/#175/#180 every header must carry
real PoW at the ASERT-expected difficulty, but an attacker can still grind cheap
**minimum-difficulty** headers forking near genesis (ASERT floor) and feed long
low-work side branches → unbounded memory/disk growth. This is a resource-DoS
category that matters once independent public nodes onboard.

The defense must bound side-branch storage **without ever impairing the node's
ability to follow the most-work chain, including reorgs.**

### Why count-only fails (the rejected prototype)

A cap that refuses *new* side-branch headers once a fixed count (e.g. 10000) is
reached is the "naive cap." It is **net-negative for consensus safety**:

- **Reorg-after-flood lockout.** Attacker pre-fills the budget with cheap
  min-difficulty headers. A *legitimate* higher-work reorg then arrives
  incrementally: `c1, c2, … cN`. Each `cᵢ` has cumulative `chainwork ≤ best`
  until the tip `cN` overtakes — so each is classified a side branch and
  **refused** (budget full). `c2` then fails with PARENT NOT FOUND (its parent
  `c1` was never stored) and the reorg dies. The node is **stuck on the minority
  chain.**
- **Durable across restart.** Admitted junk is persisted (`StoreHeader`) and
  reloaded by `LoadFromStorage`, so the lockout survives reboot and cannot
  self-heal.
- The cap discriminates by **count**, but the only thing separating junk from a
  real reorg is **work** (junk = min-difficulty; live reorg headers = current
  high difficulty). A count cap is blind to exactly the signal that matters.
- It produces **false-green tests**: a static flood test passes *because* the
  reorg was blocked ("best unchanged"); fleet-replay passes because it only
  exercises honest forward sync (never capped). Neither exercises the adversarial
  path.

---

## 2. Invariants (the bound MUST preserve all of these)

1. **Active chain is never evicted.** Every header that is an ancestor of
   `best_header_` (inclusive) is permanently retained.
2. **AssumeUTXO / replay anchors are never evicted.** (The h=13000 AssumeUTXO
   anchor and any pinned replay anchors. These sit on the best chain, so (1)
   covers them, but it is an explicit, separately-asserted guarantee.)
3. **Eviction compares cumulative chainwork, not count.** Count only triggers
   *consideration* of eviction; the decision is always work-ranked.
4. **Only losing side-branch _tips_ are eviction candidates.** A tip = a stored
   header not on the best chain with **no stored children**. Internal headers
   (those with a stored descendant) are never directly evicted.
5. **Pruning must not orphan a better descendant.** When a tip is evicted, its
   now-childless ancestors may be pruned **only** up to the first ancestor that
   (a) is on the best chain, (b) is an anchor, or (c) still has another surviving
   child. Pruning stops there. No header is ever removed while a stored descendant
   of it remains.
6. **Restart reload preserves the same safety behavior.** `LoadFromStorage` must
   rebuild the structures needed to enforce the bound, and a persisted flood must
   remain subject to eviction once a better fork appears. The on-disk store is
   bounded by the same rule (enforced on load, or immediately enforceable by the
   next `AddHeader`).
7. **Reorg-after-flood must succeed.** A competing chain whose tip strictly
   exceeds `best` must be able to reorg the node **even when the side-branch
   budget is completely full of low-work junk.** This is the acceptance criterion.

---

## 3. Recommended design — work-aware eviction (Option A)

Proportionate to the threat and to the node's existing anchoring
(`nMinimumChainWork` + AssumeValid h=13000). Keeps memory/disk bounded while
guaranteeing the most-work chain always wins.

### 3.1 Admission rule (in `AddHeader`, after ValidateHeader + chainwork computed)

```
if h.chainwork > best.chainwork:
    admit(h)                      # advances best (incl. a winning reorg) — always
else:                             # side branch
    if side_branch_count < MAX_SIDE_BRANCH_HEADERS:
        admit(h)
    else:                         # budget full → work-ranked eviction
        min_tip = lowest-chainwork eviction-eligible side-branch tip
        if h.chainwork > min_tip.chainwork:
            evict_branch(min_tip)  # tip + childless non-best/non-anchor ancestors
            admit(h)
        else:
            refuse(h)              # lower-work than everything we keep
```

**Why this lets a real reorg through a full budget:** junk forks near genesis at
min difficulty → junk tips have **low** cumulative work. A live reorg forks near
the tip (height ~13000) → its first header `c1` already carries ~13000 blocks of
cumulative work ≫ any genesis-era junk tip. So `c1` is admitted (evicting a junk
tip); `c2 > c1 > min_tip` → admitted; … the partial reorg keeps displacing the
lowest junk until it overtakes `best`. Reorg succeeds. The difficulty asymmetry
is doing the work the count cap ignored.

### 3.2 Data structures

- **Eviction-eligible-tip index:** a work-ordered structure (e.g.
  `std::multimap<arith_uint256, uint256>` or a min-heap keyed by chainwork) over
  side-branch tips only. `min_tip` lookup must be O(log n).
- **Child count / tip-ness:** track, per stored header, the number of stored
  children (or a `is_tip` bit maintained incrementally) so admit/evict can update
  tip status of the parent in O(1)/O(log n).
- **"On best chain" test:** O(1) via the existing height+ancestor walk or a
  per-entry `on_best_chain` flag updated by `UpdateBestHeader` when best moves.
  When best advances, headers that drop off the old best chain become side
  branches and must be (re)inserted into the tip index; they are recent +
  high-work so they will not be evicted, but the bookkeeping must be correct.

### 3.3 `evict_branch(tip)`

Walk from `tip` toward genesis, removing `tip`, then each ancestor, stopping at
the first ancestor that is on the best chain, is an anchor, or has another stored
child. Each removed header is erased from `header_index_`, `header_store_`, and
the tip index; the parent's child-count is decremented and, if it becomes a
non-best childless side branch, it is inserted into the tip index.

### 3.3b Parent-slot reservation (safety-critical ordering)

When the incoming header `E` EXTENDS a side-branch tip `P`, a naive ordering
(select `min_tip`, evict, then bump `P`'s child_count) is unsafe: if `P` is the
lowest-work tip, `min_tip == P`, and since `E.chainwork = P.chainwork + work(E) >
P.chainwork` the strict-`>` admission test always fires → `EvictBranch(P)` frees
`E`'s own parent (use-after-free + orphan). This is the *natural* shape of a flood
that extends one long side chain, not just many height-1 forks.

Fix: **reserve `E`'s slot in its parent BEFORE selecting/evicting** — bump
`parent->child_count` and refresh its tip status first, so `P` has `child_count >=
1` and is excluded from the eviction-eligible tip set (cannot be `min_tip`) and is
not pruned when `EvictBranch` walks upward (the walk stops at `child_count >= 1`).
The reservation is undone on the refuse path. Regression test: #5
(extend-the-min-tip).

### 3.4 Interaction with existing anchoring (sharpens the bound)

The node already refuses to follow any chain below `nMinimumChainWork` and treats
≤ h=13000 as AssumeValid. So a side branch that **cannot** lead to a chain
exceeding the node's committed work is not a reorg candidate at all and is freely
evictable. The "protected" set is effectively *the best chain + forks recent/heavy
enough to plausibly exceed `nMinimumChainWork`.* This is why Option A is
sufficient for Dinero specifically: deep genesis-era reorgs are already outside
what the node will commit to.

### 3.4b Reorg demotion slack (implementation note)

The cap is enforced on **admission** of new side-branch headers. A reorg additionally
**demotes** the old best-chain headers above the fork point into side branches
*without* routing them through the admission cap, so `side_count` can transiently
exceed `MAX_SIDE_BRANCH_HEADERS` by up to the reorg depth. This is bounded and
self-healing: the demoted headers are the real (PoW-backed) prior chain, reorgs
are shallow, and the next over-budget side-branch admission evicts the lowest-work
tip. We deliberately do **not** add a post-reorg trim pass (extra eviction on the
reorg path, for no resource benefit — the demoted headers were already stored as
the best chain). The effective bound is `best_chain_len + MAX_SIDE_BRANCH_HEADERS +
O(reorg depth)`. The reorg-after-flood test asserts this (cap + a small slack), not
an exact-cap equality.

### 3.5 Parameters

- `MAX_SIDE_BRANCH_HEADERS` — generous (≫ any realistic reorg depth; ~10⁴ ≈ a few
  MB). Document the storage bound it implies. Re-evaluate against real
  public-node telemetry.
- Throttle the cap-hit log (one line per episode) — a header flood must not become
  a log flood. (The prototype's `side_branch_cap_warned_` member + value-init /
  `REQUIRE`-not-`assert` test lessons carry forward.)

---

## 4. Alternative — Bitcoin-style headers presync (Option B)

The complete solution: a two-phase header sync that commits to a peer's **total
work** in a low-bandwidth first pass before storing anything, only committing
storage once the chain demonstrates sufficient cumulative work. Fully solves even
deep genesis-era reorg-vs-flood. Larger redesign of the sync layer. **Recommend
deferring** unless independent-node telemetry shows Option A is insufficient —
consistent with the prior "revisit as proper headers-presync only if real need."

---

## 5. Test plan (all required before #1 implementation merges)

1. **Reorg-after-flood (the discriminating test — must PASS):** fill the budget
   with low-work forks near genesis; then feed a competing chain whose tip
   strictly exceeds `best`; assert the selector **reorgs to it** and the active
   chain is correct. (This is the test the count-only cap FAILS.)
2. **Static flood bound:** low-work side-branch flood is bounded; active chain +
   best tip intact (reuse `test_header_sidebranch_bound.cpp`, but strengthen it
   so passing requires the reorg path, not just "best unchanged").
3. **Pruning correctness:** evicting a tip never orphans a stored descendant;
   shared-prefix forks keep the shared ancestors while a sibling tip is evicted.
4. **Restart equivalence:** persist a full/flooded store, restart, confirm the
   bound + reorg-after-flood behavior is identical post-`LoadFromStorage`.
5. **Normal-sync regression:** honest forward sync + shallow reorgs are never
   capped (existing header tests stay green).
6. **Fleet-replay:** real chain still syncs (necessary but **not sufficient** —
   it cannot validate the adversarial path; #1 must not rely on it for safety).

---

## 6. Decision

- Implement **Option A (work-aware eviction)** as #181's fix, after this spec is
  reviewed.
- Keep **Option B** documented as the heavyweight fallback.
- Acceptance gate = test #1 (reorg-after-flood) passing, plus #3/#4.
- The count-only prototype on `security/bounded-sidebranch-headers` is throwaway
  scaffolding (test harness + log-throttle/value-init lessons reusable); its
  admission logic must be **replaced**, not merged.
