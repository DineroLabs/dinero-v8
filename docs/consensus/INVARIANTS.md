# Dinero Consensus Invariants

## These rules are non-negotiable. Violating any one is a consensus bug.

---

### 1. State Partition

Utreexo tracks transparent coin ownership. CommitmentTree tracks private
coin existence. NullifierSet tracks private coin spending. No structure
references objects from another structure. No shared index. No shared
lookup. No cross-referencing IDs.

### 2. Shielded Outputs Never Enter Utreexo

A shielded output produces a commitment leaf in the CommitmentTree.
It does NOT produce a UTXO. It does NOT enter the Utreexo accumulator.
No placeholder coins. No dual-recording. No fake UTXO representations.

### 3. Transparent Outputs Never Enter CommitmentTree

A transparent output produces a UTXO in the Utreexo accumulator.
It does NOT produce a commitment leaf. The two domains are disjoint
by construction — the same output cannot exist in both.

### 4. One Conservation Law

For every transaction: `value_in = value_out`. The sum of transparent
inputs plus shielded spend values equals the sum of transparent outputs
plus shielded output values plus fee. The fee is always transparent.
There is no hidden fee. This law is enforced per-transaction AND
per-block (global sum check).

### 5. Atomic Block Transitions

A block applies one atomic state transition. All projections (Utreexo,
CommitmentTree, NullifierSet) update together or not at all. There is
no point during block application where some projections are updated
and others are not. Reorg disconnects all projections atomically.

### 6. Privacy Is Transaction Policy

Privacy mode is determined by user intent, wallet policy, and fee
constraints — never by the destination address type. All address types
(Taproot, P2MR) can receive funds via either transparent or shielded
execution. The routing engine maps intent to execution path. Address
type determines cryptographic identity, not privacy level.

### 7. Canonical Encoding

Every v5 ShieldedBundle has exactly one valid byte representation.
Spends are sorted by nullifier ascending. Outputs are sorted by
commitment ascending. Varints use minimal CompactSize encoding.
Deserialize(Serialize(bundle)) == bundle. Serialize(Deserialize(bytes))
== bytes. Alternative encodings are rejected. This prevents two nodes
from computing different txids for the same logical transaction.

### 8. Deterministic Ordering

Commitment tree insertions follow block transaction order. Within each
transaction, outputs are inserted in canonical commitment order.
Nullifier insertions follow the same order. Every node processing the
same block computes the identical tree root and nullifier count.
Implementation-defined behavior is a consensus bug.

### 9. Nullifier Uniqueness

A nullifier may appear at most once in the entire chain history. Per-
transaction validation checks against the existing NullifierSet. Block-
level validation checks for inter-transaction duplicates within the
same block. A duplicate nullifier in any scope rejects the containing
block.

### 10. Scheme Extensibility

New signature schemes (FALCON-512, SPHINCS+) are added by: one registry
row (scheme_id, activation_height), one verifier wrapper, one purpose
code in the HD wallet spec. No change to the state partition, conservation
law, routing engine, or any invariant in this document. If adding a new
scheme requires modifying any invariant above, the scheme design is wrong.
