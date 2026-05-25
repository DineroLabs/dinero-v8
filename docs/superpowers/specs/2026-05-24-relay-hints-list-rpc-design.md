# Relay Hints List + 24h Relay Counters RPC Design (Phase 2b daemon)

**Date:** 2026-05-24
**Status:** Approved
**Consumed by:** [MyNodeDashboard Phase 2b qt PR](./2026-05-24-my-node-dashboard-design.md) — DiscoverySection, Decentralization-score real inputs

## Goal

Expose three pieces of relay-cache + relay-traffic state to the JSON-RPC surface so the MyNodeDashboard Cmd+K panel can render its DiscoverySection (per-target hint rows) and stop using the four placeholder approximations Phase 2a documented in code (`bytes_relayed_24h` extrapolation, `blocks_served_today = 0`, `circuits_active = 0`, gossip-reach proxy).

## Scope

In:
1. **New RPC `relay_hints.list`** — returns the contents of `P2pManager::relay_hints_by_target_` as a JSON array, one entry per target node id with up to four endpoint records each.
2. **New field `getnetworkinfo.relay.blocks_served_24h`** — rolling 24-hour count of `BlockRelayManager::HandleGetData` block sends (the daemon serving a full block to a peer in response to inv/getdata).
3. **New field `getnetworkinfo.relay.bytes_relayed_24h`** — rolling 24-hour total of bytes carried over relay-virtual connections (sent + received via the relay-virtual `send_relay_data_to_virtual_peer` path).

Out (deferred to later phases or kept as the existing Phase 2a proxies):
- Per-source-peer tracking inside `RelayHintRecord` (would require RELAY_HINTS protocol/wire change to carry the source). Gossip-reach in the Decentralization score stays on the `hints_received_relay` proxy from Phase 2a.
- "Circuits active" as a distinct concept — dinero-v8 has no Tor-style circuit primitive. The Qt UI relabels this row as "Registrants active" and reads the already-present `getnetworkinfo.relay.registrants_count`.
- `relayhints.dial` action RPC — Phase 3 ("Topology + Actions" per the dashboard design).
- Persistence of the 24h counters across daemon restart — the buckets are in-memory only; restart resets them to 0. Acceptable because the dashboard is observational and the rates re-converge within an hour.

## RPC shape

### `relay_hints.list`

**Method:** `relay_hints.list` (dot-namespaced, matches the existing `dynamic_p2p.observe` convention from PR #140 and the qt-side `relay.hints` field group in `getnetworkinfo`).

**Params:** none.

**Result:**

```jsonc
{
  "targets": [
    {
      "target_node_id_hex": "20-byte target node id, 40 hex chars",
      "endpoints": [
        {
          "net": "ipv4",                     // or "ipv6"
          "addr": "203.0.113.7",             // dotted-quad / canonical v6
          "port": 20999,
          "age_seconds": 47,                 // monotonic time since learned_at
          "dial_failures": 0,                // consecutive_dial_failures
          "near_eviction": false             // age_seconds >= 0.8 * kHintTtl OR dial_failures >= kHintMaxFailures - 1
        }
        // up to kMaxHintsPerTarget (4) endpoints per target
      ]
    }
    // ... one entry per key in relay_hints_by_target_
  ],
  "total_targets": 12,
  "ttl_seconds": 900,                        // kHintTtl as seconds (currently 15 min)
  "max_failures": 3                          // kHintMaxFailures
}
```

**Error semantics:**
- Always returns 200 OK. Empty cache → `{"targets": [], "total_targets": 0, ...}`.
- No filter params in v1. (Filter-by-near-eviction etc. can come later if dashboard needs it.)

**Lock discipline:** acquires `relay_hints_mutex_` (existing), snapshots the map into the JSON builder, releases. The JSON build itself happens with the lock dropped to avoid holding `relay_hints_mutex_` during `Json::Value` construction.

**Why a new top-level method instead of extending `getnetworkinfo`:** `getnetworkinfo` is hot-path on the existing dashboard tick (every 5 seconds × 4-section consumer). The per-target hint array can be O(unique_relay_targets) which grows with the network. Keeping it on its own method lets the DiscoverySection poll independently (still 5s per spec) without inflating every `getnetworkinfo` response.

### `getnetworkinfo.relay.blocks_served_24h`

**Existing `getnetworkinfo.relay` block** gains one new uint64 field:

```jsonc
{
  "relay": {
    "active": true,
    "registrants_count": 3,
    "hints": { ... },
    "blocks_served_24h": 142,    // NEW
    "bytes_relayed_24h": 7891234 // NEW (see below)
  }
}
```

**Source:** `BlockRelayManager::CountBlockSent()` — incremented from inside `HandleGetData` after a successful block send. Bucket: rolling 24 × 1-hour windows. On each increment we drop any bucket older than 24h.

**Implementation note:** Use `std::array<std::atomic<uint64_t>, 24>` indexed by `hour_bucket = (steady_clock_now.epoch_seconds() / 3600) % 24`, plus a `last_increment_hour_` member that lets us zero-out wraparound buckets on first touch each hour. Single `std::mutex` for the bucket rotation; atomic increments inside the current bucket are lock-free.

### `getnetworkinfo.relay.bytes_relayed_24h`

**Source:** `P2pManager::send_relay_data_to_virtual_peer` and the inbound relay-virtual decode path. Both call a new `RecordRelayBytes(uint64_t bytes)` on a 24-hour rolling counter, identical bucket pattern to `blocks_served_24h`.

**Counts:** sum of `bytes_sent + bytes_received` over relay-virtual peers in the last 24h. Direct (non-relay) peer traffic is NOT counted.

## Backward compatibility

- New RPC method (`relay_hints.list`) — additive. Older clients that don't call it are unaffected.
- New fields on `getnetworkinfo.relay` — additive. JSON consumers that ignore unknown fields (the qt dashboard via Qt's JSON parser) keep working unchanged.
- No existing wire formats change.
- Phase 2a's `received_self` / `received_relay` counters under `getnetworkinfo.relay.hints` remain — Phase 2b additions sit alongside them.

## Threading + race safety

- `relay_hints_mutex_` is the existing guard for `relay_hints_by_target_`; new RPC takes shared snapshot under that lock.
- 24h bucket counters use atomic increments inside the current bucket; a mutex protects rotation logic (called at most once per minute via existing keepalive_loop or on first increment in a new hour). No new lock-ordering concern since the bucket counters don't call back into P2pManager state.

## Test plan

Add `tests/network/test_relay_hints_list_rpc.sh` (shell, in the same style as Phase 1.5's `test_dashboard_rpcs.sh`):
1. Spin up regtest A (relay) + B (target) + C (origin); A advertises B's relay endpoint via RELAY_REGISTER; C learns about A through RELAY_HINTS.
2. C's `relay_hints.list` returns one entry with `target_node_id_hex == B.node_id`, one endpoint with `dial_failures == 0`, `age_seconds < 10`, `near_eviction == false`.
3. C's `getnetworkinfo.relay.blocks_served_24h` increments after A serves a regtest-mined block to a downstream peer.
4. `bytes_relayed_24h` is > 0 once relay-virtual traffic flows.

Unit-test the 24h bucket rotation (C++ gtest, deterministic via FakeClockSource): bucket at hour=0 increments to 5, advance clock 25h, bucket at hour=25 reports 0 (the old hour=0 was rotated out).

## Risk + mitigation

| Risk | Mitigation |
|---|---|
| Bucket rotation race under heavy multi-thread increment | Mutex around rotation; atomics within a bucket. Worst case is a single increment landing in the wrong bucket — acceptable for an observational counter. |
| `relay_hints.list` returns a huge result on a hub node with thousands of targets | None for v1. If the dashboard polls become slow we'll add pagination. Currently relay_hints_by_target_ is bounded by network-wide target_node_id count, typically < 100 on canary. |
| Forgetting to call `RecordRelayBytes` on a new code path | Wrap the existing relay-virtual send/receive funnels (only 2 call sites). Add a test that asserts the counter advanced after a relay-virtual ping round-trip. |

## Files (estimated)

- `include/rpc/rpc_relay_hints_handlers.h` (new, ~25 lines)
- `src/rpc/rpc_relay_hints_handlers.cpp` (new, ~120 lines)
- `src/daemon/rpc_context_wiring.cpp` (modify, ~5 lines to register)
- `src/daemon/p2p_manager.{h,cpp}` (modify, ~40 lines for the 24h bucket counter + RecordRelayBytes hook)
- `src/daemon/block_relay_manager.{h,cpp}` (modify, ~25 lines for blocks_served_24h bucket + CountBlockSent hook)
- `src/rpc/methods_network_context.cpp` (modify, ~10 lines to surface 2 new fields on getnetworkinfo.relay)
- `tests/network/test_relay_hints_list_rpc.sh` (new, ~100 lines)
- `tests/network/test_rolling_24h_bucket.cpp` (new gtest, ~80 lines)
- `tests/CMakeLists.txt` (modify, ~5 lines)

Total estimate: ~400-450 LOC. Comparable to PR #140 (~370 LOC).
