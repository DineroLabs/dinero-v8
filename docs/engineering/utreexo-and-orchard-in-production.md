# Utreexo in the consensus header, and Orchard on top of it: a deployment report

*A production experience report from the Dinero project, maintainer of [`DineroLabs/dinero-v8`](https://github.com/DineroLabs/dinero-v8).*

## TL;DR

- Dinero is a small open-source proof-of-work cryptocurrency (MIT-licensed C++, [GitHub](https://github.com/DineroLabs/dinero-v8)) that launched on 17 April 2026.
- It commits the **[Utreexo](https://eprint.iacr.org/2019/611) accumulator root inside the 128-byte block header** — a consensus rule, not an optional node mode. See [`docs/BLOCK_HEADER_V1.md`](../BLOCK_HEADER_V1.md).
- On 10 November 2026 at block height 150000 it activated a **shielded transaction pool derived from the Zcash [Orchard](https://zcash.readthedocs.io/en/latest/rtd_pages/nu5.html) protocol**. The activation went in — and had rough edges worth being honest about. See [`docs/operations/activation-150000-mining-recovery.md`](../operations/activation-150000-mining-recovery.md).

This note explains what the design is, what seven months of production surfaced, how the Orchard activation went, and how to reproduce the claims. It is aimed at protocol engineers and researchers; the code is open and everything below can be checked directly against it.

## Why this is interesting

Utreexo — the hash-based UTXO accumulator designed by Dryja and others at the MIT Digital Currency Initiative — has mostly lived in research papers and in [`utreexod`](https://github.com/utreexo/utreexod), a Bitcoin node that uses it as node software behind an unchanged consensus layer. Dinero took the inverse choice: the accumulator root is part of the consensus rules themselves, committed inside every block header. A validating node proves coin existence against the accumulator in the header rather than against a multi-gigabyte UTXO database it maintains separately.

The practical consequences are:

- Every node, including a mobile wallet, can validate from the same compact primitive the miner committed to. There is no "UTXO set" to agree on out-of-band.
- UTXO-snapshot fast sync (AssumeUTXO-style) composes trivially: the authenticity of a snapshot is what the block header already commits to, so a node that assumes a snapshot at height *h* is making a weaker assumption than a node assuming checkpoint hashes of the same chain.
- Reorg handling is a property of the accumulator, not of the node's local database. A deeper reorg is more accumulator work, not a bigger database migration.

Dinero also includes a post-quantum signature scheme — NIST FIPS 204 ML-DSA-65 — as an accepted signature scheme from block 0, with registry slots reserved for FALCON-512 and SPHINCS+-128s. This is orthogonal to the Utreexo choice but relevant to the second half of this note, because it constrains how transaction bodies can evolve post-activation.

## What the first seven months showed

Between the genesis block on 17 April 2026 and this writeup, the chain produced roughly 150,000 blocks at approximately two-minute cadence. The specific things that were being watched in that window:

**Header-native accumulator behavior under reorgs.** In practice the reorg depths we observed were the ones a small chain would be expected to see — mostly 1–2 blocks, occasionally 5+. The accumulator handled these without any special-case path. The code path for "undo an accumulator step under a reorg" is the same inside and outside reorg handling.

**UTXO-snapshot fast sync.** Nodes that come up fresh and fast-sync from a published snapshot verify against the accumulator root in the header at the snapshot height. The snapshot is thus a checkpointed accumulator state, not a checkpointed UTXO table. In every snapshot-start we observed, the first block built on the snapshot validated normally; there was no post-snapshot catch-up period where the node was running a weaker rule set.

**Mobile validation.** The same Utreexo validation logic runs inside the mobile wallets (via a C++ library called NodeCore), so the phone verifies for itself rather than trusting a server. Over seven months we have not had a case where a mobile node and a desktop node disagreed on validity — which is the obvious thing to worry about any time there are two implementations.

What we have *not* yet produced is the stressed numbers: a deeper reorg, a chain reorg against a snapshot, a header-only sync of the whole chain under adverse latency. These are the next things we want to publish controlled measurements on.

## Block 150000: activating Orchard on top of Utreexo

On 10 November 2026 at block height 150000 Dinero activated a shielded transaction pool derived from Zcash's Orchard protocol. The canonical block 150000 is hash `00000000c7b0a0a1f94547bf4cb6a6c8446a7ac3e50682c01477685b46939fd2`; it was mined by one of the project's own mining nodes and was accepted by every independent node that was running the correct binary.

The activation worked, meaning the consensus rules that were supposed to change at height 150000 changed, and no invalid-accept has been found in any audit since. The fleet-recovery story around it is less flattering and worth telling honestly, because the right lessons live there.

What went well: the actual cryptographic layer. The pool uses the post-NU6.2 Orchard circuit (upstream crate `orchard` version 0.15.5, `halo2_gadgets` 0.5.0, Cargo.lock + `--locked`, no git dependencies). A security review before activation found no proof-bypass, inflation, or double-spend path. The pool-balance turnstile is proof-independent and per-transaction; it refuses on underflow. The verify path has no bypass.

What went poorly: the activation-day machinery around the cryptographic core.

1. **The embedded solo miner shipped speaking legacy `getblocktemplate`.** `getblocktemplate` cannot carry the typed Orchard body and proof. At the Orchard boundary, every legacy-miner request failed with a generic "Selected Orchard mining construction unavailable" error that didn't clearly point at the real cause. Operators saw the error and did what operators do: retry. See [`docs/operations/activation-150000-mining-recovery.md`](../operations/activation-150000-mining-recovery.md).

2. **The daemon acquired its mining owner before rejecting legacy callers**, so every retry ran a full historical prefix replay before being refused. Under concurrent retries this degenerated into stacked replays of the same ~150,000-block prefix. The cumulative wall-clock cost was hours per server.

3. **On one server (our European node) an unrelated backup system held hardlinks to the live database's SST files**, which blocked a storage-layout migration triggered by the activation. The refusal diagnostic from the storage layer at that site was `selected-05-stored-state`, which legitimately covers at least three distinct conditions: "state exists," "store unreadable," and "not migrated." The loop treated a permanent "not migrated" condition as a retryable one. That node spent ~13 hours in that loop before the hardlinks were retired and the migration proceeded.

4. **One other server (our North American node) crash-looped on the old binary** until its deploy completed. This was expected old-peer cutoff: the "upgrade-required" handshake rejections at the P2P layer are the intended signal, and the service did exactly what it was supposed to — refuse to serve blocks under incompatible rules.

The fix commits live in the repo; the shape is the right one. Embedded CPU and GPU miners now use the immutable `mining.getjob` / `mining.submit` interfaces that *can* carry the typed Orchard body and proof. The daemon refuses incompatible requests before preparation, so a stale legacy caller no longer triggers a historical replay. The boundary proof material is cached per service with a try-lock that prevents concurrent replays; cached material is bound to the storage owners, parent height/hash/work, full profile, and historical storage revision, and is invalidated at commit for staged historical writes. Full details are in the recovery doc linked above.

**The honest meta-lesson:** Dinero's cryptographic core was ready for activation. The operator-facing machinery around it — one miner version with a stale protocol, one diagnostic string overloaded across three causes, one backup process entangled with the live storage layout — was not. Every failure failed closed, which matters: no invalid block was ever accepted, no funds moved on an unverified proof, and no state rolled back. But the recovery took about 18 hours end-to-end across four servers, where the engineering could have let it take minutes.

If you are shipping an activation on your own chain, three things are worth stealing from this experience:

- Ship your miner *before* your daemon's refusal path, not with it.
- Split any diagnostic that overloads "transient failure" with "permanent misconfiguration." Retries should be a response to the first, not the second.
- Know what has a hardlink to your live database. Backup clones that coexist with the live tree by shared SSTs are a whole class of activation hazards.

## Reproducibility

Everything above can be checked without talking to us. To verify the header claim, build from source:

```
git clone https://github.com/DineroLabs/dinero-v8
cd dinero-v8
# follow docs/build/*.md for your platform
```

Then inspect any block header with the RPC:

```
dinero-cli getblockheader <block-hash> false
```

The returned 128 bytes decode as the layout in [`docs/BLOCK_HEADER_V1.md`](../BLOCK_HEADER_V1.md): 4 bytes version, 32 bytes prev hash, 32 bytes Merkle root, **32 bytes Utreexo root at offset 68**, 8 bytes 64-bit timestamp, 4 bytes difficulty, 4 bytes nonce, 12 reserved bytes.

To verify the Orchard activation, request block 150000 from any Dinero node:

```
dinero-cli getblock 00000000c7b0a0a1f94547bf4cb6a6c8446a7ac3e50682c01477685b46939fd2 2
```

A full-node re-sync from genesis, running the same binary that was in production on activation day, reaches the same tip height and the same tip hash the rest of the network has. If it does not, that is a bug — please file an issue.

## Reach out

The project is a small group of people who care about getting the engineering right and are willing to say when they didn't. If you are a protocol researcher, a journalist covering accumulator-based scaling or post-quantum migration, or a node operator curious to play with this stack, you are welcome to:

- Open an issue or discussion on [the main repository](https://github.com/DineroLabs/dinero-v8/issues).
- Ask for specific numbers (reorg depths, snapshot sync times, activation telemetry) — happy to provide them with the caveat that the production data set is small.
- Reproduce the claims above against a local or mainnet node; we will take bug reports.

— The Dinero project, 14 November 2026.
