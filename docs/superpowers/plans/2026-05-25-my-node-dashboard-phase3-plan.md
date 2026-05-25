# MyNodeDashboard Phase 3 — Implementation Plan

**Spec:** `docs/superpowers/specs/2026-05-25-my-node-dashboard-phase3-topology-actions-design.md`
**Branch family:** `feature/dashboard-phase3-*`
**Base:** `dinero-main` after PR #146
**Rule:** split into three implementation PRs. Do not combine daemon action RPC, topology rendering, and destructive peer actions in one PR.

## Sequence

1. PR A: daemon `relayhints.dial`
2. PR B: Qt topology view
3. PR C: peer context actions

This order keeps the action backend available before Qt exposes a button, and keeps the passive topology visible before destructive controls appear.

## PR A — daemon `relayhints.dial`

### Goal

Expose one safe manual relay-hint dial RPC that can only use hints already present in the daemon cache.

### Files

- Modify: `src/daemon/p2p_manager.h`
- Modify: `src/daemon/p2p_manager.cpp`
- Create or extend: `include/rpc/rpc_relay_hints_handlers.h`
- Modify: `src/rpc/rpc_relay_hints_handlers.cpp`
- Modify RPC registration file that currently registers `relay_hints.list`
- Add focused tests in the nearest existing network/RPC test area

### Steps

1. Add value-type result:

```cpp
struct ManualRelayDialResult {
    enum class Status {
        Submitted,
        DryRunOk,
        AlreadyConnected,
        NoHint,
        RelayNotConnected,
    };
    Status status{Status::NoHint};
    std::string target_node_id_hex;
    std::string relay_endpoint;
    uint64_t request_id{0};
};
```

2. Add method on `P2PManager`:

```cpp
ManualRelayDialResult TryDialRelayHint(
    const std::string& target_node_id_hex,
    const std::optional<std::string>& relay_endpoint,
    bool dry_run);
```

3. Route-selection discipline:

- validate target hex length and decode before lookup
- snapshot `relay_hints_by_target_` under `relay_hints_mutex_`
- if `relay_endpoint` supplied, match exactly against one cached endpoint for that target
- if omitted, pick freshest non-near-eviction hint with lowest failure count
- check already-connected virtual/direct peer state before submitting
- verify selected relay endpoint is currently connected
- call `SendRelayConnect` with locks dropped

4. Add RPC handler:

Method: `relayhints.dial`

Params:

```json
{
  "target_node_id_hex": "...",
  "relay_endpoint": "optional host:port",
  "dry_run": false
}
```

5. Result shape:

```json
{
  "rpc_schema": "din.rpc.v1",
  "target_node_id_hex": "...",
  "relay_endpoint": "...",
  "submitted": true,
  "request_id": 123,
  "status": "submitted"
}
```

6. Tests:

- malformed params return JSON-RPC error
- unknown target returns `no_hint`
- dry-run on known target returns `dry_run_ok`
- connected target returns `already_connected`
- disconnected relay returns `relay_not_connected`
- successful route calls into the SendRelayConnect path or a test seam

7. Validation:

```bash
cmake --build build-default --target dinerod
ctest --test-dir build-default -R 'RelayHints|relayhints|RPC' --output-on-failure
```

## PR B — Qt topology view

### Goal

Add a passive topology section to Cmd+K using existing data already polled by `NodePoller`.

### Files

- Modify: `qt/src/dashboardtypes.h`
- Modify: `qt/src/nodepoller.h`
- Modify: `qt/src/nodepoller.cpp`
- Create: `qt/src/topologysection.h`
- Create: `qt/src/topologysection.cpp`
- Modify: `qt/src/mynodedashboard.h`
- Modify: `qt/src/mynodedashboard.cpp`
- Modify: `qt/CMakeLists.txt`
- Add Qt tests for topology view-model construction

### Value types

Add:

```cpp
struct TopologyNode {
    QString id;
    QString label;
    QString endpoint;
    QString kind;       // self/direct/relay_virtual/hint/fleet
    QString bucket;     // hot/warm/demote/relay_candidate/empty
    int quality_score{-1};
    bool connected{false};
};

struct TopologyEdge {
    QString from_id;
    QString to_id;
    QString kind;       // direct/relay_virtual/hint
    QString via_relay;
};

struct TopologySnapshot {
    QVector<TopologyNode> nodes;
    QVector<TopologyEdge> edges;
};
```

### NodePoller

1. Keep latest copies of:
   - `NodeIdentity`
   - `QVector<PeerRow>`
   - `QVector<HintRow>`
   - `DynamicP2POverview`
2. Emit:

```cpp
void topologyUpdated(const TopologySnapshot& snapshot);
```

3. Build snapshot after either peers or hints update.

### TopologySection rendering

Use `QGraphicsView` / `QGraphicsScene`:

- center self node
- direct connected peers in ring 1
- relay virtual peers grouped by relay endpoint
- hint-only targets in ring 2
- deterministic positions based on sorted ids
- no physics animation

### Tests

- snapshot includes self + connected peers
- relay virtual peer creates relay edge
- hint-only target appears but is disconnected
- fleet endpoint receives fleet label

### Validation

```bash
cmake --build build-qt --target dinero-qt
ctest --test-dir build-qt -R 'Topology|Dashboard' --output-on-failure
```

## PR C — peer context actions

### Goal

Add explicit peer actions with confirmation gates. No action should silently change networking state.

### Files

- Modify: `qt/src/peerssection.h`
- Modify: `qt/src/peerssection.cpp`
- Modify: `qt/src/discoverysection.h`
- Modify: `qt/src/discoverysection.cpp`
- Modify: `qt/src/topologysection.h`
- Modify: `qt/src/topologysection.cpp`
- Create: `qt/src/dashboardactioncontroller.h`
- Create: `qt/src/dashboardactioncontroller.cpp`
- Modify: `qt/src/mynodedashboard.h`
- Modify: `qt/src/mynodedashboard.cpp`
- Add Qt tests for menu availability and guardrails

### DashboardActionController

Centralize RPC calls so sections stay mostly UI-only:

```cpp
class DashboardActionController : public QObject {
    Q_OBJECT
public:
    explicit DashboardActionController(RpcClient* rpc, QWidget* parent_widget, QObject* parent = nullptr);

public Q_SLOTS:
    void copyEndpoint(const QString& endpoint);
    void copyPeerDetails(const PeerRow& peer);
    void disconnectPeer(const QString& peer_addr);
    void banPeer(const QString& endpoint, int seconds);
    void tryDirectReconnect(const QString& endpoint);
    void dialRelayHint(const HintRow& hint);
};
```

### Confirmation rules

- `disconnectPeer`: confirmation dialog
- `banPeer`: confirmation dialog with duration and endpoint
- `banPeer` disabled for:
  - `relay:` virtual endpoints
  - hostnames
  - empty endpoint
- fleet endpoints require stronger warning text
- `dialRelayHint`: confirmation not required, but result must be surfaced as "submitted", "no hint", "relay not connected", or error

### RPC mapping

| Controller method | RPC |
|---|---|
| `tryDirectReconnect` | `addnode [endpoint, "onetry"]` |
| `disconnectPeer` | `disconnectnode [peer_addr]` |
| `banPeer` | `setban [ip, "add", seconds, false]` |
| `dialRelayHint` | `relayhints.dial {"target_node_id_hex": ..., "relay_endpoint": ...}` |

### Tests

- virtual relay peer does not expose ban action
- direct IP peer exposes disconnect + ban
- hint row exposes dial action
- destructive actions require confirmation seam
- controller sends expected RPC params for non-destructive actions

### Validation

```bash
cmake --build build-qt --target dinero-qt
ctest --test-dir build-qt -R 'PeerActions|Topology|Discovery|Dashboard' --output-on-failure
```

## Final smoke after PR C

Run a live daemon with at least four peers, then:

1. Open Cmd+K.
2. Verify the dashboard has six sections.
3. Verify topology paints self, direct peers, and hint-only nodes.
4. Right-click a direct peer:
   - copy endpoint works
   - disconnect asks confirmation
   - ban asks stronger confirmation
5. Right-click a relay hint:
   - dial action sends `relayhints.dial`
   - result text appears without freezing Qt
6. Confirm dashboard polling continues after a failed action.

## Merge discipline

- PR A may merge alone.
- PR B may merge after PR A, even before PR C.
- PR C must not merge before PR A.
- Do not include release packaging in any Phase 3 PR. Release cut is separate.
