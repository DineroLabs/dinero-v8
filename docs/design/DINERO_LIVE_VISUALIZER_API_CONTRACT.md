# Dinero Live Visualizer API Contract

**Status:** Draft  
**Scope:** Backend contract for a dashboard collector and UI consumer  
**Companion:** [`DINERO_LIVE_VISUALIZER_DASHBOARD_SPEC.md`](./DINERO_LIVE_VISUALIZER_DASHBOARD_SPEC.md)

## 1. Goal

Define the API between:

- Dinero nodes / fleet RPC sources
- a dashboard collector/normalizer
- the live visualizer frontend

This contract is intentionally **collector-centric**. It does not require every field to come from a single node RPC.

## 2. Architecture

### Layers

1. **Source layer**
   - local Dinero RPC
   - fleet Dinero RPCs
   - optional external explorer comparison

2. **Collector layer**
   - polls sources
   - derives parity / alert / health states
   - exposes normalized API

3. **Frontend layer**
   - renders views
   - does minimal computation

## 3. Data Provenance

Every response object SHOULD include:

- `source`
- `collected_at`
- `staleness_ms`

Allowed `source` values:

- `local`
- `fleet`
- `derived`
- `external`

## 4. Transport

### Required

- REST/JSON snapshot endpoints

### Recommended

- SSE endpoint for live events

### Optional

- WebSocket stream for future high-frequency updates

## 5. Top-Level Endpoints

MVP endpoints:

- `GET /api/v1/status`
- `GET /api/v1/stream/txs`
- `GET /api/v1/mempool`
- `GET /api/v1/candidate-block`
- `GET /api/v1/fleet`
- `GET /api/v1/utreexo`
- `GET /api/v1/blocks`
- `GET /api/v1/alerts`
- `GET /api/v1/tx/{txid}`
- `GET /api/v1/block/{hash}`
- `GET /api/v1/events` (SSE)

## 6. Common Envelope

All snapshot responses SHOULD follow:

```json
{
  "ok": true,
  "data": {},
  "meta": {
    "source": "derived",
    "collected_at": "2026-04-17T12:00:00Z",
    "staleness_ms": 420,
    "collector_version": "v1"
  }
}
```

On failure:

```json
{
  "ok": false,
  "error": {
    "code": "upstream_unavailable",
    "message": "Local node RPC unavailable"
  },
  "meta": {
    "source": "local",
    "collected_at": "2026-04-17T12:00:00Z",
    "staleness_ms": 5000,
    "collector_version": "v1"
  }
}
```

## 7. Status Endpoint

### `GET /api/v1/status`

Purpose:

- fill top status bar
- provide compact dashboard summary

Response shape:

```json
{
  "chain": "mainnet",
  "local_height": 3733,
  "fleet_median_height": 3733,
  "best_hash": "0000001c9a9c8a857b596fe90e703bf8190f79afaf4431bf27e494f3fba66f7f",
  "peer_count": 4,
  "mempool_tx_count": 0,
  "tx_per_second": 0.12,
  "mining_safety": {
    "state": "safe",
    "reason": null
  },
  "utreexo_health": {
    "state": "healthy",
    "reason": null
  },
  "activation": {
    "scheduled": true,
    "height": 4000,
    "blocks_remaining": 267,
    "active_at_tip": false
  }
}
```

## 8. Live Transaction Stream

### `GET /api/v1/stream/txs`

Parameters:

- `limit` default `50`, max `250`
- `include_confirmed` default `true`

Response item:

```json
{
  "txid": "abc123...",
  "short_txid": "abc123…9f",
  "state": "mempool",
  "type": "taproot",
  "amount_out": "12.50000000",
  "fee": "0.00010000",
  "weight": 784,
  "size": 196,
  "age_seconds": 14,
  "candidate_block": {
    "included": false,
    "reason": "excluded_by_policy"
  },
  "source": "local"
}
```

Allowed `type` values for v5:

- `transparent`
- `taproot`
- `op_return`
- `coinbase`
- `unknown`

Reserved for v7:

- `p2mr_receive`
- `p2mr_spend`

## 9. Transaction Detail

### `GET /api/v1/tx/{txid}`

Response shape:

```json
{
  "txid": "abc123...",
  "state": "confirmed",
  "block_height": 3733,
  "block_hash": "0000...",
  "confirmations": 1,
  "fee": "0.00010000",
  "weight": 784,
  "inputs": [
    {
      "prevout": "txid:vout",
      "amount": "1.00000000"
    }
  ],
  "outputs": [
    {
      "index": 0,
      "amount": "0.99990000",
      "script_type": "witness_v1_taproot",
      "address": "din1..."
    }
  ],
  "mempool": {
    "has_ancestors": false,
    "has_descendants": false
  },
  "candidate_block": {
    "included": true,
    "reason": null
  },
  "rejection": null
}
```

## 10. Mempool Endpoint

### `GET /api/v1/mempool`

Response shape:

```json
{
  "tx_count": 42,
  "total_weight": 240000,
  "total_bytes": 65000,
  "oldest_age_seconds": 122,
  "arrival_rate_per_second": 1.8,
  "fee_histogram": [
    { "min": 0, "max": 1, "count": 10 },
    { "min": 1, "max": 5, "count": 18 }
  ],
  "rejections_last_10m": [
    { "reason": "freeze_fork_gate", "count": 3 },
    { "reason": "template_self_verify_failed", "count": 1 }
  ]
}
```

## 11. Candidate Block Endpoint

### `GET /api/v1/candidate-block`

Response shape:

```json
{
  "height": 3734,
  "previous_block_hash": "0000...",
  "weight": 784,
  "tx_count": 0,
  "total_fees": "0.00000000",
  "coinbase_value": "100.00000000",
  "utreexo_root": "ddb9eb30...",
  "composition": [
    { "type": "taproot", "count": 0 }
  ],
  "included": [],
  "excluded": [
    {
      "txid": "abc123...",
      "reason": "excluded_by_mining_safety_gate"
    }
  ]
}
```

## 12. Fleet Endpoint

### `GET /api/v1/fleet`

Response shape:

```json
{
  "summary": {
    "tip_parity": true,
    "binary_parity": true,
    "mempool_parity": false,
    "activation_parity": true
  },
  "nodes": [
    {
      "name": "Mac",
      "role": "local",
      "commit": "130eefb0c",
      "height": 3733,
      "headers": 3733,
      "best_hash": "0000...",
      "peer_count": 4,
      "mempool_tx_count": 0,
      "initial_block_download": false,
      "mining_safety": "safe",
      "utreexo_health": "healthy",
      "drift": false
    }
  ]
}
```

## 13. Utreexo Endpoint

### `GET /api/v1/utreexo`

Response shape:

```json
{
  "root": "ddb9eb30...",
  "root_changed_at": "2026-04-17T12:00:00Z",
  "proof_generation_ms": 12,
  "proof_refresh_ms": 4,
  "coverage": {
    "state": "healthy",
    "missing_utxo_count": 0
  },
  "safe_mode": false,
  "recovery_marker_present": false,
  "template_self_verify_ok": true,
  "reindex_required": false
}
```

## 14. Blocks Endpoint

### `GET /api/v1/blocks`

Parameters:

- `limit` default `20`, max `100`

Response item:

```json
{
  "height": 3733,
  "hash": "0000...",
  "age_seconds": 14,
  "miner": "CN",
  "tx_count": 1,
  "fees": "0.00000000",
  "weight": 784,
  "utreexo_root": "ddb9eb30..."
}
```

## 15. Block Detail Endpoint

### `GET /api/v1/block/{hash}`

Response shape:

```json
{
  "height": 3733,
  "hash": "0000...",
  "previous_hash": "0000...",
  "time": "2026-04-17T12:00:00Z",
  "miner": "CN",
  "tx_count": 1,
  "fees": "0.00000000",
  "weight": 784,
  "coinbase_txid": "abc123...",
  "utreexo_root": "ddb9eb30...",
  "transactions": [
    "abc123..."
  ]
}
```

## 16. Alerts Endpoint

### `GET /api/v1/alerts`

Response shape:

```json
{
  "active": [
    {
      "id": "alert-1",
      "severity": "red",
      "title": "Mining paused",
      "message": "Chainstate safe mode active",
      "source": "local",
      "started_at": "2026-04-17T11:59:00Z",
      "related_node": "Mac",
      "action_hint": "Restart after recovery completes"
    }
  ]
}
```

Allowed `severity` values:

- `blue`
- `green`
- `amber`
- `red`

## 17. Event Stream

### `GET /api/v1/events`

Server-Sent Events recommended.

Allowed event types:

- `status.updated`
- `tx.seen`
- `tx.confirmed`
- `tx.rejected`
- `candidate.updated`
- `block.mined`
- `fleet.drift_detected`
- `alert.opened`
- `alert.closed`
- `utreexo.health_changed`

Example SSE payload:

```text
event: tx.seen
data: {"txid":"abc123...","type":"taproot","state":"mempool","age_seconds":0}
```

## 18. Derived Alert Rules

The collector SHOULD derive, at minimum:

- `node_behind_fleet`
- `binary_mismatch`
- `mempool_divergence`
- `mining_paused`
- `chainstate_safe_mode`
- `utreexo_coverage_degraded`
- `activation_approaching`
- `activation_live`
- `template_self_verify_failed`

## 19. Source Mapping

The collector SHOULD reuse existing node RPCs wherever possible.

Likely existing or near-existing sources:

- blockchain info
- peer info
- mempool info
- mining template / mining safety
- activation / privacy status
- wallet summaries
- Utreexo proof or chainstate diagnostics

If a metric cannot be assembled cheaply from existing RPCs, add one collector-facing RPC rather than forcing the UI to stitch together consensus concepts.

## 20. Versioning

This contract SHOULD be versioned at the path:

- `/api/v1/...`

Breaking changes require:

- `/api/v2/...`

Additive fields are allowed if:

- existing fields keep their meaning
- clients can ignore unknown fields safely

## 21. MVP Implementation Order

Recommended backend build order:

1. `/status`
2. `/fleet`
3. `/alerts`
4. `/mempool`
5. `/candidate-block`
6. `/blocks`
7. `/tx/{txid}`
8. `/utreexo`
9. `/events`

This order delivers operator value earliest.

