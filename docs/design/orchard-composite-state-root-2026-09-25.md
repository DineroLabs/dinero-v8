# Draft Orchard composite state root

Status: root construction and read-only storage projection, not live DNRS
validation, a selected activation, or a completed snapshot format.

## Logical sets

The selected database view is read under the caller's chainstate writer lock.
The stored Orchard tip must match the caller's expected state exactly. Projection
merges a bounded new-nullifier list and one new anchor reference into that view,
without writing it or copying the complete historical sets.

Ordering is **unsigned raw-byte lexicographic order**, independent of uint256
numeric/display order. Every key has exactly 32 bytes. Duplicate newly supplied
nullifiers and already-present nullifiers reject. Malformed keys/owners/counts,
iterator errors, a missing current anchor, and nullifier count disagreement with
the current tree size are errors, never empty-state fallbacks.

Canonical set digests use single SHA-256:

```
ONF1 | 01 | sorted nullifier keys[32]... | count LE64
OAN1 | 01 | sorted (anchor[32], references LE64)... |
    distinct anchors LE64 | total references LE64
```

Fixed element widths and final counts make these unambiguous. Every current
Orchard action contributes one nullifier and one note commitment, including
dummy actions, so nullifier count equals tree size. Current retention keeps one
anchor reference per connected post-boundary block, including empty blocks.
A future pruning/retention rule requires an explicit protocol revision.

Nullifier **owner-block metadata is not hashed**: new entries name this block,
and hashing those owners in its own coinbase would be circular. Owner rows must
still be well formed; correct ancestry/backreferences and undo are separate
validation obligations. A matching logical-state digest alone is not permission
to trust arbitrary snapshot owner or undo metadata.

Scanning is linear in existing nullifiers/anchors, with bounded overlay memory.
No production performance claim is made; loaded-chain capacity qualification or
an independently reviewed incremental representation remains required.

## Composite preimage

The fixed-width 356-byte preimage, followed by one SHA-256, is:

```
DNORST01 (8 bytes)
transaction version LE32 | bundle profile U8 | pool profile U8 |
    circuit profile U8 | effect-commitment version LE32
network U8 | genesis[32] | branch LE32 | activation height LE32 |
    legacy epoch height LE32 | boundary parent[32] | legacy SHR1[32] |
    retired una LE64 | legacy tree root[32] | legacy tree size LE64 |
    legacy nullifier count LE64
current height LE32 | current parent[32]
Orchard anchor[32] | tree size LE64 | pool una LE64
nullifier digest[32] | nullifier count LE64 |
    anchor digest[32] | distinct anchors LE64 | total references LE64
```

Integers have explicit widths and hashes use raw wire bytes. Shared backend
constants supply the transaction/profile identities. The root builder checks
network/profile consistency, activation context, money bounds, set counts and
canonical frontier/root/size agreement through the pinned Orchard backend.
It excludes the current block hash, undo serialization, RocksDB ordering details
and frontier serialization. Those are not consensus state preimage fields.

The root is intended for the explicitly selected DNRS v2 script encoding;
legacy v1/SHR1 construction remains unchanged. The builder does not select the
network configuration or decide which height uses which encoding.

## Trust boundary and remaining work

`BuildOrchardStateRootPreimage` is a from-parts operation. It cannot authenticate
supplied set digests, historical SHR1/value, or the boundary's ancestry merely
by hashing them. The runtime connector must supply independently checked legacy
state and a selected ChainDB read/projection, enforce pool flows and proofs,
compare the coinbase commitment, stage retirement and companion state atomically,
and publish memory only after commit. Those callers are not enabled here.

This change supports forward set projection. Tests check readback after exact
undo; a pre-commit reverse projection and complete disconnect/root validation
remain integration work. Snapshot import must verify the root against an
authenticated selected header/coinbase and separately reconstruct or validate
excluded metadata. It must not trust a root supplied beside an untrusted snapshot.

## Verification

An independent Python implementation reads public synthetic bundle nullifiers
and a saved opaque Orchard tree root. It calculates set hashes, the preimage and
the root for a funded block and its empty child without linking Rust or C++.
It does not independently implement Orchard curve/tree hashing. CI checks these
saved vectors. C++ tests use real verified synthetic funding/spend plans, compare
projection with committed/reopened state, restore prior sets after undo, vary
root fields, reject malformed stored sets and test an initial empty block.
