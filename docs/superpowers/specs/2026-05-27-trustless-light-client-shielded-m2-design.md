# Trustless Light-Client Shielded Scanning — M2 Daemon Output Feed Design

**Status:** Draft
**Date:** 2026-05-27
**Scope:** dinero-v8 daemon RPC + storage helpers; DineroDPI consumer noted but not implemented here
**Primary engineering site:** dinero-v8
**Depends on:** M1 receive primitives merged in DineroDPI PR #1 and T8 systems wiring merged in DineroDPI PR #2

## Goal

Close the T8 reach gap for iOS thin-client shielded receive.

After M1 + T8, DineroDPI can scan shielded outputs end-to-end once a block reaches the existing matched-block hook. That hook is still driven by transparent DNRF matches. A wallet with only shielded activity may never request the block that contains its note.

M2 adds a daemon-side light-client RPC surface that lets iOS discover and scan shielded outputs directly, without downloading full blocks and without sending any view key, diversifier, address, or wallet fingerprint to the daemon.

## Non-goals

- **No recipient-specific shielded compact filter in M2.** The current v5/v6 shielded output format does not expose a recipient tag that a client can precompute privately. `epk` is sender-random, `pk_d` and `d` are inside the encrypted note, and the daemon cannot know recipient diversifiers. A wallet-specific filter would require a future wire-format change that adds a sender-provided viewing tag.
- **No view-key sharing.** iOS still trial-decrypts locally. The daemon only returns public chain data.
- **No spend witness RPC.** `shielded.witness.by_index` belongs to M3.
- **No consensus change.** M2 is an RPC/indexing optimization over already-committed v5/v6 shielded bundle bytes.

## Design Decision

M2 ships a shielded output feed, not a recipient filter.

The feed returns the minimum public data needed for iOS to trial-decrypt, maintain its local commitment tree, and mark already-known notes spent:

- `height`
- `block_hash`
- `txid`
- `tx_index`
- `spent_nullifiers`
- `output_index`
- `leaf_index`
- `commitment`
- `encrypted_note`

iOS scans every returned `encrypted_note` with its local IVK. This preserves the M1 privacy model: the daemon learns the requested height range, but does not learn which outputs decrypt successfully.

## RPC Surface

### `blockchain.shielded.outputs`

Batch fetch shielded outputs by height range.

Request, named form:

```json
{
  "from_height": 8650,
  "count": 2000
}
```

Positional form mirrors `blockchain.getblockfilters`:

```json
[8650, 2000]
```

Response:

```json
{
  "from_height": 8650,
  "count": 2000,
  "tip_height": 30123,
  "blocks": [
    {
      "height": 12847,
      "block_hash": "0000...",
      "shielded_spend_count": 1,
      "shielded_output_count": 2,
      "spent_nullifiers": [
        { "txid": "beef...", "tx_index": 2, "spend_index": 0, "nullifier": "32-byte-hex" }
      ],
      "outputs": [
        {
          "txid": "abcd...",
          "tx_index": 3,
          "output_index": 0,
          "leaf_index": 912,
          "commitment": "32-byte-hex",
          "encrypted_note": "611-byte-hex"
        }
      ]
    }
  ]
}
```

Blocks with zero shielded outputs and zero shielded spends are omitted from `blocks`; `count` still reports the scanned height span, not the number of returned blocks. This keeps the response compact and lets iOS advance sync progress deterministically by height.

### `blockchain.shielded.outputsummary`

Optional first task if implementation wants a cheap progress/presence endpoint:

```json
{
  "from_height": 8650,
  "count": 2000,
  "blocks": [
    { "height": 12847, "block_hash": "0000...", "shielded_spend_count": 1, "shielded_output_count": 2 }
  ],
  "tip_height": 30123
}
```

This is not required for correctness. It can be skipped if `blockchain.shielded.outputs` is fast enough.

## Leaf Index Semantics

`leaf_index` is the global shielded commitment-tree position at the time the output is appended. Spend nullifiers do not affect `leaf_index`; they are included only so the client can compare public nullifiers against locally known notes and mark those notes spent.

Ordering is consensus order:

1. Block height ascending
2. Transaction order within the block
3. Canonical output order within each shielded bundle

This matches `ApplyBlockShielded` in `src/consensus/shielded/shielded_block_validation.cpp`: bundles are processed in block transaction order, and bundle outputs are already canonical by commitment due to `DeserializeShieldedBundle`.

M2 can compute `leaf_index` by walking shielded outputs from activation height forward. Persisting the index is allowed but not required for the first implementation, but the cost model must be honest: the first request for a high `from_height` walks `[shielded_activation_height, from_height)`, then the requested batch. That is acceptable under current chain size, but the sanity log must measure it. If persisted, it must be keyed by block hash, not height alone, so reorgs naturally invalidate stale entries.

## Storage

Two acceptable implementation paths:

1. **On-demand extraction only.** For each requested height, load the block, parse v5/v6 shielded bundles, accumulate output metadata, and return it. This is simplest and safe for M2.
2. **ChainDB sidecar cache.** Add a RocksDB prefix for shielded output summaries keyed by block hash. Populate during `ConnectTip` and backfill on RPC miss. This is faster but adds invalidation surface.

Recommended M2 implementation: start with on-demand extraction, then add cache only if benchmarked RPC latency demands it.

## Security

- The daemon response is not trusted for ownership or balance. iOS still trial-decrypts, verifies commitment bytes against the output, and maintains its own commitment tree.
- The block hash is anchored by the existing header chain.
- A malicious endpoint can omit outputs or serve an old tip; this is the same DoS class as existing light-client RPC. Multi-endpoint fallback handles liveness.
- A malicious endpoint cannot forge spendable notes because encrypted-note decrypt, commitment binding, and local tree roots are checked on-device.

## Performance

Compared with full `blockchain.getblock` fetch:

- Transparent transaction bytes are omitted.
- Shielded proofs are omitted.
- Only public nullifiers plus the 32-byte commitment and 611-byte encrypted note are returned.

At 10 shielded outputs per block, response payload is roughly 6.5 KB per shielded block plus JSON overhead. This is larger than a hypothetical recipient filter but much smaller than scanning full blocks and preserves today’s wire format.

## iOS Consumer Shape

DineroDPI adds a `ShieldedFilterDiscovery` or extends the existing sync driver:

1. Fetch `blockchain.shielded.outputs` in height batches from `shielded_activation_height`.
2. For each returned block, compare `spent_nullifiers` against locally known note nullifiers and mark matches spent.
3. Build `ShieldedBlockOutputs(blockHash, blockHeight, outputs)`.
4. Call the existing `ShieldedScanner.scan(block:)`.
5. On reorg, reuse the already-wired `FilterChainSync` rewind hook into `ShieldedScanner.handleReorg`.

T8’s matched-block hook remains useful for mixed transparent/shielded wallets, but M2 becomes the complete path for shielded-only wallets.

## Test Plan

- `ShieldedOutputFeedExtractTest`: synthetic block with no shielded bundle returns zero outputs.
- `ShieldedOutputFeedOneBundleTest`: block with one daemon-serialized bundle returns commitment + encrypted note bytes exactly.
- `ShieldedOutputFeedMultiTxOrderTest`: multiple v5/v6 txs return outputs in consensus append order.
- `ShieldedOutputFeedLeafIndexTest`: outputs across multiple blocks receive monotonically increasing leaf indexes.
- `ShieldedOutputFeedNullifierTest`: shielded spends return public nullifiers in block tx order without changing output leaf indexes.
- `BlockchainShieldedOutputsRpcTest`: RPC over a regtest or fixture chain returns only blocks with shielded outputs and includes `tip_height`.
- `BlockchainShieldedOutputsMalformedBundleTest`: malformed bundle returns a structured RPC error rather than silently dropping outputs.

## Rollout

M2 is backwards-compatible:

- Older daemons do not expose `blockchain.shielded.outputs`; iOS keeps using T8’s matched-block hook.
- New daemons expose the feed; iOS prefers it and gets complete shielded-only reach.
- No consensus activation height is required.
