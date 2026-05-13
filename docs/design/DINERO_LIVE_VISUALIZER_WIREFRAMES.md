# Dinero Live Visualizer Wireframes

**Status:** Draft  
**Scope:** Layout and interaction spec for the Dinero live dashboard MVP and later expansions  
**Companion:** [`DINERO_LIVE_VISUALIZER_DASHBOARD_SPEC.md`](./DINERO_LIVE_VISUALIZER_DASHBOARD_SPEC.md)

## 1. Design Goal

Turn the high-level dashboard spec into concrete screens that a frontend can build and a backend can support.

This document is intentionally practical:

- screen regions
- information hierarchy
- interaction behavior
- responsive layout
- state handling

## 2. Global UX Principles

### A. Operator-first

The page should answer "is something wrong?" before it answers "what is interesting?"

### B. Local-first truth

The local node drives the primary story. Fleet and external comparisons are secondary.

### C. Motion with restraint

Use animation to show liveness, not to decorate:

- tx feed drift
- block confirmation snap
- alert appearance
- candidate block inclusion/exclusion transitions

### D. Provenance visible

Every major panel should make it clear whether data is:

- local
- fleet-derived
- external

## 3. Primary Desktop Layout

Default target: 1440px+ wide operator desktop.

```text
+-----------------------------------------------------------------------------------+
| Top Status Bar                                                                    |
| Chain | Local Tip | Fleet Tip | Best Hash | Peers | Mempool | Mining | Utreexo   |
+----------------------------------------+------------------------------------------+
| Live Chain Stream                      | Alerts                                   |
| tx cards / animated flow               | active alerts list                       |
| click -> drawer                        | severity + age + next action             |
+----------------------------------------+------------------------------------------+
| Mempool Health                         | Candidate Block                          |
| fee histogram, rejected reasons, age   | next block, excluded txs, fees, root     |
+-----------------------------------------------------------------------------------+
| Node / Fleet Health                                                               |
| node table with parity highlighting                                               |
+----------------------------------------+------------------------------------------+
| Utreexo / Consensus Health             | Recent Blocks                            |
| roots, proof latency, recovery state   | last 10-20 blocks with block details     |
+-----------------------------------------------------------------------------------+
| Footer: provenance legend, refresh state, API latency, build version              |
+-----------------------------------------------------------------------------------+
```

## 4. Mobile / Narrow Layout

For mobile or narrow panes, stack sections in this order:

1. Top Status
2. Alerts
3. Live Chain Stream
4. Mempool
5. Candidate Block
6. Node / Fleet
7. Utreexo / Consensus
8. Recent Blocks

Rules:

- no dense table-first layout
- collapse fleet table into cards
- keep alerts above the fold
- transaction detail uses full-screen drawer

## 5. Top Status Bar

### Layout

```text
[mainnet] [3733 / 3733] [0000001c9a9c...] [4 peers] [0 mempool]
[Mining: SAFE] [Utreexo: HEALTHY] [Activation: 267 blocks]
```

### Behavior

- updates every 2s via snapshot polling or incremental events
- hash is copyable
- clicking `Mining` opens mining-safety detail drawer
- clicking `Utreexo` scrolls to Utreexo panel

### States

- `SAFE`
- `PAUSED`
- `DEGRADED`
- `RECOVERING`

## 6. Live Chain Stream

### Layout

```text
+------------------------------------------------------------------+
| Live Chain Stream                                                |
| [tx] [tx] [tx] [tx] [tx] [tx] [tx]                               |
| [tx] [tx] [tx] [tx] [tx]                                         |
+------------------------------------------------------------------+
```

Each tx tile includes:

- short txid
- type badge
- amount
- fee
- age
- pending/confirmed state

### Interaction

On click, open right-side detail drawer:

```text
+--------------------------------------+
| TX Detail                            |
| txid                                 |
| provenance                           |
| type                                 |
| fee / weight                         |
| outputs summary                      |
| mempool status                       |
| candidate block status               |
| rejection reason (if any)            |
| block inclusion / height             |
+--------------------------------------+
```

### Animation

- new tx enters with short slide/fade
- confirmation swaps pending chip to confirmed chip
- rejected tx flashes amber/red then settles into a rejected badge

## 7. Alerts Panel

### Purpose

This panel should be visible without scrolling on desktop.

### Layout

```text
+------------------------------------------------------+
| Alerts                                               |
| [RED] Mining paused: chainstate safe mode active     |
| [AMB] VA is 3 blocks behind fleet                    |
| [BLU] Freeze fork activates in 267 blocks            |
| [GRN] Recovery complete on local node                |
+------------------------------------------------------+
```

Each alert row shows:

- severity chip
- message
- started-at age
- source
- optional action hint

### Interaction

Click to expand:

- full reason
- implicated node(s)
- suggested next checks
- related metrics

## 8. Mempool Health Panel

### Layout

```text
+------------------------------------------------------+
| Mempool                                              |
| Tx Count  | Total Weight | Oldest Age | Arrival/s    |
| Fee histogram                                        |
| Rejected in last 10m                                 |
| - freeze fork gate: 3                                |
| - template self-verify: 1                            |
| - missing ancestor: 8                                |
+------------------------------------------------------+
```

### Key widgets

- fee histogram
- tx age distribution
- top rejection reasons
- inflow/outflow sparklines

## 9. Candidate Block Panel

### Layout

```text
+------------------------------------------------------+
| Candidate Block                                      |
| Height 3734 | Weight 784 | Fees 0.00000000          |
| Coinbase | Utreexo root short form                   |
| Included txs                                         |
| Excluded txs                                         |
| - txid... : excluded by mining safety gate           |
| - txid... : excluded by freeze rule                  |
+------------------------------------------------------+
```

### Behavior

- excluded list is capped in UI but expandable
- explicit "why not included?" is mandatory
- if no candidate exists, show the reason, not an empty box

## 10. Node / Fleet Health Panel

### Desktop Layout

```text
+-----------------------------------------------------------------------------------+
| Node / Fleet Health                                                               |
| Node | Commit | Height | Hash | Peers | Mempool | IBD | Mining | Utreexo | Drift |
| Mac  | abc123 | 3733   | ...  | 4     | 0       | no  | safe   | healthy | no    |
| CN   | abc123 | 3733   | ...  | 8     | 0       | no  | safe   | healthy | no    |
| VA   | abc123 | 3733   | ...  | 8     | 0       | no  | safe   | healthy | no    |
+-----------------------------------------------------------------------------------+
```

### Visual rules

- outlier rows get tinted background
- differing commit hashes are red
- differing tip hash is red
- differing height but same best hash is amber

### Fleet summary strip

Above table:

- `Tip parity: YES/NO`
- `Binary parity: YES/NO`
- `Mempool parity: YES/NO`
- `Activation parity: YES/NO`

## 11. Utreexo / Consensus Panel

### Layout

```text
+------------------------------------------------------+
| Utreexo / Consensus Health                           |
| Root                  ddb9eb30...                    |
| Last root change      14s ago                        |
| Proof generation      12 ms                          |
| Proof refresh         4 ms                           |
| Coverage              healthy                        |
| Safe mode             false                          |
| Recovery marker       false                          |
| Template self-verify  true                           |
+------------------------------------------------------+
```

### Failure states

If coverage degraded:

- show missing count
- show first detection time
- show whether node is in safe mode
- link to recent relevant alerts

## 12. Recent Blocks Panel

### Layout

```text
+------------------------------------------------------+
| Recent Blocks                                        |
| 3733 | 14s ago | 1 tx  | fees 0.00 | miner CN       |
| 3732 | 2m 14s  | 4 txs | fees 0.02 | miner VA       |
| 3731 | 4m 03s  | 0 txs | fees 0.00 | miner MO       |
+------------------------------------------------------+
```

Expanded block view shows:

- full hash
- miner
- tx count
- fee sum
- weight
- coinbase summary
- Utreexo root
- optional diff from previous block

## 13. Drawer System

Use one shared detail drawer pattern for:

- tx details
- block details
- node details
- alert details
- candidate block details

Rules:

- drawer opens from right on desktop
- drawer becomes full-screen sheet on mobile
- drawer state is URL-addressable if possible

## 14. Time and Refresh Behavior

### Polling / streaming model

- top status: 2s
- live tx stream: event-driven if available, fallback 2s poll
- mempool and candidate block: 2s
- node/fleet health: 5s
- Utreexo panel: 5s
- recent blocks: 5s
- alerts: event-driven + 2s refresh

### User controls

- pause animations
- freeze live updates
- auto-scroll on/off for tx stream

## 15. Empty and Failure States

### Empty mempool

Show:

- `No transactions in mempool`
- candidate block still visible

### No peers

Show:

- `0 peers`
- affected panels tinted amber/red
- explicit note that fleet comparisons may be stale

### RPC unavailable

Show:

- panel-local error state
- last successful update timestamp
- retry status

## 16. Implementation Notes

### Frontend stack assumptions

Any stack is fine, but the UI should support:

- real-time updates
- drawers
- compact data tables
- basic charting
- responsive layout

### Accessibility

- no color-only status signals
- keyboard navigable drawers
- reduced-motion mode
- readable hash truncation with copy actions

## 17. MVP Acceptance

The wireframe is considered implemented when:

- operators can detect node/fleet drift in under 10 seconds
- mining pause reasons are visible without reading logs
- candidate block exclusions are visible by tx and reason
- Utreexo health is readable without CLI/RPC spelunking

