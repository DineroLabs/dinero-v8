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

### Background validation does not catch it either — settled

This was an open question; it is now answered from the code.

Background validation does **not** merely validate forward from the base. It
performs a real genesis→base replay into an isolated in-memory consensus set,
and that replay **includes shielded state** — the engine owns a genesis-fresh
`CommitmentTree`, `NullifierSet` and `AnchorHistory`, replayed alongside the
transparent set, because without them a stateful `BlockValidator` would
hard-reject every shielded transaction in honest history.

So the honest shielded state *is* re-derived. It is simply never compared. At
completion the worker checks exactly three things, all transparent:

| check | source | covers |
|---|---|---|
| `VerifyUTXOSetMatch()` | snapshot metadata | UTXO count |
| `replay->RecordsDigestHex()` | `kExpectedCommitmentKey` | transparent records |
| `replay->UtreexoRootHex()` | `kExpectedUtreexoRootKey` | forest |

The engine's own header states the scope limitation:

> *"The records digest commits only the transparent set (shielded commitment
> scope: see plan Task 10 accounting)."*

**Consequence.** A snapshot with a forged shielded section is accepted at load
(demonstrated above), is never contradicted by background validation, and the
node is then **promoted to FullyValidated** — formally retiring the trust
assumption — while running on shielded state nothing ever verified.

### This one is fixable today, without the fork

The comparison is a small local addition, not a new subsystem, and it does
**not** depend on the header commitment. The replay engine already exposes

```cpp
const CommitmentTree* ShieldedTree()      const;
const NullifierSet*   ShieldedNullifiers() const;
const AnchorHistory*  ShieldedAnchors()    const;
```

which is exactly the trio `ComputeShieldedRoot()` takes. Persisting the
snapshot's shielded root at load beside `kExpectedUtreexoRootKey`, then
comparing it against the replay's at completion, closes the gap for the
snapshot-forgery case — because the replayed value is derived from genesis
history rather than from the file.

It is weaker than the header commitment in two ways, and does not replace it:
detection arrives only at replay completion (hours on mainnet) rather than at
load, and it protects a node that performs the replay rather than establishing
a value the whole network agrees on. But it upgrades the current outcome from
*"promotes itself to FullyValidated on forged state"* to *"fails validation"*,
and it is symmetric with the transparent check that already exists.

## The value

Tag `SHR1`, **version 2**. SHA-256 over:

```
[tag 'SHR1']                    4 B
[version = 2]                   1 B
[shielded tree root]           32 B   (REQUIRED; any other length is rejected)
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

## Placement — FROZEN: coinbase commitment

A node performing snapshot bootstrap **has headers and no blocks**. That single
fact drove the trade:

| option | verification cost | fork type | verdict |
|---|---|---|---|
| new header field | zero extra fetches | hard fork; header grows past 128 B | **avoided** |
| the 12 reserved header bytes | zero extra fetches | hard fork (reserved is consensus-zero), no size change | **rejected** |
| coinbase commitment | one block — the snapshot base, whose merkle root is already checkable against the validated header | soft-forkable | **CHOSEN** |

**Chosen: coinbase commitment.** It is still cryptographically bound to the
header, through the transaction merkle root:

```
commitment  <-  coinbase  <-  merkle_root  <-  PoW-validated header
```

the same trust chain the DNRF filter commitment already uses. The 128-byte
header stays frozen, so every CPU/GPU/SV2 mining implementation, template,
hardware assumption, and wire format is untouched. This remains a **consensus
fork**; it is not a **mining-format fork**.

**Rejected — the 12 reserved bytes.** 96 bits cannot hold a 256-bit commitment
without truncation or an indirect scheme. Against an attacker who is not the
miner the relevant attack is second-preimage (2⁹⁶, safe); against a mining
attacker who grinds transactions while varying a fake state it is a collision
(2⁴⁸ by birthday bound). Too thin for a value that protects money.

**Avoided — growing the header.** It would change PoW serialization, miners,
GPU kernels, SV2 templates, hardware assumptions, and networking, without
adding cryptographic strength over a properly proven coinbase commitment.

### Canonical script (39 bytes)

```
[0]     0x6a                OP_RETURN
[1]     0x25                push 37
[2..5]  0x44 4E 52 53       "DNRS" magic
[6]     0x01                script encoding version
[7..38] 32-byte root        state_commitment_v1, big-endian as produced
```

`DNRS` is deliberately distinct from the `SHR1` preimage tag: one
domain-separates the coinbase encoding, the other the digest preimage. A value
from one must never parse as the other. The script encoding version is separate
from `SHIELDED_ROOT_VERSION`, so the two can move independently.

### Rules

1. After activation, **exactly one** tagged, versioned commitment output in the
   coinbase. Zero is a rejection; two or more is a rejection — a second
   commitment would let one block claim two different shielded states. Note the
   DNRF precedent scans backwards and lets the last match win; this does not.
2. Before activation, **no** commitment is recognized.
3. After activation, **missing, duplicated, truncated, malformed, or incorrect**
   commitments are all rejections. A tagged-but-corrupt script must report
   *malformed*, never *absent* — treating corruption as absence is how a
   truncation slips past a presence check.
4. Snapshots carry the base block's **coinbase transaction and merkle branch**,
   so a bootstrapping node verifies the commitment against the PoW-authenticated
   header without fetching the block.
5. The committed value is **post-block** shielded state. A coinbase transaction
   cannot itself alter shielded state, so there is no circularity.

### Canonical byte representation (pinned)

`uint256::GetHex()` emits `data[31]` **first**: the display string is the
*reverse* of the raw array, and `FromHex()` reverses back. Three
representations are therefore legitimately in play, and mixing them is the
easiest way to fork the chain by accident.

| where | representation |
|---|---|
| coinbase script bytes `[7..38]` | **raw internal order**, `data[0..31]` as produced |
| RPC output, snapshot metadata (`assumeutxo_expected_shielded_root`) | **display hex**, `GetHex()` — reversed |
| snapshot ↔ replay comparison | `GetHex()` on both sides, so string equality is well defined |

The wire form is the raw array. Nothing may place `GetHex()` output into the
script, and nothing may treat script bytes as a display string.

**A self-consistent reversal is the dangerous failure.** If both the writer and
the parser were flipped, every round-trip test would still pass while the bytes
on the wire were wrong. Verified by mutation: with writer and parser both
reversed, 7 of the 8 canonical tests still pass. Only the one that compares
script bytes against the raw `uint256` array — refusing to use the parser as its
own oracle — catches it. Any future test of this encoding must keep that
property.

### Status

The **format is frozen**; activation is not. No activation height is selected
and no consensus enforcement is wired: `RequiresStateCommitment()` returns false
at every height and a test pins that. Enforcement belongs in a separate reviewed
change after this format has been reviewed.

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

This digest is **unchanged** by the 32-byte length requirement: it always used a
valid 32-byte root.

A root of any other length now produces **no digest at all**. It was previously
zero-filled, which meant a corrupt or truncated root committed to exactly the
same value as a genuinely empty tree. The committed vector file recorded that
collision plainly — `tree_len_1`, `tree_len_31`, `tree_len_33` and
`tree_len_64` all carried the single digest
`d4414d6601c8b1307a7a4258401a11ec07abbdfe5c866648b0c714d8677fcb63`. Those
entries now read `REJECTED`, and `SHIELDED_ROOT_VERSION` is deliberately **not**
bumped: no input the encoder still accepts changed by a single byte. See the
rationale block in `scripts/check_shielded_vectors.sh`.

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
| does background validation catch a forged shielded base? | code review of `BackgroundValidationWorker` | **NO — compares transparent set only** |

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
