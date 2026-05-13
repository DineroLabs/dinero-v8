# Privacy Lanes: Mempool and Fee Policy

**Status:** Design draft — not yet consensus  
**Branch:** p2p-fix  
**Date:** April 7, 2026  
**Depends on:** f1e28eaa2 (CPU proving baseline), 7557d3ae7 (lane benchmarks)

---

## 0. Design Invariants (Hard Requirements)

These are constraints, not preferences. Any implementation choice that violates
them is the wrong implementation choice.

**Invariant 0.1 — Lane isolation**  
The three lanes are separate spend templates with shared settlement, not a
single transaction system where everything depends on the heaviest machinery.
Same chain, same asset, same node software — different validation paths.

**Invariant 0.2 — ZK is additive, not invasive**  
Ring transactions must not require ZK witness construction, ZK proof objects,
ZK mempool rules, GPU assumptions, or heavy proving libraries in the wallet
path. If ring depends on ZK plumbing, the cheap private lane stops being cheap.

**Invariant 0.3 — The premium lane must not degrade the other two**  
The existence of the ZK covenant lane must never make transparent or ring
privacy worse in cost, latency, relay behavior, or wallet UX.

**Invariant 0.4 — Fee separation is mandatory**  
Each lane pays its own cost class. Pricing everything as if it were near ZK
cost kills ordinary usage. Transparent pays transparent cost. Ring pays ring
cost. ZK pays ZK cost.

**Invariant 0.5 — Wallet escalation is capability-driven**  
"Send privately" maps to ring, not ZK. ZK is invoked only when the transaction
specifically needs covenant/policy enforcement or a hidden binding relation.
A wallet that silently escalates ring requests to ZK violates this invariant.

**The sentence that must stay true forever:**  
> The existence of the premium ZK lane must never make transparent or ring
> privacy worse.

---

## 1. Transaction Classes

Three distinct privacy lanes exist in Dinero. Each imposes different costs on
verifying nodes and carries different privacy and programmability guarantees.

| Class | Lane | Privacy | Covenants | Proving | Verifying |
|-------|------|---------|-----------|---------|-----------|
| 0 | Transparent | None (public) | No | <1 ms | 0.013 ms |
| 1 | Ring privacy | Good (CLSAG-16) | No | <1 ms | 1.53 ms |
| 2 | ZK covenant | Strong + structured | Yes | 15–60 s | 3,618 ms |

These numbers are measured on Apple M-series (arm64), secp256k1-zkp,
from `bench_zkvm` at commit `7557d3ae7`. GPU proving will reduce Class 2
proving time; verify time is less affected and remains the policy anchor.

**Key insight:** Class 2 is not "a bigger transaction." It is a different
class of computation. A single ZK covenant verify consumes ~285,000× more
CPU than a transparent verify and ~2,366× more than a ring verify.
Fee-per-byte alone cannot capture this.

---

## 2. Validation Cost Model

### 2.1 Weight formula

```
tx_weight = (serialized_bytes × BYTE_WEIGHT)
          + (CLASS_WEIGHT[class] × BASE_VERIFY_UNIT)
```

### 2.2 Proposed initial weights (policy, not consensus)

| Component | Value | Rationale |
|-----------|-------|-----------|
| `BYTE_WEIGHT` | 1 | Standard byte cost (bandwidth + storage) |
| `BASE_VERIFY_UNIT` | 1,000 | Calibration constant |
| `CLASS_WEIGHT[0]` | 1 | Transparent baseline |
| `CLASS_WEIGHT[1]` | 100 | Ring is ~120× transparent; compress to 100× |
| `CLASS_WEIGHT[2]` | 2,000 | ZK is ~285,000× transparent; compress to 2,000× |

Raw CPU ratios are not used directly. The weights are compressed deliberately:
- Prevents fees from becoming economically inaccessible
- Leaves room to tune as optimization reduces verify cost
- Keeps mempool functional under load

### 2.3 Proof sizes (from benchmark)

| Lane | Proof bytes | Ratio vs transparent |
|------|-------------|----------------------|
| Transparent | 64 B | 1× |
| Ring (CLSAG-16) | 611 B | 10× |
| ZK covenant | 7,691 B | 120× |

For small transactions, the compute component dominates weight for Class 2.
For very large transactions (many inputs), byte weight grows and compute
weight becomes relatively less important.

---

## 3. Mempool Policy

### 3.1 Block validation budget

Blocks are no longer primarily bandwidth-limited. A block containing even a
small number of ZK covenant transactions is CPU-limited.

**Proposed limits (per block):**

| Resource | Limit | Notes |
|----------|-------|-------|
| Total tx_weight | 4,000,000 wu | Standard block weight cap |
| Max Class 2 txs | 4 per block | At 2,000 BASE_VERIFY_UNIT each, ~4 ZK txs fill compute budget |
| Max ZK verify time | ~15 s total | Conservative; revisit as verify improves |

The Class 2 count limit exists independently of weight, as a DoS safeguard
during the period when ZK verify is expensive. It should be removed once
verify time falls below ~100 ms.

### 3.2 Admission rules

A transaction is admitted to the mempool if:
1. `fee / tx_weight >= MIN_FEE_RATE` (same formula for all classes)
2. Class 2: proof is structurally valid before admission (fast structural
   check runs first; full IPA verify deferred to block validation)
3. Class 2: mempool ZK slot count < `MAX_MEMPOOL_ZK_TXS` (proposed: 16)

### 3.3 Eviction priority

Under pressure, evict lowest `fee / tx_weight` first. This naturally
prices out low-fee ZK covenant txs before low-fee transparent txs of
equivalent byte size, which is the correct behavior.

---

## 4. Fee Policy

### 4.1 Minimum fees by class

```
min_fee(tx) = tx_weight × MIN_FEE_RATE_PER_WU
```

With `MIN_FEE_RATE_PER_WU = 1 una/wu` and typical transactions:

| Lane | Typical tx_weight | Typical min fee |
|------|------------------|-----------------|
| Transparent (1 in, 2 out) | ~600 wu | 600 una |
| Ring (1 in, 2 out, ring-16) | ~100,000 wu | 100,000 una |
| ZK covenant (1 input) | ~2,007,691 wu | ~2,007,691 una |

These are minimum floors. Market fees will exceed minimums when blocks are full.

### 4.2 Fee estimation for wallets

Wallets should present fees in DIN, not wu, and communicate the lane clearly:

- Class 0: "Standard fee: 0.006 DIN"
- Class 1: "Private send fee: ~1 DIN"  
- Class 2: "Covenant proof fee: ~20 DIN (high-assurance mode)"

The Class 2 fee should be accompanied by a UX warning about proving time.
Proving time is a UX concern, not a fee concern — but users need to know
that the proof takes time to generate before they see a fee prompt.

---

## 5. Cross-Lane Spend Rules

### 5.1 Valid combinations

| Input lane | Output lane | Valid? | Notes |
|------------|-------------|--------|-------|
| Transparent | Transparent | ✅ | Standard |
| Transparent | Ring | ✅ | Upgrade privacy on send |
| Ring | Ring | ✅ | Preserve privacy |
| Ring | Transparent | ✅ | User choice to de-shield |
| Ring | ZK covenant | ✅ | Cryptographic binding handles this |
| ZK covenant | Transparent | ✅ | Covenant may permit transparent output |
| ZK covenant | Ring | ✅ | Covenant may permit ring output |
| ZK covenant | ZK covenant | ✅ | Chained covenants |

### 5.2 Binding constraint

When a ZK covenant spends a ring output, the binding proof (CLSAG +
hidden-member IPA) must cryptographically link the ring input to the
covenant proof. This is already implemented via `VerifyRingCovenant`'s
`StructuralCheck` → `CLSAGCheck` → `ZKCheck` → `BindingCheck` sequence.

The consensus rule is: a cross-lane spend is valid iff all component
proofs verify AND the binding check passes. No special casing required.

### 5.3 Fee class for mixed transactions

A transaction containing inputs from multiple lanes is classified at the
**highest** lane present. A tx with one transparent input and one ZK
covenant input is Class 2 for fee and mempool purposes.

---

## 6. Wallet Lane Selection

### 6.1 Automatic selection rules

The wallet selects the lane automatically based on what the transaction requires:

1. If output script contains covenant opcodes (OP_CTV, etc.) → Class 2
2. If user requests "private send" and no covenants → Class 1
3. Otherwise → Class 0

The user never manually selects a lane number. The wallet may expose a
"Private mode" toggle (Class 0 → Class 1) and an "Advanced: add covenant"
option (Class 1 → Class 2).

### 6.2 UX guidance

| Scenario | Suggested default | Why |
|----------|------------------|-----|
| Payment to exchange | Class 0 | Exchange needs visible receipt |
| Payment to individual | Class 1 | Default privacy without cost |
| Vault / time-lock | Class 2 | Covenant required |
| High-value send with spending rules | Class 2 | User explicitly needs guarantees |
| Everyday commerce | Class 0 or 1 | Speed and cost matter |

---

## 7. Future Adjustment Clause

All weights and limits in this document are **policy**, not consensus.
They can be adjusted via a soft parameter update without a hard fork,
provided the adjustment does not change which transactions are valid —
only their economic cost.

Expected adjustment triggers:

| Trigger | Likely adjustment |
|---------|------------------|
| GPU verify reduces Class 2 to <100 ms | Remove Class 2 tx count limit |
| GPU verify reduces Class 2 to <10 ms | Lower CLASS_WEIGHT[2] toward 200× |
| Lookup gate optimization (-50% constraints) | Reduce CLASS_WEIGHT[2] proportionally |
| Real-world ZK tx volume observed | Tune MAX_MEMPOOL_ZK_TXS |

The goal is to track actual node burden, not to permanently penalize
the ZK lane. As proving and verifying get cheaper, the premium should shrink.

---

## 8. Open Questions

These require further design or community input before finalizing:

1. **Structured delay for ZK txs?** Should nodes defer full ZK proof
   verification to a background queue, accepting the structural check
   at relay time and the full proof only at block inclusion time? This
   would improve propagation speed at the cost of complexity.

2. **ZK proof caching?** If the same proof appears in multiple competing
   blocks (reorg scenario), can nodes cache the verified result to avoid
   re-verifying? Transcript-binding makes this safe if indexed by proof hash.

3. **Multi-input ZK transactions?** The current benchmarks cover single-input
   ZK covenant txs. Multiple ZK inputs multiply verify cost. Does each input
   get its own weight entry, or is there a per-tx cap?

4. **Fee market interaction with proving time?** During periods of high
   demand, users may overbid fees to get ZK txs included faster. Does the
   wallet need a "fee acceleration" path that doesn't require re-proving?

5. **Minimum ring size for Class 1?** Currently ring-16 is standard.
   Should smaller rings (ring-4, ring-8) be a sub-class with lower fees?
   Or enforce ring-16 as the minimum for Class 1 to preserve anonymity sets?

---

## Appendix: Raw Benchmark Data

From `bench_zkvm` at `7557d3ae7`, Apple M3 Max (arm64):

```
Lane 1: Transparent (Schnorr)       Verify:    0.0127 ms   Size:    64 B
Lane 2: Ring privacy (CLSAG-16)     Verify:    1.529  ms   Size:   611 B
Lane 3: ZK covenant (ring+IPA)      Verify: 3618.4   ms   Size: 7,691 B

Ratios vs transparent:
  Ring:         120×   verify time,   10× proof size
  ZK covenant:  285,000× verify time, 120× proof size

Ratios vs ring:
  ZK covenant:  2,366×  verify time,  13× proof size
```

Proving time (from `test_zkvm` ring_covenant_sign_verify, cold start):
```
  Lane 1: Transparent         <1 ms
  Lane 2: Ring (CLSAG-16)     <1 ms
  Lane 3: ZK covenant         ~15 s (CPU baseline, f1e28eaa2)
                              ~2-5 s (projected with GPU MSM)
```
