# Fleet Watcher (Sub-project B) — Design

**Status:** approved design, not yet implemented
**Scope:** external fleet observability and alerting. Sub-project A (node-side reorg event
feed) is a separate spec and a separate PR. Neither blocks the other.

## Goal

Detect chain-integrity problems across the Dinero fleet as they happen, from outside the
daemon, using RPCs that work today. Persist every observation so thresholds can be tuned from
real data rather than guessed. Page only for conditions that are genuinely dangerous.

## Why B before A

A watcher built on existing RPCs ships immediately and keeps working if the node-side change is
delayed. That ordering is deliberate: three separate node-side subsystems were found in this
codebase that were written, looked finished, compiled, and never reached a running binary
(`getchaintips`/`getchainwork`/`getreorgstatus`, `telemetry.getmetrics`/`server.health`, and the
`ProductionMetrics`/`AlertThresholds` prototype deleted in #528). A design that depends on
node-side work landing first inherits that risk. A design that does not, does not.

Running B against the real fleet also produces the evidence for A: which rules fire, how often,
and which fields turn out to be missing. That is a better specification for A than reasoning
about it now.

## Non-goals

- Not a metrics/monitoring platform. No Prometheus exporter, no dashboards, no time-series
  database. If that is wanted later it is a separate decision with its own consumers.
- Not a replacement for `fleet_status`. That tool answers "what is the fleet doing right now"
  interactively; this answers "what has the fleet been doing, and has anything gone wrong."
- Does not modify the daemon. No C++ changes belong in this sub-project.
- Does not revive any part of the deleted `ProductionMetrics`/`AlertThresholds` prototype.
- Does not couple telemetry to consensus persistence.

## Deployment

One watcher process on **EU1**, as a systemd service. EU1 is chosen because it is off the NA
host that already carries the bridge watchers and seed RPC, and it already has an operational
role (the snapshot publisher).

Nodes are polled over **SSH to each node's loopback RPC**. This adds no new public exposure and
matches the existing `wdin-bridge` operational pattern.

Note on the existing RPC posture: `seed`/`seed2`/`seed3` already proxy JSON-RPC publicly over
HTTPS with a server-injected Basic auth header, because DineroDPI's bridge fleet depends on it.
This design deliberately does not use those public endpoints and does not change them. Closing
or re-authenticating the public RPC surface is separate work and is out of scope here.

## Components

Four units with clear boundaries, each independently testable:

### 1. Poller

Given a node list, returns one `Observation` per node per cycle. Pure I/O; contains no rules.

An observation records:

| field | source |
|---|---|
| `timestamp` | watcher clock, UTC |
| `node` | fleet name (`mac`, `la`, `sj`, `na`, `eu1`) |
| `height` | `getdaemonstatus` / `node.status` |
| `tip_hash` | `blockchain.getbestblockhash` |
| `peers_in`, `peers_out` | `node.status` |
| `synced` | `node.status` |
| `safe_mode_active`, `safe_mode_reason` | `safemode.status` |
| `reachable` | poll succeeded at all |
| `boot_id` | see below |

A node that fails to answer produces an observation with `reachable = false` and null
measurements. Unreachable is data, not an error — never a gap in the table.

**`boot_id`** distinguishes a restarted node from a stalled one. It is derived from the remote
process start time (`ps -o lstart=` for the `dinerod` pid over the same SSH connection), not from
the daemon, so it requires no node change. If it cannot be determined, it is null and the
restart rule simply does not evaluate for that node — a missing signal must never synthesise a
false one.

### 2. Store

SQLite on EU1. Two tables:

- `observations` — one row per node per cycle, append-only.
- `incidents` — one row per rule transition: `incident_id`, rule, node(s), opened_at,
  closed_at, severity, detail.

SQLite over append-only JSON because every real investigation is a range query — "what did the
fleet look like between 04:00 and 04:20", "how long was SJ behind" — and that is a table.

`incidents` is what makes "when did this start" a query rather than a log grep, and it is what
gives every alert a **stable incident ID** for deduplication and recovery.

### 3. Rules

Pure functions over recent observations. No I/O. This is where the false-positive discipline
lives, and it is the part most worth testing.

**Paging rules** (see Alerting for thresholds):

**Healthy majority** is used by two rules and is defined once here: the largest set of
*reachable* nodes that agree on `tip_hash` at the same height, provided that set contains at
least **3 of 5** nodes. If no such set exists, there is no healthy majority — that is itself the
`consensus_health` condition. Its height is the reference height for `node_behind`.

| rule | condition |
|---|---|
| `safe_mode` | any node reports `safe_mode_active` |
| `consensus_health` | no healthy majority exists (as defined above) |
| `tip_divergence` | two or more nodes report different `tip_hash` **at the same height** |
| `majority_unreachable` | 3 or more of 5 nodes unreachable |
| `node_behind` | a reachable node is ≥ 10 blocks below the healthy-majority height |

`majority_unreachable` and `consensus_health` can both be true at once (losing three nodes
destroys the majority). They are deduplicated by reporting `majority_unreachable` alone in that
case, because it names the cause rather than the symptom.

**Logged-only rules**, never paged: short lag, a single missed poll, peer-count changes, and
shallow expected reorgs.

The distinction that matters: **divergence is only meaningful at equal height.** Nodes at
different heights holding different tips is normal propagation. `fleet_status` reported
`block_consensus: false` on 2026-08-05 purely because nodes were one block apart — that exact
case must not page.

### 4. Notifier

Abstract interface with one concrete implementation (Pushover), so Telegram or another provider
can be added without touching the rules.

## Alerting policy

**Log everything; push only critical events.**

Every observation and every rule transition is persisted on EU1 regardless of severity.

Push only for: safe mode, consensus-health failure, sustained same-height tip disagreement,
majority of the fleet unreachable, a node stalled materially behind the healthy majority, and
watcher heartbeat silence (raised by the external dead-man, not by the watcher).

**Confirmation threshold:** a condition must hold for **3 consecutive cycles** before paging —
**except `safe_mode`, which pages on first detection.** Safe mode is a halt, not a lag; delaying
that page buys nothing.

**Initial thresholds**, to be tuned from collected data rather than treated as settled:

- poll cycle: **60s**
- confirmation: **3 consecutive cycles**
- `node_behind`: **≥ 10 blocks** behind the healthy-majority height
- `majority_unreachable`: **≥ 3 of 5** nodes unreachable

Tuning these from real observations is an explicit purpose of the `observations` table.

### Pushover requirements

- **Emergency priority with retry-until-acknowledged** for `safe_mode` and `tip_divergence`. A
  consensus-health warning must persist until acknowledged rather than scroll away among
  ordinary notifications.
- **Log the alert locally before attempting delivery.** An alert that was raised must be
  recoverable even if delivery fails entirely.
- **Bounded-backoff retry** on delivery failure.
- **Deduplicate by `incident_id`** — one open incident produces one page, not one per cycle.
- **Send a recovery notification when an incident closes.**
- **Credentials outside Git:** API token and user key in a systemd credential or a 0600
  root-owned environment file. Never committed, never logged.

## Heartbeat (external dead-man)

Software running on EU1 cannot report that EU1 has disappeared. Detection therefore lives
outside it, at **Healthchecks.io**.

- The watcher pings the heartbeat URL **every cycle (60s)**.
- The receiver alerts after roughly **5 minutes** of silence.
- **The ping is sent only after a cycle completes and its observations are committed to
  storage.** A watcher that is polling but failing to persist is broken, and must not appear
  healthy.
- A cycle containing unreachable nodes still pings. **The heartbeat means "the watcher is
  functioning," not "the fleet is healthy."** Fleet problems raise their own Pushover incidents.
- The heartbeat URL is a systemd credential, never in Git.
- The request carries **no node details and no secrets** — it is a liveness signal only.

## Error handling

- A node that fails to respond yields `reachable = false`; the cycle continues.
- SSH or RPC failure for one node never aborts the cycle for the others.
- If the store write fails, the cycle does **not** ping the heartbeat — this is the condition the
  dead-man exists to catch.
- Notifier failure never blocks the cycle; the incident is already persisted and delivery is
  retried with bounded backoff.

## Testing

- **Rules are pure functions over observation sequences and are unit-tested directly**, including
  the one-block-apart case that must not page and the same-height-divergence case that must.
- Poller tested against recorded RPC fixtures, including a node that times out and a node
  missing `safemode.status` (older daemons answer -32601; that must read as unknown, never as
  "not in safe mode" — the same rule already applied in DineroDPI's safe-mode surfacing).
- Store tested for append-only behaviour and incident open/close transitions.
- Notifier tested with a fake transport for dedup, retry, and recovery-notification behaviour.
- **The dead-man switch is tested explicitly by stopping the watcher and confirming an alert
  arrives.** An untested dead-man is indistinguishable from a dead one.

## Implementation notes

Python, matching the `wdin-bridge` operational precedent on this fleet (venv, systemd unit,
0600 secrets). Code lives under `tools/fleet-watcher/` in this repository.

## Follow-on: Sub-project A

Designed separately, from watcher data, and deliberately small:

- Record the reorg at the live chainstate site (`chainstate_service.cpp`, where `fork_point`,
  `disconnect_path` and `connect_path` are already in hand).
- Expose a bounded event ring plus a process-lifetime counter over RPC, carrying boot ID, fork
  point, old/new tip, disconnected/connected depth, and timestamp.
- Durable storage and alerting stay in B. A does not persist and does not alert.
- **An integration test must start `dinerod`, cause a reorg, call the RPC, and verify the
  returned event. Compilation is not an acceptable gate** — every dead subsystem found in this
  codebase compiled fine.
