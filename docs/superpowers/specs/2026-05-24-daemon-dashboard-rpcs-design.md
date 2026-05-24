# Daemon Dashboard RPC Surface — Design

**Date:** 2026-05-24
**Author:** Dinero Labs (with Claude Opus 4.7)
**Status:** Spec (pending implementation plan)
**Target repo:** `DineroLabs/dinero-v8`
**Target branch (impl):** `feature/daemon-dashboard-rpcs` (off `dinero-main`)
**Related:** PR #139 (MyNodeDashboard Phase 1 MVP) — this PR closes the three "—" gaps in that dashboard

---

## Background

The Cmd+K MyNodeDashboard panel landed in PR #139 (`feature/my-node-dashboard-mvp`). During live integration against a real running `dinerod`, three dashboard fields were observed empty (`—`) because the underlying data exists internally in the daemon but has no RPC surface:

| Dashboard field | Internal source | Reachable via JSON-RPC? |
|---|---|---|
| Identity `node_id_hex` | `node_identity_->get_node_id_bytes()` — used in `OrchestrateRelayDials` (`src/daemon/p2p_manager.cpp:1751`) and `RelayHints` send path (`src/daemon/p2p_manager.cpp:2046`) | ❌ No |
| Peers table `ping_ms` column | `peer_connection.cpp` member `m_ping_time_ms` (initialized at line 23); wallet's `slow_reason_analyzer` already reads `stats.avg_ping_ms` (`src/wallet/slow_reason_analyzer.cpp:149`) | ❌ No |
| Peers table `quality_score` column + governor detail | `services/p2p_service.cpp::BuildDynamicP2PQualitySnapshot()` (line 144) constructs per-peer `PeerQualitySnapshot` (`include/p2p/peer_quality.h:22`); `status.dynamic_p2p_governor` struct (lines 387-397) carries hot/warm/relay-capable counts | ❌ No |

This spec covers the small focused daemon-side PR that exposes these three already-computed values via JSON-RPC. **Surface-only — no new tracking logic, no new instrumentation, no consensus-adjacent code.**

## Goal

Three additive RPC changes that let the Cmd+K dashboard fill in its currently empty cells, with zero impact on existing consumers (all changes additive, no field renames/removals).

## Non-goals

- Adding new per-peer instrumentation (ping is already measured; quality is already scored)
- Bumping `rpc_schema` versions (all changes are additive — `v1` consumers ignore new fields, no breaking change)
- Solving the deferred Phase 2/3 needs (sparkline-grade per-second hashrate metrics, etc.)
- Refactoring `getnetworkinfo`'s flat-field shape into nested `identity { ... }` (would be breaking)

## Architecture

Three independent additive changes:

```
┌─────────────────────────────────────────────────────────────────┐
│  Change 1: getnetworkinfo                                       │
│  ─────────────────                                              │
│  Add one top-level field: node_id_hex (40-char hex string)      │
│  Source: existing node_identity_->get_node_id_bytes()           │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  Change 2: getpeerinfo (via BuildPeerInfoJson)                  │
│  ─────────────────                                              │
│  Add two per-peer fields:                                       │
│    ping_ms       (int, 0 if unmeasured)                         │
│    quality_score (int 0-100, defaults 50)                       │
│  Sources: peer_connection.m_ping_time_ms,                       │
│           PeerQuality::Snapshot().score                         │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│  Change 3: NEW RPC dynamic_p2p.observe                          │
│  ─────────────────                                              │
│  Surface the full per-peer PeerQualitySnapshot + governor       │
│  struct that p2p_service.cpp already builds. Mode-aware:        │
│  works even when DPP is OFF (returns empty governor/peers).     │
└─────────────────────────────────────────────────────────────────┘
```

No changes to the data-tracking layer. No changes to the P2P stack. No changes to `node_identity_` or `peer_connection` or `PeerQuality` — only their JSON-serialization callers.

---

## Change 1 — `getnetworkinfo.node_id_hex`

### Wire shape

```json
{
  ...,
  "subversion": "/dinerod:8d4cbe1c/",
  "version": 80000,
  "node_id_hex": "fd4fc04df38bacbf72d4ecae451d1589570bcaba",   // NEW
  ...
}
```

- Lower-case hex, exactly 40 characters (20 bytes × 2)
- Always present (node_identity_ is established before the daemon starts serving RPC)
- No surrounding nested object — flat top-level field, matching the existing pattern of `subversion`, `version`, `protocolversion`

### Source

```cpp
// node_identity_ is constructed in P2PManager and accessible via a getter.
// Implementer pattern:
const auto bytes = node_identity_->get_node_id_bytes();  // std::array<uint8_t, 20>
QString hex_lower = bytesToHexLower(bytes);
result["node_id_hex"] = hex_lower;
```

### Handler location

`src/core/rpc/network_rpc_handlers.cpp` — append one line to whichever function builds the `getnetworkinfo` response object (look near the existing `subversion` / `version` insertions).

### Backward compatibility

Additive. Pre-existing consumers ignore unknown fields. No `rpc_schema` bump.

### Edge cases

- **Daemon starting up, node_id not yet generated.** Should not happen — node_id_ is constructed before P2PManager exposes RPC. If somehow null, emit `""` (empty string) rather than omitting the field, so consumers can rely on key existence.

---

## Change 2 — `getpeerinfo` per-peer `ping_ms` + `quality_score`

### Wire shape (extension to existing per-peer object)

```json
{
  "addr": "1.2.3.4:20999",
  "subver": "/dinerod:.../",
  "synced_blocks": 28100,
  ...,
  "ping_ms": 12,           // NEW: int, milliseconds, 0 if never measured
  "quality_score": 92      // NEW: int 0-100, defaults to 50 if peer not yet observed
}
```

Both fields **always present** on every peer (no conditional omission, no null). When data hasn't been collected yet:
- `ping_ms` = 0 (consumer interprets as "not yet measured")
- `quality_score` = 50 (the PeerQualitySnapshot default — see `peer_quality.h:23`)

The dashboard's render logic in `peerssection.cpp` already treats 0/-1 specially for the unmeasured case; this RPC contract aligns with that.

### Sources

- **ping_ms**: peer_connection's `m_ping_time_ms` (uint32_t). Read directly, cast to int. Already initialized to 0; updated by the existing ping/pong RTT path. No new tracking.
- **quality_score**: `PeerQuality::Snapshot().score` (int, default 50). Each peer object already holds a `PeerQuality` instance (or has one accessible via PeerInfo); fetch the snapshot and emit just the `score` field. Full snapshot detail goes in Change 3's `dynamic_p2p.observe` RPC.

### Handler location

`BuildPeerInfoJson` — the single peer-row builder used by `rpc_getpeerinfo` (`src/core/rpc/network_rpc_handlers.cpp:21`). One edit point covers all consumers of the per-peer shape (getpeerinfo + any other handler that calls BuildPeerInfoJson).

### Why one field per concept instead of grouping

Could nest: `{"latency": {"ping_ms": 12}, "quality": {"score": 92}}`. Rejected for two reasons:
1. Matches the existing peer-row pattern (everything else is flat — `addr`, `bytessent`, `synced_blocks`, no nested groups)
2. Keeps the JSON small for the high-frequency consumer (dashboard polls every 5s × 8+ peers = lots of objects)

The richer per-peer detail belongs in Change 3 (`dynamic_p2p.observe`), explicitly because it's optional + heavier.

### Backward compatibility

Additive. v1 consumers ignore new fields.

---

## Change 3 — New RPC `dynamic_p2p.observe`

### Method signature

- **Method name:** `dynamic_p2p.observe` (dotted namespace, matching existing `mining.status` convention at `src/daemon/rpc_server.cpp:208`)
- **Parameters:** none
- **Result:** JSON object (shape below)

### Wire shape — DPP enabled (active or observe mode)

```json
{
  "rpc_schema": "din.rpc.v1",
  "enabled": true,
  "mode": "active_slow_churn",
  "governor": {
    "available": true,
    "mode": "active_slow_churn",
    "candidate_source": "connected_peers",
    "connected_outbound": 3,
    "configured_seed_hot": 4,
    "relay_capable_seen": 2,
    "hot_peers":                     ["172.93.160.131:20999", "173.249.195.59:20999"],
    "warm_candidates":               ["8.8.8.8:20999"],
    "relay_registration_candidates": [],
    "demote_candidates":             []
  },
  "peers": [
    {
      "addr": "1.2.3.4:20999",
      "quality": {
        "score": 92,
        "connection_successes": 12,
        "connection_failures": 0,
        "handshake_successes": 12,
        "handshake_failures": 0,
        "useful_headers": 247,
        "useful_blocks": 14,
        "stale_height_events": 0,
        "relay_successes": 0,
        "relay_failures": 0,
        "latency_ms": 12,
        "hot_peer_candidate": true,
        "relay_candidate": false
      }
    },
    ...
  ]
}
```

The `quality` sub-object is a 1:1 JSON serialization of the existing `PeerQualitySnapshot` struct (`include/p2p/peer_quality.h:22`). All 13 fields exposed verbatim.

### Wire shape — DPP off mode (config `p2p.dynamic_p2p=off`)

```json
{
  "rpc_schema": "din.rpc.v1",
  "enabled": false,
  "mode": "off",
  "governor": null,
  "peers": []
}
```

Never errors out. Dashboard sees `enabled: false` and can render a "DPP disabled" empty state without trying to parse missing fields.

### Mode classification

| Config value of `p2p.dynamic_p2p` | `enabled` | `mode` |
|---|---|---|
| `"active"` or `"active_slow_churn"` | `true` | `"active_slow_churn"` |
| `"observe"` (or default) | `true` | `"observe"` |
| `"off"` or unset-and-flag-false | `false` | `"off"` |

Match the classification in `P2PService::DynamicP2PMode()` at `src/daemon/services/p2p_service.cpp:435`.

### Source

`P2PService` already builds both halves:
- `BuildDynamicP2PQualitySnapshot(peer)` returns the per-peer struct (`p2p_service.cpp:144`)
- The governor struct (`status.dynamic_p2p_governor`) is built in `BuildStatus()` around `p2p_service.cpp:387-397`

The new RPC handler calls (or replicates) these existing builders and serializes their output. **No new computation.**

### Handler location

New file `src/daemon/rpc_dynamic_p2p_handlers.cpp` registers the `dynamic_p2p.observe` method. Mirrors the existing single-file-per-domain pattern (compare `rpc_mining_implementation.cpp` for `mining.*` methods). Registration in `src/daemon/rpc_server.cpp` alongside `mining.status`.

### Backward compatibility

New method — no existing consumer is touched. Discovery: appears in `help` output once registered.

---

## File map

| File | Status | Responsibility |
|---|---|---|
| `src/core/rpc/network_rpc_handlers.cpp` | Modify | Add `node_id_hex` to `getnetworkinfo` builder. Add `ping_ms` + `quality_score` to `BuildPeerInfoJson` (or its file if separate). |
| `src/daemon/rpc_dynamic_p2p_handlers.cpp` | **Create** | New file. `dynamic_p2p.observe` handler. JSON-serializes the existing PeerQualitySnapshot + governor struct. |
| `src/daemon/rpc_server.cpp` | Modify | Register `dynamic_p2p.observe` in `m_method_handlers` map (near `mining.status` line ~208). |
| `src/daemon/services/p2p_service.cpp` | Modify (minor) | If `BuildStatus()` is the only path that builds the governor struct, extract a small accessor returning just that struct so the RPC handler can use it without rebuilding the full status. Otherwise: no change needed. |
| `include/p2p/peer_quality.h` | No change | PeerQualitySnapshot struct is the wire shape; emit verbatim. |
| `src/core/rpc/network_rpc_handlers_test.cpp` (or equivalent) | Modify or **Create** | Unit-test the new field presence in getnetworkinfo + BuildPeerInfoJson. |
| `tests/integration/test_dashboard_rpcs.sh` | **Create** | 3-RPC smoke against regtest node: assert shapes. |

---

## Data flow

### Cold-start path (daemon launches, no peers yet)

1. `getnetworkinfo` → `node_id_hex` populated immediately (identity is wired at startup before RPC opens)
2. `getpeerinfo` → empty array `[]` (no peers connected)
3. `dynamic_p2p.observe` → returns current `mode` from config + empty `peers: []` + governor reflecting the no-peer state

### Warm path (peers connected, mining running, DPP in observe mode)

1. `getnetworkinfo` returns `node_id_hex` (unchanged from cold)
2. `getpeerinfo` returns N peers each with `ping_ms` (positive int after first ping/pong round-trip) and `quality_score` (50 initially, evolving as events accumulate)
3. `dynamic_p2p.observe` returns the full snapshot — governor counts populated, per-peer detail with all 13 PeerQualitySnapshot fields per row

### Daemon-degraded path (RPC subsystem partially up, e.g., during shutdown)

If `node_identity_` somehow not yet available: `getnetworkinfo.node_id_hex = ""` (empty string, key still present).
If a peer's `PeerQuality` not yet attached: emit `quality_score = 50` (struct default).
If DPP's internal `BuildStatus()` raises: catch, log, return `{enabled: false, mode: "error", governor: null, peers: []}` — same wire shape as off-mode but with `mode: "error"` discriminator.

---

## Error handling

| Situation | Behavior |
|---|---|
| node_identity_ null | getnetworkinfo.node_id_hex = `""` (key present, empty value) |
| peer_connection's m_ping_time_ms is 0 | Emit `ping_ms: 0` directly. Consumer interprets as "unmeasured" — already the dashboard's existing convention. |
| Peer has no PeerQuality instance | Emit `quality_score: 50` (the snapshot struct default) |
| DPP subsystem exception during dynamic_p2p.observe | Catch at handler level, log to debug.log, return `{enabled: false, mode: "error", ...}` so consumers always get parseable JSON |
| Method invoked with extraneous params | Standard JSON-RPC `-32602` invalid params error (existing infrastructure) |

---

## Testing strategy

### Unit tests

`tests/core/rpc/test_network_rpc_handlers_extension.cpp` (NEW) or extend existing test file:

```cpp
TEST(NetworkRpc, getnetworkinfo_includes_node_id_hex) {
    auto result = build_test_getnetworkinfo_response(...);
    EXPECT_TRUE(result.contains("node_id_hex"));
    EXPECT_EQ(result["node_id_hex"].as_string().size(), 40u);
}

TEST(NetworkRpc, getpeerinfo_per_peer_includes_ping_and_quality) {
    PeerInfo p = make_test_peer_with_ping(12);
    auto json = BuildPeerInfoJson(p);
    EXPECT_EQ(json["ping_ms"].as_int(), 12);
    EXPECT_EQ(json["quality_score"].as_int(), 50);  // default
}
```

`tests/daemon/test_dynamic_p2p_observe_handler.cpp` (NEW):

```cpp
TEST(DynamicP2PObserve, off_mode_returns_disabled_shape) {
    auto handler = make_test_handler_with_dpp_mode("off");
    auto result = handler.handle({});
    EXPECT_EQ(result["enabled"].as_bool(), false);
    EXPECT_EQ(result["mode"].as_string(), "off");
    EXPECT_TRUE(result["governor"].is_null());
    EXPECT_EQ(result["peers"].size(), 0u);
}

TEST(DynamicP2PObserve, observe_mode_serializes_peer_qualities) {
    auto handler = make_test_handler_with_dpp_mode("observe");
    add_test_peer_with_quality(handler, "1.2.3.4:20999", make_snapshot(92));
    auto result = handler.handle({});
    EXPECT_EQ(result["enabled"].as_bool(), true);
    ASSERT_EQ(result["peers"].size(), 1u);
    EXPECT_EQ(result["peers"][0]["quality"]["score"].as_int(), 92);
}
```

### Integration smoke

`tests/integration/test_dashboard_rpcs.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

# Spin a regtest node, call each new RPC, assert shape
"$DINEROD" -regtest -daemon -datadir="$TMP" ...
sleep 3

# Change 1
"$DINERO_CLI" getnetworkinfo | jq -e '.node_id_hex | test("^[0-9a-f]{40}$")'

# Change 2 — once at least one peer exists (use addnode + a sibling node)
"$DINERO_CLI" getpeerinfo | jq -e '.[0] | (.ping_ms | type == "number") and (.quality_score | type == "number")'

# Change 3 — off-mode baseline + observe-mode shape
"$DINERO_CLI" dynamic_p2p.observe | jq -e '(.enabled | type == "boolean") and (.mode | type == "string") and (.peers | type == "array")'
```

### Manual canary checklist

After deploy to a real `dinerod`:
- [ ] `dinero-cli getnetworkinfo | jq .node_id_hex` returns a 40-char hex string
- [ ] `dinero-cli getpeerinfo | jq '.[0] | .ping_ms, .quality_score'` returns two numbers
- [ ] `dinero-cli dynamic_p2p.observe | jq .mode` returns a meaningful mode string
- [ ] `dinero-qt`'s Cmd+K dashboard's Q + ping columns populate with actual numbers (after the matching qt-side consumer PR lands — separate dinero-qt follow-up)

---

## Migration & rollout

- **Daemon-only PR.** No dinero-qt change in this PR. Consumer (dashboard) gets a separate small follow-up.
- **Cuts before next rc.** This PR can land any time; the new fields/RPCs become available the moment a daemon built from `dinero-main` is deployed.
- **Old consumers unaffected.** Both extensions are additive. New `dynamic_p2p.observe` method is opt-in.
- **No fleet rollout discipline gate.** Pure RPC-surface additions don't affect consensus, networking, or storage. Standard CI green is sufficient.

---

## Open questions

1. **Should `BuildPeerInfoJson` take a `const PeerQuality&` parameter or look it up from a registry?** If `PeerInfo` already carries a `PeerQuality*` member, easy. If not, the handler may need to consult a separate map. Investigate in Step 1 of the implementation plan.

2. **Should `mining.status` similarly gain a Stratum-aware path?** Out of scope for this PR but worth noting: the qt-app side has Stratum mining state the daemon doesn't see (covered in dinero-qt PR #139's commit a3bf5c2c). A future "stratum.status" RPC could consolidate.

3. **Is `dynamic_p2p.observe` the right method name, or should it match the config key `p2p.dynamic_p2p` better (e.g., `p2p.dynamic_p2p.status`)?** Decision: `dynamic_p2p.observe` mirrors the *mode* terminology used in code (`IsDynamicP2PActiveMode`, `DynamicP2PMode()`) rather than the config key. Defer to reviewer if a different convention is preferred — trivial rename either way.

---

## Related artifacts

- Existing code referenced: `node_identity_->get_node_id_bytes()` (used at `src/daemon/p2p_manager.cpp:1751`, `:2046`), `peer_connection.cpp:23` (m_ping_time_ms init), `services/p2p_service.cpp:144` (BuildDynamicP2PQualitySnapshot), `include/p2p/peer_quality.h:22` (PeerQualitySnapshot struct)
- Convention models: `mining.status` registered at `src/daemon/rpc_server.cpp:208`, all RPCs emit `rpc_schema: "din.rpc.v1"` per `network_rpc_handlers.cpp:65` and peers
- Consumer of this work: dinero-qt PR #139 (`feature/my-node-dashboard-mvp`), specifically `qt/src/nodepoller.cpp::parseNetworkInfo` and `parsePeers`, plus a future Phase 1.5 follow-up that wires `dynamic_p2p.observe` polling
