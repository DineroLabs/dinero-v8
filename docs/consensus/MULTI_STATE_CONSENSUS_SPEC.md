# Multi-State Consensus Specification

## Core Model

A Dinero block applies ONE atomic state transition. There are not three
state machines — there is one state machine with three projections:

```
S → S'   (a single transition, applied or rejected as a unit)

where S = (utreexo_forest, commitment_tree, nullifier_set, tip_height)
```

The transparent UTXO set, the shielded commitment tree, and the nullifier
set are views into the same ledger state. They are constrained by the same
conservation law. They are committed atomically in the same block header.
If any projection is inconsistent, the entire transition is rejected.

This is not "three systems that must agree." It is one system whose state
happens to be partitioned for verification efficiency.

---

## The Single Conservation Law

Every transaction obeys exactly one rule:

```
value_in = value_out
```

Where:

```
value_in  = sum(transparent_inputs) + sum(shielded_spends)
value_out = sum(transparent_outputs) + sum(shielded_outputs) + fee
```

The fee is always transparent. There is no hidden fee.

For pure-transparent transactions (v2): shielded terms are zero.
For pure-shielded transactions (v5 transfer): transparent terms are zero.
For shield/unshield (v5 mixed): both terms are non-zero.

The ZK proof demonstrates that the shielded terms satisfy conservation
without revealing their values. The transparent terms are verified by
direct inspection. The binding signature commits to the cross-domain
value flow so neither side can lie about its contribution.

This is one law, not two laws that happen to match.

---

## State Projections (Not Independent Systems)

### Projection 1: Transparent (Utreexo)

The Utreexo forest is a **projection** of the ledger onto the set of
unspent transparent outputs. It does not define validity — it is a
**verification aid** that enables stateless checking of transparent spends.

When a block is applied:
- Transparent outputs are added to the forest
- Spent transparent inputs are removed from the forest
- Shielded components do not touch the forest

The forest roots appear in the block header as a commitment to this
projection of the state.

### Projection 2: Private (Commitment Tree + Nullifier Set)

The commitment tree is a **projection** of the ledger onto the set of
existing shielded notes. The nullifier set is a **projection** onto the
set of spent shielded notes.

When a block is applied:
- Shielded outputs append commitments to the tree
- Shielded spends insert nullifiers into the set
- Transparent components do not touch these structures

The tree root and nullifier count appear in the block header.

### Projection 3: Economic (VWU)

VWU is not state — it is a **constraint** on which transactions are
admitted into a block. It prices computation across both domains using
a single metric:

| Component | VWU Cost |
|-----------|----------|
| Transparent input (P2TR) | stripped_bytes + witness_bytes + 0 |
| Transparent input (P2MR) | stripped_bytes + witness_bytes × weight + verify_cost |
| Shielded spend | SHIELDED_SPEND_VWU (fixed: 5000) |
| Shielded output | SHIELDED_OUTPUT_VWU (fixed: 500) |
| Transparent output | output_bytes |

Mempool admission: `fee / VWU(tx) >= min_relay_rate`
Block template: sorted by `fee / VWU(tx)` descending

This ensures transparent and shielded transactions compete in the same
fee market. Privacy costs more VWU because proof verification is more
expensive than signature verification — but the pricing is explicit.

---

## Block Application (Atomic)

A block is applied as a single atomic operation. There is no point during
application where some projections are updated and others are not.

```
fn apply_block(state: &mut LedgerState, block: &Block) -> Result<(), Error> {
    // Phase 1: Validate ALL transactions (no state mutation)
    for tx in block.transactions:
        validate_transparent(tx, &state.utreexo)?
        if tx.has_shielded_bundle():
            validate_shielded(tx.bundle, &state.tree, &state.nullifiers)?
        validate_conservation(tx)?  // THE conservation law
        validate_vwu(tx)?

    // Phase 2: Apply ALL state changes (atomic commit)
    for tx in block.transactions:
        apply_transparent(tx, &mut state.utreexo)
        if tx.has_shielded_bundle():
            apply_shielded(tx.bundle, &mut state.tree, &mut state.nullifiers)
    state.tip_height += 1

    // Phase 3: Verify header commitments match computed state
    assert(block.header.utreexo_roots == state.utreexo.roots())
    assert(block.header.shielded_tree_root == state.tree.root())

    Ok(())
}
```

Phase 1 is pure (no mutation). Phase 2 is atomic (all-or-nothing).
Phase 3 is a consistency check. If any phase fails, the block is rejected
and no state changes.

---

## Reorg (Atomic Undo)

Block disconnection is the inverse of application, applied atomically:

```
fn disconnect_block(state: &mut LedgerState, block: &Block) {
    // All three projections roll back together
    reverse_transparent(block, &mut state.utreexo)
    state.tree.truncate_to(pre_block_size)
    state.nullifiers.rollback_above(block.height - 1)
    state.tip_height -= 1
}
```

There is no state where one projection has rolled back and another hasn't.

---

## Boundary Rule (Structural, Not Policy)

```
Shielded commitments are NEVER inserted into Utreexo.
Transparent UTXOs are NEVER inserted into the Commitment Tree.
```

This is not a policy choice — it is a structural invariant of the state
machine. The two projections index different aspects of the same ledger.
Mixing them is a type error, not a design tradeoff.

The previous CT/ring system violated this by inserting confidential outputs
into Utreexo with `value=0`. This was equivalent to storing a float in an
integer column — it compiled, ran, and corrupted 3474 entries before the
inconsistency was detected.

---

## Transaction Version Map

| Version | Transparent | Shielded | Status |
|---------|:-:|:-:|--------|
| v1 (legacy) | Yes | No | Frozen (consensus accepts, wallet won't create) |
| v2 (segwit) | Yes | No | Active — P2WPKH, P2TR, P2MR |
| v3 (ring) | Yes | No | **DEAD at block 4000** |
| v4 (covenant) | Yes | No | **DEAD at block 4000** |
| v5 (shielded) | Optional | Optional | New — shield, transfer, unshield |

A v5 tx with only transparent I/O is semantically identical to v2.
A v5 tx with only shielded I/O is a pure private transfer.
A v5 tx with both is a shield or unshield operation.

---

## What This Spec Defines

- One atomic state transition per block
- One conservation law across all domains
- Three projections (verification aids, not independent systems)
- One fee market (VWU prices everything)
- One boundary invariant (structural, not policy)

## What This Spec Does NOT Define

- ZK circuit design (proving system, constraint count)
- Poseidon parameters (width, rounds)
- Encrypted note format (viewing keys, memos)
- Wallet UX (shield/unshield flows)
- Nullifier set accumulator (for stateless private verification)

---

## One-Line Summary

A Dinero block is a single atomic state transition with three projections
(transparent, private, economic) governed by one conservation law — the
projections partition verification work but do not partition validity.
