# MyNodeDashboard Phase 3 — Topology, Peer Actions, and Manual Relay Dial

**Date:** 2026-05-25
**Status:** Draft spec
**Target repo:** `DineroLabs/dinero-v8`
**Depends on:** PR #143, #144, #146
**Related specs:** `2026-05-24-daemon-dashboard-rpcs-design.md`, `2026-05-24-relay-hints-list-rpc-design.md`, `2026-05-24-dynamic-p2p-v1-design.md`

## Why this phase exists

Phase 1 and Phase 2 turned Cmd+K MyNodeDashboard into a real observability surface:

- identity, reachability, mining, relay role, and Dynamic P2P state
- connected peers with ping/quality
- contribution metrics and decentralization score
- relay-hint cache visibility through `relay_hints.list`

Phase 3 is the first phase where the dashboard becomes interactive. That changes the risk profile. Passive displays can be forgiving; peer controls can disconnect useful peers, ban a good relay, or create confusing manual relay attempts. This phase must therefore separate **observe**, **inspect**, and **act**.

The goal is not to make the wallet a node-operator cockpit. The goal is to let normal users see the shape of their P2P participation, and let advanced users take a small set of intentional actions without hiding what those actions do.

## User-facing goals

1. Show a topology view that answers:
   - Which peers am I connected to directly?
   - Which peers are relay-backed virtual peers?
   - Which peers are fleet/bootstrap nodes versus discovered/community peers?
   - Which relay hints are available but not currently connected?
   - Which peers look hot/warm/weak from Dynamic P2P's perspective?
2. Add right-click peer actions:
   - copy endpoint
   - copy peer details
   - disconnect peer
   - ban peer for a bounded duration
   - try reconnect / addnode onetry
3. Add manual relay-hint dial:
   - user can choose a known target hint and ask the daemon to try a relay path now
   - the daemon uses its existing cached hints and existing relay machinery
   - Qt shows submitted / failed / already-connected status clearly

## Non-goals

- No third-party RELAY_HINTS gossip implementation. That belongs to the relay-hints lifecycle spec Phase 1c.
- No on-chain relay registration.
- No arbitrary "dial any relay to any target" RPC. Manual dial must use hints already learned and validated by the daemon.
- No new ban database UI. The existing `setban` / `listbanned` RPC surface is enough for this phase.
- No automatic peer-governor policy changes. Phase 3 observes and manually nudges; it does not alter Dynamic P2P churn rules.
- No force-dial loops from Qt. A click creates one daemon-side attempt; retries stay daemon-owned.

## Existing surfaces Phase 3 should reuse

| Need | Existing surface |
|---|---|
| Connected peers | `getpeerinfo` |
| Peer quality / Dynamic P2P buckets | `dynamic_p2p.observe` |
| Relay-hint cache | `relay_hints.list` |
| Direct one-shot connect | `addnode "<host:port>" "onetry"` |
| Disconnect connected peer | `disconnectnode "<peer addr>"` |
| Ban / unban | `setban "<ip>" "add|remove" [bantime] [absolute]` |
| Node identity / relay role / counters | `getnetworkinfo` |

The only required daemon addition for Phase 3 is `relayhints.dial`.

## Topology model

Qt should build a local view model from existing poller data. The daemon does not need a topology RPC in this phase.

### Nodes

| Node type | Source | UI treatment |
|---|---|---|
| `self` | `getnetworkinfo.node_id_hex` | Center node |
| `direct_peer` | `getpeerinfo` entries where `via_relay == false` | Solid edge to self |
| `relay_virtual_peer` | `getpeerinfo` entries where `via_relay == true` or address starts with `relay:` | Dashed edge through relay |
| `relay_hint_target` | `relay_hints.list.targets` not currently connected | Faint ghost node |
| `fleet_peer` | existing fleet IP map in `NodePoller` / `PeersSection` | Small label: LA / VA / MO / CN |
| `dynamic_bucket` | `dynamic_p2p.observe.governor` | Badges: hot / warm / demote / relay candidate |

### Edges

| Edge type | Meaning |
|---|---|
| solid green/blue | direct connected peer |
| dashed purple | relay-backed virtual peer |
| dotted gray | known relay hint, not currently connected |
| thin orange | weak/demote candidate |

The topology is intentionally approximate. It is a local node's view, not a global network map.

## Qt layout

Add a sixth MyNodeDashboard section:

```
Identity
Network
Peers
Contribution
Discovery
Topology
```

`TopologySection` can use `QGraphicsView` / `QGraphicsScene` with a deterministic radial layout:

- self in the center
- direct peers around the first ring
- relay virtual peers grouped near their relay
- hint-only targets in an outer faint ring
- no physics simulation in v1

This keeps tests deterministic and avoids adding rendering libraries.

## Peer actions

### Context menu actions

Right-click on a peer row or topology node:

| Action | Applies to | RPC |
|---|---|---|
| Copy endpoint | all peer/hint nodes | none |
| Copy details JSON | all peer/hint nodes | none |
| Try direct reconnect | direct peer or hint endpoint with IP/port | `addnode [endpoint, "onetry"]` |
| Disconnect | connected direct or relay virtual peer | `disconnectnode [addr]` |
| Ban 1 hour | direct IP peers only | `setban [ip, "add", 3600, false]` |
| Ban 24 hours | direct IP peers only | `setban [ip, "add", 86400, false]` |
| Dial via relay hint | hint target row | `relayhints.dial` |

### Safety rules

- Destructive actions require confirmation:
  - disconnect: simple confirm
  - ban: confirm with duration and endpoint
- Ban is disabled for:
  - `relay:` virtual addresses
  - hostnames
  - peers without a concrete IP
- Fleet/bootstrap peers should show an extra warning before ban:
  - "This is one of your configured bootstrap peers. Ban only if you are debugging."
- The UI must show "request submitted" rather than "connected" for async actions.

## New daemon RPC: `relayhints.dial`

### Purpose

Allow the dashboard to ask the daemon to try a relay-backed connection to a known target now, instead of waiting for the normal orchestrator cadence.

### Method

`relayhints.dial`

### Parameters

Object form only:

```json
{
  "target_node_id_hex": "40-char target id",
  "relay_endpoint": "optional host:port from relay_hints.list",
  "dry_run": false
}
```

Rules:

- `target_node_id_hex` is required.
- `relay_endpoint` is optional. If omitted, daemon selects the freshest eligible hint for that target using the same preference as the orchestrator.
- If provided, `relay_endpoint` must match one of the current cached hints for that target. The daemon must not use arbitrary user-supplied relays.
- `dry_run=true` validates and selects the route but does not send `RELAY_CONNECT`.

### Result

Submitted:

```json
{
  "rpc_schema": "din.rpc.v1",
  "target_node_id_hex": "...",
  "relay_endpoint": "172.93.160.131:20999",
  "submitted": true,
  "request_id": 123456,
  "status": "submitted"
}
```

Already connected:

```json
{
  "rpc_schema": "din.rpc.v1",
  "target_node_id_hex": "...",
  "submitted": false,
  "request_id": 0,
  "status": "already_connected"
}
```

No route:

```json
{
  "rpc_schema": "din.rpc.v1",
  "target_node_id_hex": "...",
  "submitted": false,
  "request_id": 0,
  "status": "no_hint"
}
```

### Error semantics

JSON-RPC error only for malformed input:

- invalid params type
- invalid node id hex
- invalid relay endpoint string

Operational outcomes are normal results with `status`:

- `no_hint`
- `relay_not_connected`
- `already_connected`
- `submitted`
- `dry_run_ok`

### Daemon implementation shape

Add a small P2PManager method:

```cpp
struct ManualRelayDialResult {
    enum class Status {
        Submitted,
        DryRunOk,
        AlreadyConnected,
        NoHint,
        RelayNotConnected,
    };
    Status status;
    std::string target_node_id_hex;
    std::string relay_endpoint;
    uint64_t request_id{0};
};

ManualRelayDialResult TryDialRelayHint(
    const std::string& target_node_id_hex,
    std::optional<std::string> relay_endpoint,
    bool dry_run);
```

This method should reuse the route-selection and duplicate-connection guards from `OrchestrateRelayDials`, but it should not run the whole orchestrator pass.

Locking discipline:

1. Snapshot `relay_hints_by_target_` under `relay_hints_mutex_`.
2. Check current connected peers under `peers_mutex_`.
3. Choose relay endpoint without holding either lock.
4. Call `SendRelayConnect` after locks are dropped.

Do not call into Qt-facing RPC while holding P2P locks.

## Qt implementation phases

### PR A — daemon `relayhints.dial`

- add P2PManager `TryDialRelayHint`
- add RPC handler
- add unit tests for route selection / no route / already connected / dry-run
- add one regtest smoke if practical; otherwise focused C++ tests are acceptable

### PR B — topology view

- add `TopologyNode` / `TopologyEdge` value types
- extend NodePoller to synthesize a topology snapshot from already-polled data
- add `TopologySection` using deterministic `QGraphicsScene`
- add parser/view-model tests
- no peer actions yet

### PR C — peer actions

- add context menus to `PeersSection`, `DiscoverySection`, and `TopologySection`
- wire actions through a small `DashboardActionController`
- use existing RPCs for addnode / disconnectnode / setban
- use `relayhints.dial` for manual relay hint dial
- add Qt tests for menu availability and confirmation gates

## Validation

Minimum per PR:

```bash
cmake --build build-qt --target dinero-qt
ctest --test-dir build-qt -R 'Dashboard|RelayHints|Topology|PeerActions' --output-on-failure
```

Manual smoke:

1. Start a daemon with at least four peers.
2. Open Cmd+K.
3. Confirm Topology renders:
   - self node
   - direct peers
   - relay virtual peer if present
   - hint-only nodes if `relay_hints.list` is non-empty
4. Right-click direct peer:
   - copy works
   - disconnect asks confirmation
   - ban asks stronger confirmation
5. Right-click hint-only node:
   - manual relay dial appears
   - action returns submitted / no_hint visibly
6. Confirm dashboard keeps polling after failed action.

## Release notes wording

MyNodeDashboard Phase 3 adds an interactive local topology view and advanced peer actions. Users can inspect how their node connects to the network, distinguish direct and relay-backed paths, and manually test a known relay hint. Destructive peer actions require confirmation.
