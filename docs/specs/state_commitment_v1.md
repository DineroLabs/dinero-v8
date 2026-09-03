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

## Demonstrated: a forged shielded section is accepted today

This is not hypothetical. Reproduced end to end against the live production
artifact on 2026-09-02.

**Setup.** Downloaded the fleet's live signed snapshot at **height 100,793**
(39,041,138 bytes). That height has **no compiled trust anchor** — which is the
point: it is exactly the artifact an anchor-free model accepts, and exactly what
the publisher serves today.

Flipped **one bit** inside the v4 `SHLD` section's anchor-history bytes, then
recomputed the file's trailing checksum. That checksum is simply
`sha256(file[:-32])` stored in the last 32 bytes, so whoever produces the file
controls it — it authenticates transport, not authorship.

The UTXO/forest section was left untouched, so the header `utreexo_root`
binding still matches.

**Result — both nodes loaded, at the same height:**

```
good     shielded_root = 4e38370945bd61291354ef588947af22e1f3a1b41ee39a488937010ce9873596
forged   shielded_root = 40f4e7d7fa85751bb9b90453ddda0bec061d7737afb0dffa283ae77623fe3795

[LoadSnapshot] Checksum verified successfully
[LoadSnapshot] v4 shielded section: frontier=1032B anchors=3608B nullifiers=14B
[snapshot] loaded — node usable at height 100793; background validation running
```

The forged snapshot passed every check that exists:

| check | outcome on the forged file |
|---|---|
| in-file trailing checksum | passes — recomputed by the forger |
| header `utreexo_root` binding | passes — the UTXO section was not touched |
| compiled trust anchor | **not consulted — height 100,793 is not registered** |
| shielded state | **nothing checks it** |

The shielded root *detects* the tamper — the two values differ. Nothing
*consults* the shielded root, because no header commits it. That single sentence
is the whole case for this document.

### The anchor is what catches this today, and it is what we are removing

The same bit-flip applied to the **99,677** snapshot *was* rejected:

```
[LoadSnapshot] Snapshot content does not match built-in trust anchor at height 99677
  (expected d4b8d88c..., got f286c7ee...)
[snapshot] rejected — base present but load failed; fallback to full sync
```

Note what did the work: the anchor's pinned **file hash**, not any
shielded-aware check. That protection exists only at registered heights, needs a
release per snapshot, and is precisely what the ~3,800-blocks-per-day drift
forces operators to abandon. **Moving to the anchor-free model removes the only
mechanism currently catching this** — unless the header commitment replaces it.

### Scope of this demonstration

It shows **undetected acceptance of altered shielded state**. It is *not* a
working theft: crafting a value-stealing forgery — a valid-but-malicious anchor,
or a nullifier omission that unlocks a specific note — was deliberately not
attempted. The gap it establishes is integrity, and integrity is the
precondition for the theft scenarios described above.

### An open question, not a finding

Background validation replays forward **from** the snapshot base. A forged
*base* state may therefore never be re-derived, and so may never be caught at
all — as opposed to being caught late. This has not been verified and should be
before any activation decision.

## The value

Tag `SHR1`, **version 2**. SHA-256 over:

```
[tag 'SHR1']                    4 B
[version = 2]                   1 B
[shielded tree root]           32 B   (zero-filled if not exactly 32 B)
[shielded tree size]            8 B   little-endian
[nullifier accumulator]        32 B   ComputeNullifierAccumulator, tag 'NUL1'
[anchor history length]         8 B   little-endian
[anchor history bytes]     variable   AnchorHistory::SerializeBytes()
```

### The canonical nullifier accumulator (tag `NUL1`, version 1)

```
[tag 'NUL1']        4 B
[version = 1]       1 B
[entry count]       8 B   little-endian
  per entry, in canonical order:
[block height]      4 B   little-endian
[nullifier]        32 B
```

Canonical order is **height ascending, then nullifier bytes ascending as
unsigned octets**, applied inside the accumulator. Duplicates collapse; the
count is committed, so an entry cannot be dropped and the remainder re-padded.

**Why v1's approach was replaced.** v1 hashed `NullifierSet::SerializeContent()`
bytes directly, inheriting two properties from the storage layer that a header
commitment must not depend on:

1. **Canonicality was delegated to SQLite.** Ordering came from
   `ORDER BY block_height ASC, nullifier ASC` inside the query, so a consensus
   value depended on BLOB collation semantics and on that query never being
   reworked for performance. The ordering a commitment depends on has to be
   stated by the commitment.

2. **It failed open.** `SerializeContent()` returns an empty vector to signal a
   read error — *"signal error via empty"*. Hashed directly, that is bit-for-bit
   the digest of an empty nullifier set, so a local database fault produces the
   same commitment as an attacker who deleted every nullifier. That is the
   omitted-nullifier forgery, reachable without an attacker.

`AccumulateNullifierSet()` returns `std::optional` and yields `nullopt` on any
enumeration failure. `ComputeShieldedRoot()` propagates it, and the
`daemon.shieldedroot` RPC reports `nullifier_set_unreadable` rather than a
digest. An unreadable set and an empty set are different facts and the API
cannot express them as the same value.

v1 was never activated on any network, so there is no compatibility obligation.

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
| 32 × `00` | 0 | empty accumulator | *(empty)* | `6d08ae4f5424b20f752690ef55dee04ef8c97cedc3989284c1b283cf56f79b93` |

Empty nullifier accumulator (`NUL1`, zero entries):
`4f760a0ef5ff321a3eaa0feca778ff19c9126aefd506b91732d747495df62696`

Observed values, for orientation (not fixtures):

Observed under **v1** (superseded; retained so the version change is auditable):
empty live state `a950379977bd7cf7…`, mainnet @ 99,677
`2439253378763e7606dc7a720bbd09875e83a5ba04458941c441cf76ad5de56e`.

## Evidence gathered

| property | method | result |
|---|---|---|
| layout is unambiguous | 14 unit tests, neuter-verified | PASS |
| omitted / truncated / reordered nullifiers detected | unit vectors, every position | PASS |
| injected anchor detected | unit vectors | PASS |
| restart determinism | real mainnet state @ 99,677, bootstrap → restart | PASS |
| live vs reindex | regtest, tree + anchors + a real nullifier | PASS |
| connect/disconnect/reorg invertibility | `test_shielded_reorg_invertibility.sh` | PASS |
| cross-node agreement + deep reorg through a shielded spend | `test_shielded_root_multinode_deep_reorg.sh` | PASS |
| canonical ordering + fail-closed enumeration | `test_nullifier_accumulator.cpp`, 10 tests | PASS |
| forged shielded section is accepted at a non-anchored height | live 100,793 artifact, one bit + recomputed checksum | **ACCEPTED — the gap** |
| same forgery at an anchored height | 99,677 artifact | rejected, by the anchor's file hash only |

## Still owed before any activation

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
