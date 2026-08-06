# Fleet Watcher (Sub-project B) — Design

**Status:** proposed
**Scope:** external fleet observability and alerting. Sub-project A (node-side reorg event
feed) is a separate spec and a separate PR. Neither blocks the other.

Operational topology, host roles, addresses, and endpoint inventory are deliberately **not**
recorded here. This document is public; deployment specifics belong in the private operations
runbook.

## Goal

Detect chain-integrity problems across a node fleet as they happen, from outside the daemon,
using RPCs that work today. Persist every observation so thresholds can be tuned from real data
rather than guessed. Page only for conditions that are genuinely dangerous.

## Why B before A

A watcher built on existing RPCs ships immediately and keeps working if the node-side change is
delayed. That ordering is deliberate: three separate node-side subsystems in this repository
were written, looked finished, compiled, and never reached a running binary
(`getchaintips`/`getchainwork`/`getreorgstatus`, `telemetry.getmetrics`/`server.health`, and the
`ProductionMetrics`/`AlertThresholds` prototype deleted in #528). A design that depends on
node-side work landing first inherits that risk. A design that does not, does not.

Running B against a real fleet also produces the evidence for A: which rules fire, how often,
and which fields turn out to be missing.

## Non-goals

- Not a metrics/monitoring platform. No Prometheus exporter, no dashboards, no time-series
  database.
- Not a replacement for interactive fleet status tooling. That answers "what is the fleet doing
  now"; this answers "what has it been doing, and has anything gone wrong."
- Does not modify the daemon. No C++ changes belong in this sub-project.
- Does not revive any part of the deleted `ProductionMetrics`/`AlertThresholds` prototype.
- Does not couple telemetry to consensus persistence.
- Does not change any node's existing RPC exposure or authentication posture.

## Deployment

One watcher process, running as a systemd service on a **watcher host** chosen for operational
separation from the fleet's primary service host.

The watcher polls each remote node over SSH to that node's **loopback** RPC, adding no new
network exposure. When the watcher host is itself a fleet node, it polls its own loopback RPC
directly rather than over SSH.

### Polling and access contract

- A **dedicated unprivileged account** on each polled node. Not root, not a shared operator
  account.
- Access via a **forced-command read-only wrapper**, not a general shell. The wrapper exposes
  only the specific read RPCs this design needs.
- **No port forwarding, no agent forwarding, no X11 forwarding**, no PTY.
- **Pinned SSH host keys.** A host-key change fails the poll rather than prompting or trusting.
- **Strict connect and read timeouts** on every poll, so one hung node cannot stall the cycle.

## Node roles

Nodes fall into two tiers. The distinction is a design decision, not an implementation detail:

- **Voting nodes** — continuously available server nodes. Only these participate in quorum and
  consensus rules.
- **Observer nodes** — nodes whose availability is not guaranteed (for example an operator
  workstation, which may sleep or sit behind NAT). Observers are polled and fully recorded, and
  may raise their own `safe_mode` and `node_behind` incidents, but are **never counted toward
  quorum**.

Excluding intermittent hosts from quorum keeps ordinary laptop availability out of
chain-integrity decisions. A sleeping workstation is not a consensus event.

The current deployment has **3 voting nodes and 1 observer**. Which hosts these are is
configuration, not part of this document.

## Components

Four units with clear boundaries, each independently testable.

### 1. Poller

Given a node list, returns one `Observation` per node per cycle. Pure I/O; contains no rules.

| field | meaning |
|---|---|
| `cycle_id` | groups all observations from one cycle |
| `timestamp` | watcher clock, UTC |
| `node`, `role` | fleet identifier; voting or observer |
| `height`, `tip_hash` | chain position |
| `peers_in`, `peers_out` | connectivity |
| `synced` | daemon's own sync flag |
| `safe_mode` | `active` / `inactive` / **`unknown`** |
| `safe_mode_reason` | when active |
| `reachable` | see below |
| `restart_id` | see below |

**`reachable` means the daemon answered a core RPC** — chain height and tip hash. It does *not*
mean every optional field succeeded. A node serving height but not `safemode.status` is
reachable with degraded telemetry; those are different conditions with different responses.

**`safe_mode` is tri-state.** A daemon too old to register `safemode.status` answers -32601.
That is `unknown`, never `inactive`. **Unknown must not behave like safe.** Sustained unknown
opens a `telemetry_degraded` incident after the confirmation threshold — logged, not paged.

**`restart_id`** distinguishes a restarted node from a stalled one, read from **systemd
(`InvocationID`) or `/proc/<pid>/stat` start ticks** — never from locale-formatted
`ps -o lstart=` output, which varies by locale and is unsafe to parse. If it cannot be
determined it is null and the restart rule does not evaluate for that node. A missing signal
must never synthesise a false one.

### 2. Store

SQLite. Three tables:

- `observations` — one row per node per cycle, carrying `cycle_id`.
- `incidents` — one row per rule transition: `incident_id`, rule, node(s), `opened_at`,
  `closed_at`, severity, detail.
- `outbox` — durable pending notifications.

**All observations for a cycle are written in a single transaction, keyed by `cycle_id`.** Rules
evaluate only complete cycles. A partially written cycle is never visible to rule evaluation —
otherwise a crash mid-cycle could present a fleet that appears to have lost quorum.

**The outbox is what makes alerting crash-safe.** Opening an incident and enqueuing its
notification happen in the **same transaction**. A separate delivery worker sends from the outbox
and records completion afterwards.

Without the outbox there is a silent failure window: the watcher opens an incident, crashes
before contacting the notification provider, and on restart deduplication sees an already-open
incident and never notifies. The alert would be permanently lost precisely because the system
believed it had already been sent.

### 3. Rules

Pure functions over complete cycles. No I/O. This is where false-positive discipline lives and
is the part most worth testing.

#### Fleet quorum

Named **fleet quorum**, deliberately not "healthy majority": mutual agreement between nodes does
not independently prove the chain is correct. A quorum can be wrong together.

Computed over **voting nodes only**:

1. Choose a **common height** at which reachable voting nodes can be compared — the highest
   height for which enough of them have reported a hash.
2. Group those nodes by their block hash **at that common height**, not by their current tip.
3. A **fleet quorum** is a unique mutually agreeing group of at least **2 of the 3 voting
   nodes**.
4. The quorum's **median tip height** is the reference height for lag rules.
5. If **two equally sized competing groups** exist, there is no unique quorum: that is
   `consensus_health` failure.

Comparing at a common height is the crux. Comparing current tips would page whenever healthy
nodes are momentarily one block apart, which is ordinary propagation, not divergence.

#### Rules

| rule | condition |
|---|---|
| `safe_mode` | any node (voting or observer) reports safe mode active |
| `consensus_health` | no unique fleet quorum exists among voting nodes |
| `tip_divergence` | reachable voting nodes report different hashes at the common height |
| `majority_unreachable` | 2 or more of the 3 voting nodes unreachable |
| `node_behind` | a reachable node is ≥ 10 blocks below the quorum's median tip height |
| `telemetry_degraded` | sustained `unknown` safe-mode, or missing optional fields |

`majority_unreachable` and `consensus_health` can both hold at once, since losing voting nodes
destroys any quorum. In that case only `majority_unreachable` is reported: it names the cause
rather than the symptom.

**Logged, never paged:** short lag, a single missed poll, peer-count changes, shallow expected
reorgs, and `telemetry_degraded`.

### 4. Notifier

Abstract interface with one concrete implementation (Pushover), so another provider can be added
without touching the rules. Delivery is driven entirely from the outbox.

## Alerting policy

**Log everything; push only critical events.** Every observation and every rule transition is
persisted regardless of severity.

**Paged:** `safe_mode`, `consensus_health`, `tip_divergence`, `majority_unreachable`,
`node_behind`, and heartbeat silence (raised externally — see below).

**Opening threshold:** a condition must hold for **3 consecutive cycles** before paging, **except
`safe_mode`, which pages on first detection.** Safe mode is a halt, not a lag.

**Closing threshold:** an incident closes only after **3 consecutive healthy cycles**, including
`safe_mode`. Safe mode may open immediately but must not flap closed on a single good response.

**Initial thresholds**, to be tuned from collected data:

- poll cycle: **60s**
- open: **3 consecutive cycles** (safe mode: immediate)
- close: **3 consecutive healthy cycles**
- quorum: **2 of 3 voting nodes**
- `node_behind`: **≥ 10 blocks** below quorum median tip
- `majority_unreachable`: **≥ 2 of 3 voting nodes** unreachable

### Notification requirements

- **Emergency priority with retry-until-acknowledged** for `safe_mode`, `consensus_health`, and
  `tip_divergence`. These must persist until acknowledged rather than scroll away.
- **Persist the alert locally before attempting delivery** (the outbox guarantees this).
- **Bounded-backoff retry** on delivery failure.
- **Deduplicate by `incident_id`** — one open incident produces one page, not one per cycle.
- **Send a recovery notification when an incident closes.**
- **Credentials outside version control:** API token and user key in a systemd credential or a
  0600 root-owned environment file. Never committed, never logged.

### Planned-maintenance silencing

A maintenance window may temporarily suppress **delivery** of `node_behind`,
`majority_unreachable`, and `telemetry_degraded`.

It must **never** suppress `safe_mode` or `tip_divergence`. Those indicate chain-integrity
problems that a maintenance window does not explain, and silencing them is how a real incident
gets missed during routine work.

Suppression affects delivery only. Incidents are still opened, recorded, and closed normally.

## Heartbeat (external dead-man)

Software on the watcher host cannot report that the watcher host has disappeared. Detection
therefore lives outside it, at an external dead-man service.

- The watcher pings the heartbeat URL **every cycle (60s)**; the receiver alerts after roughly
  **5 minutes** of silence.
- **The ping is sent only after a cycle completes and its observations are committed.** A watcher
  that polls but cannot persist is broken and must not appear healthy.
- A cycle containing unreachable nodes still pings. **The heartbeat means "the watcher is
  functioning," not "the fleet is healthy."** Fleet problems raise their own incidents.
- The dead-man service must be configured to **deliver to the operator's phone independently of
  the watcher's own notification path**. If both routes share a failure, neither reports it.
- The heartbeat URL is a systemd credential, never in version control.
- The request carries **no node details and no secrets** — liveness only.

## Error handling

- A node that fails to answer a core RPC yields `reachable = false`; the cycle continues.
- SSH or RPC failure for one node never aborts the cycle for others.
- If the cycle transaction fails, the heartbeat is **not** pinged — this is what the dead-man is
  for.
- Notifier failure never blocks the cycle; the notification stays in the outbox and is retried.

## Testing

- **Rules are pure functions over cycle sequences and are unit-tested directly**, including:
  voting nodes one block apart (must not page), hash disagreement at the common height (must
  page), no unique quorum (must report `consensus_health`), `unknown` safe mode (must not read as
  inactive), and an observer node that is down or behind (must never affect quorum).
- Poller tested against recorded RPC fixtures, including a timing-out node and a node answering
  -32601 for `safemode.status`.
- Store tested for single-transaction cycle writes, and for outbox durability across a simulated
  crash between incident creation and delivery.
- Notifier tested with a fake transport for dedup, retry, and recovery notifications.
- **The dead-man switch is tested explicitly by stopping the watcher and confirming an alert
  arrives.** An untested dead-man is indistinguishable from a dead one.

## Implementation notes

Python, matching existing operational tooling on this fleet (venv, systemd unit, 0600 secrets).
Code lives under `tools/fleet-watcher/`.

Fleet inventory — which hosts are polled, their addresses, and their roles — is configuration,
not code, and is not recorded in this repository.

## Follow-on: Sub-project A

Designed separately, from watcher data, and deliberately small:

- Record the reorg at the live chainstate site, where fork point and both paths are already in
  hand.
- Expose a bounded event ring plus a process-lifetime counter over RPC, carrying restart
  identity, fork point, old/new tip, disconnected/connected depth, and timestamp.
- Durable storage and alerting stay in B. A does not persist and does not alert.
- **An integration test must start the daemon, cause a reorg, call the RPC, and verify the
  returned event. Compilation is not an acceptable gate** — every dead subsystem found in this
  codebase compiled fine.
