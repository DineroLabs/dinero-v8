# Proof Service Contract

```
Title:   Utreexo Proof Service Protocol
Status:  Active
Version: 1
Created: 2026-03-14
```

## 1. Overview

Bridge nodes (`NODE_UTREEXO_BRIDGE` service flag) generate and serve Utreexo
inclusion proofs to compact state nodes (CSNs). This document specifies the
proof response format, root-binding semantics, rejection behavior, retry
policy, and batch request handling.

Audience: wallet developers, mobile client authors, CSN implementors.

---

## 2. Proof Response Format

### 2.1 P2P: `utxoproof` / `utxoproofs` Message

Wire message sent by a bridge node in response to `getutxoproof` / `getutxoproofs`.

| Field                        | Type           | Description |
|------------------------------|----------------|-------------|
| `block_hash`                 | uint256        | Block this proof applies to |
| `block_height`               | uint32         | Height of the block |
| `accumulator_root_before`    | 32 bytes       | Forest root **before** applying the block |
| `accumulator_root_after`     | 32 bytes       | Forest root **after** applying the block |
| `proof_data`                 | BlockUtreexoData | Batched inclusion proof + spent output metadata |

**proof_data** contains:
- `accumulator_root_before` (redundant, for self-containment)
- `batch_proof`: positions + sibling hashes for all spent inputs
- `spent_outputs`: per-input metadata (txid, vout, amount, scriptPubKey)

### 2.2 RPC: `getutxoproof` Response

JSON-RPC response for a single UTXO proof query.

| Field               | Type     | Description |
|----------------------|----------|-------------|
| `txid`               | hex string | Transaction ID of the UTXO |
| `vout`               | uint     | Output index |
| `leaf_hash`          | hex string | SHA256 leaf hash (domain-separated, see DINERO-UTREEXO-SPEC) |
| `position`           | uint64   | Leaf position in the forest |
| `num_leaves`         | uint64   | Total leaves in the forest at proof time |
| `proof_size`         | uint     | Number of sibling hashes |
| `siblings`           | [hex]    | Ordered sibling hashes from leaf to root |
| `accumulator_root`   | hex string | Forest commitment this proof is valid against |
| `block_hash`         | hex string | Tip block hash at proof generation time |
| `height`             | uint     | Tip height at proof generation time |

---

## 3. Root Binding

### 3.1 What It Means

Every Utreexo inclusion proof is valid **only** relative to a specific
accumulator root. The root is a 32-byte SHA256 commitment over the entire
forest state (all UTXO leaves). A proof generated at root `R_A` is
**cryptographically meaningless** at root `R_B`.

### 3.2 Why It Matters

- A proof received from a bridge is trustworthy only if the client can
  verify the root matches a block header it trusts (PoW-validated).
- After every new block, the forest root changes. Cached proofs become stale.
- During reorgs, proofs may point to orphaned roots.

### 3.3 Verification Rule

A client receiving a proof MUST:

1. Obtain `accumulator_root` from the proof response.
2. Verify it matches the `utreexo_root` field of a PoW-validated block
   header at the reported `height` / `block_hash`.
3. If the root does not match any trusted header, **reject the proof**.

### 3.4 Chain Context Fields

The `block_hash` and `height` fields in the response identify **which chain
tip** the proof was generated against. These are informational — the binding
authority is the `accumulator_root` / `utreexo_root` in the block header.

---

## 4. NACK / Rejection Semantics

### 4.1 `utxoproofnack` Message

When a bridge cannot serve some or all requested proofs due to resource
pressure, it sends a NACK instead of silently dropping the request.

| Field             | Type     | Description |
|-------------------|----------|-------------|
| `reason`          | uint8    | Rejection reason code |
| `retry_after_ms`  | uint32   | Suggested delay before retry (milliseconds) |
| `block_hashes`    | [uint256]| Which requested hashes were rejected |

### 4.2 Reason Codes

| Code | Name          | Meaning | Retry? |
|------|---------------|---------|--------|
| 0    | `QUEUE_FULL`  | Proof generation queue at capacity | Yes, after `retry_after_ms` |
| 1    | `STALE`       | Chain reorged during proof generation | Yes, immediately with fresh request |
| 2    | `NOT_FOUND`   | Block not on canonical chain | No (unless reorg resolves it) |
| 3    | `SHUTDOWN`    | Node is shutting down | No |

### 4.3 When NACKs Are Sent

NACKs are only sent for **queue-full** rejections. Other rejection types
(block not found, non-canonical, stale) are silently skipped because:

- `NOT_FOUND`: The client asked for a hash the bridge doesn't have. No
  amount of retrying the same bridge will help.
- `STALE`: The proof was generated but the chain moved. The client will
  naturally re-request at the new tip.
- `SHUTDOWN`: The connection will close shortly.

Queue-full is the only case where the bridge has the data but temporarily
cannot serve it — the NACK tells the client "I can help, just not right now."

---

## 5. Retry Semantics

### 5.1 Exponential Backoff

On receiving a `QUEUE_FULL` NACK:

1. Wait at least `retry_after_ms` (typically 5000ms / 5 seconds).
2. On repeated NACKs, double the delay: 5s → 10s → 20s → 40s (cap at 60s).
3. Reset backoff after a successful proof response.

### 5.2 Bridge Rotation

If multiple bridge peers are available, rotate to a different bridge on NACK
rather than hammering the same overloaded bridge.

### 5.3 Rate Limiting (Bridge Side)

Bridges enforce per-peer rate limits independently of NACKs:

| Limit | Value | Window |
|-------|-------|--------|
| `getutxoproof` requests | 8 per peer | 5 seconds |
| Block hashes per request | 64 per peer | 5 seconds |
| Violation tolerance | 3 strikes | Then disconnect |

Exceeding these limits results in immediate disconnect, not a NACK.

---

## 6. Batch Request Behavior

### 6.1 Request

`getutxoproof` / `getutxoproofs` carries up to **16 block hashes** per
request (`MAX_BATCH_SIZE = 16`).

### 6.2 Partial Success

A batch request may return **fewer proofs than requested**. This is normal
and expected. Reasons for missing proofs:

| Reason | NACK sent? | Client action |
|--------|------------|---------------|
| Block not in bridge's database | No | Try a different bridge |
| Block not on canonical chain | No | Wait for reorg to resolve |
| Height not indexed yet | No | Retry after sync |
| Queue full (backpressure) | **Yes** | Backoff and retry |
| Proof stale (reorg during gen) | No | Re-request at new tip |

### 6.3 Response Ordering

Proofs are returned in the order they are generated, which may differ from
the request order. Clients MUST match responses by `block_hash`, not by
position in the response stream.

### 6.4 Mixed Success + NACK

A single batch request can produce **both** proof responses and a NACK. For
example, requesting blocks `[A, B, C, D]`:

- `A`: served from cache → `utxoproof` sent
- `B`: generated on-demand → `utxoproof` sent
- `C`: queue full → included in `utxoproofnack`
- `D`: block not found → silently skipped

Result: 2 proof messages + 1 NACK message (containing `C`). Block `D` has
no response at all.

---

## 7. Proof Freshness Guarantees

### 7.1 Bridge-Side Freshness Checks

Before serving a proof, the bridge validates:

1. **Cache freshness**: Cached proofs are checked against canonical chain.
   If a reorg invalidated the cached entry, it is evicted.
2. **Canonical height check**: The block must be on the best chain at the
   claimed height.
3. **Root continuity**: `root_after` in the cached proof must match the
   block header's `utreexo_root`.
4. **Async freshness re-check**: After proof generation completes (which
   may take 10-100ms in the worker pool), the result is re-validated
   against the current chain state before serving.

### 7.2 Client-Side Freshness

Clients SHOULD:

1. Track the `accumulator_root` of their most recently validated block.
2. On receiving a proof, verify `accumulator_root` matches a known header.
3. Discard proofs whose roots don't match any header in the client's
   validated chain.

---

## 8. Priority Classification

Bridge nodes classify proof requests by priority to manage queue pressure:

| Priority | Condition | Behavior |
|----------|-----------|----------|
| **TipCritical** | Block within 6 of tip | Never preempted |
| **Recent** | Block within 144 of tip (~1 day) | May preempt Historical |
| **Historical** | Older than 144 blocks | May be preempted by Recent or TipCritical |

When the queue is full:
1. Higher-priority requests preempt lower-priority queued tasks.
2. If no preemption is possible (queue full of same/higher priority), NACK.

---

## 9. Wire Format Reference

### 9.1 `getutxoproof` Request

```
varint          num_hashes    (1..16)
uint256[]       block_hashes  (32 bytes each)
uint32          flags         (reserved, set to 0)
```

### 9.2 `utxoproof` Response

```
uint256         block_hash
uint32          block_height
bytes[32]       accumulator_root_before
bytes[32]       accumulator_root_after
varint          proof_data_size
bytes[]         proof_data    (serialized BlockUtreexoData)
```

### 9.3 `utxoproofnack` Rejection

```
uint8           reason        (0=QUEUE_FULL, 1=STALE, 2=NOT_FOUND, 3=SHUTDOWN)
uint32          retry_after_ms
varint          num_hashes
uint256[]       block_hashes  (rejected hashes)
```

---

## 10. Wallet Proof Lifecycle (RPC)

### 10.1 `wallet.getproofbundle`

Batch-fetch proofs for all wallet UTXOs at the current tip. Returns a
root-bound bundle that clients can cache and check for staleness.

**Request:**
```json
wallet.getproofbundle [{"min_confirmations": 1, "spendable_only": true, "max_utxos": 500}]
```

**Response:**
```json
{
  "accumulator_root": "abc123...",
  "block_hash": "def456...",
  "height": 12345,
  "utxo_count": 42,
  "truncated": false,
  "proofs": [
    {
      "txid": "...", "vout": 0, "amount_una": 100000000,
      "leaf_hash": "...", "position": 1234, "num_leaves": 50000,
      "siblings": ["...", "..."],
      "success": true
    }
  ]
}
```

### 10.2 `wallet.proofstatus`

Lightweight staleness check. Clients call this periodically or before
spending to determine if their cached proofs need refreshing.

**Request:**
```
wallet.proofstatus <accumulator_root_hex>
```

**Response:**
```json
{
  "stale": true,
  "client_root": "abc123...",
  "current_root": "xyz789...",
  "current_height": 12346,
  "current_block_hash": "..."
}
```

### 10.3 `blockchain.getproofupdates`

Re-prove specific outpoints at the current tip. Used when a client knows
which UTXOs it needs fresh proofs for (e.g., before building a transaction).

If `root_from` matches the current root, returns `"no_update_needed"` without
regenerating proofs — making this safe to call frequently.

**Request:**
```json
blockchain.getproofupdates {
  "root_from": "abc123...",
  "outpoints": [
    {"txid": "...", "vout": 0},
    {"txid": "...", "vout": 1}
  ]
}
```

**Response:**
```json
{
  "status": "updated",
  "root_from": "abc123...",
  "root_to": "xyz789...",
  "height": 12346,
  "block_hash": "...",
  "proofs": [
    {"txid": "...", "vout": 0, "leaf_hash": "...", "position": ..., "siblings": [...], "success": true},
    {"txid": "...", "vout": 1, "success": false, "error": "UTXO not found or proof generation failed"}
  ]
}
```

### 10.4 Recommended Client Flow

```
1. On wallet open:
   response = wallet.getproofbundle()
   cache = { root: response.accumulator_root, proofs: response.proofs }

2. Periodically (every block / every 30s):
   status = wallet.proofstatus(cache.root)
   if status.stale:
       cache = wallet.getproofbundle()   // Refresh all

3. Before spending specific UTXOs:
   updates = blockchain.getproofupdates({
       root_from: cache.root,
       outpoints: [{ txid, vout }, ...]
   })
   if updates.status == "updated":
       merge updates.proofs into cache
       cache.root = updates.root_to
```

---

## 11. Security Considerations


1. **Proofs are untrusted data.** Always verify against a PoW-validated
   block header's `utreexo_root`.
2. **NACKs are advisory.** A malicious bridge could NACK to deny service.
   Rotate bridges on persistent NACKs.
3. **Batch size limits prevent DoS.** Never request more than 16 hashes.
4. **Rate limits are enforced.** Exceeding per-peer budgets causes
   disconnection, not NACK.
5. **Root binding prevents replay.** A proof valid at height N is
   meaningless at height N+1 without an update.
