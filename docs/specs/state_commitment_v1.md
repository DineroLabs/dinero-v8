# state_commitment_v1 — binding shielded state to the chain

**Status:** design + evidence. No consensus code exists yet. Nothing in this
document is active on any network.

## The gap

`header.utreexo_root` already binds the transparent half of an AssumeUTXO
snapshot to the chain. `LoadSnapshot` recomputes the forest commitment and
refuses to load on mismatch, so a node that has validated the header chain by
proof-of-work can verify the UTXO set against the chain itself, trusting no
publisher.

The shielded half has no such binding. The shielded pool is, by design, a
separate state machine:

> *"Architecturally separate from Utreexo. No code path in this library inserts
> a shielded commitment into the Utreexo accumulator."*
> — `src/consensus/shielded/CMakeLists.txt`

So `utreexo_root` is silent about the shielded commitment tree, the anchor
history, and the nullifier set. A snapshot's shielded section is currently
authenticated by whichever of these an operator happens to have:

| mechanism | who vouches | goes stale? | ongoing cost |
|---|---|---|---|
| compiled-in trust anchor | release reviewers | yes, ~3,800 blocks/day | a release per refresh |
| ed25519 signed manifest | the key holder | no | key custody + rotation, forever |
| **header commitment** | **nobody — the chain** | **never** | **none** |

## What forging the shielded section buys an attacker

None of these are visible to `utreexo_root`:

* **Omit nullifiers** → previously-spent shielded notes appear unspent. A
  shielded double-spend.
* **Inject an anchor** → a spend proof can reference a commitment-tree state
  that never existed on the real chain. Minting from nothing.
* **Alter the tree frontier** → changes what every later spend proves against.

The section cannot simply be dropped from snapshots: a bootstrapped node needs
the anchor history to validate incoming spends and the nullifier set to reject
double-spends, and neither is derivable without replaying the blocks the
snapshot exists to skip.

## The value

Tag `SHR1`, version 1. SHA-256 over:

```
[tag 'SHR1']                    4 B
[version = 1]                   1 B
[shielded tree root]           32 B   (zero-filled if not exactly 32 B)
[shielded tree size]            8 B   little-endian
[nullifier content length]      8 B   little-endian
[nullifier content]        variable   NullifierSet::SerializeContent(),
                                      sorted (block_height ASC, nullifier ASC)
[anchor history length]         8 B   little-endian
[anchor history bytes]     variable   AnchorHistory::SerializeBytes()
```

Implementation: `src/consensus/shielded/shielded_root.cpp`. Pure function, no
chainstate, no I/O.

### Why not reuse DSR2

`ChainstateService::ComputeShieldedReorgStateHash()` (tag `DSR2`, v2) already
hashes these same three containers, is canonical and versioned, and agrees
across independently-synced fleet nodes. It is deliberately **not** reused:

1. **It also hashes the utreexo forest**, which `header.utreexo_root` already
   commits. Committing it twice would tie a consensus header value to forest
   serialization — any future change to forest encoding would alter it.

2. **It has no length framing.** DSR2 concatenates nullifier content directly
   with anchor bytes. For a reorg-invertibility oracle that is harmless. For a
   value committed in a header it is not: with the boundary inferred from
   content rather than declared, a byte moved across it can produce the same
   preimage from different state. Every variable-length section here carries
   its length.

Point 2 is enforced by `ShieldedRoot.SectionBoundaryIsUnambiguous`, which is
neuter-verified: removing the length prefixes (i.e. adopting DSR2's layout)
makes it fail.

## Placement — undecided, both viable

A node performing snapshot bootstrap **has headers and no blocks**. That single
fact decides the trade:

| option | verification cost | fork type |
|---|---|---|
| new header field | zero extra fetches | hard fork; header grows past 128 B |
| the 12 reserved header bytes | zero extra fetches | hard fork (reserved is consensus-zero), **no size change** |
| coinbase commitment | one block — the snapshot base, whose merkle root is already checkable against the validated header | soft-forkable |

Precedent exists for height-gated forks: `UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET`
= 60,000 and `shielded_epoch_reset_height` = 61,000.

**The reserved-bytes option is only 96 bits.** Against an attacker who is not
the miner the relevant attack is second-preimage (2⁹⁶, safe). Against a mining
attacker who can vary transactions while grinding a fake state it is a
collision (2⁴⁸ by birthday bound) — mitigated by each attempt requiring a full
shielded-state serialization and by the burial requirement, but thinner than a
money-protecting consensus value deserves. It buys a fork with **no wire-format
size change**, which matters because the live pool and miner stack assume a
128-byte header.

## Adoption rule (post-activation)

A snapshot is acceptable when **all** hold:

1. its base block is on the node's own PoW-validated header chain;
2. the base is buried at least *N* blocks below the header tip;
3. `snapshot.utreexo_root == header.utreexo_root`, and the forest recomputes to
   it (already enforced today);
4. `snapshot.shielded_root == header.shielded_root`.

The compiled-in anchor registry then becomes unnecessary. Note it is already an
**optional** gate, not a restriction on which heights may load:

```cpp
// "Snapshot trust anchors (optional hard gate)"
if (auto anchor = AssumeUTXORegistry::GetSnapshot(header.block_height)) { ... }
```

## Test vectors

From `tests/consensus/test_shielded_root.cpp`. The first is pinned as a
regression lock: if it changes, the layout changed, which after activation is a
chain split.

| tree root | size | nullifier content | anchor bytes | digest |
|---|---|---|---|---|
| 32 × `00` | 0 | *(empty)* | *(empty)* | `d4c4c8cab0145d981b138bd960fbb9437a537f2630a840c5916c24139ffbbbd2` |

Observed values, for orientation (not fixtures):

| context | shielded_root |
|---|---|
| empty shielded state, live node | `a950379977bd7cf7900bb1742bd88a6d9ede59827c8ff1e349a7230781442b3c` |
| mainnet @ 99,677 (frontier 1032 B, anchors 3608 B, nullifiers 0/0) | `2439253378763e7606dc7a720bbd09875e83a5ba04458941c441cf76ad5de56e` |

## Evidence gathered

| property | method | result |
|---|---|---|
| layout is unambiguous | 14 unit tests, neuter-verified | PASS |
| omitted / truncated / reordered nullifiers detected | unit vectors, every position | PASS |
| injected anchor detected | unit vectors | PASS |
| restart determinism | real mainnet state @ 99,677, bootstrap → restart | PASS |
| live vs reindex | regtest, tree + anchors + a real nullifier | PASS |
| connect/disconnect/reorg invertibility | `test_shielded_reorg_invertibility.sh` | PASS |
| cross-node agreement + deep reorg through a shielded spend | `test_shielded_root_multinode_deep_reorg.sh` | see run log |

## Still owed before any activation

* canonical nullifier accumulator (this layout depends on `SerializeContent()`
  ordering being canonical — that guarantee should be structural, not incidental)
* end-to-end corrupt-snapshot rejection at the loader (requires the commitment
  to exist; unit vectors prove detection, not enforcement)
* scheduled activation height + compatibility period, clear of any future
  `shielded_epoch_reset_height`
* independent review

## Why determinism is the whole risk

Two live-vs-reindex divergences in exactly these containers have already been
found and fixed, both documented as would-have-been consensus splits:

* the reindexer never persisted anchor history, so a reindexed node loaded a
  stale or empty blob (`reindexer.cpp`);
* a coinbase `vtx[0]` shielded bundle had to be skipped during reindex to match
  the live path, or the reindexed node got a different commitment tree, anchor
  history, and hash (`reindexer.cpp`).

Both are closed. They establish that this bug class is subtle and survives
review, which is why the evidence above is gathered while the value is a
diagnostic that enforces nothing.
