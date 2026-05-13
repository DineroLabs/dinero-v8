# Dinero Live Visualizer Dashboard Spec

**Status:** Draft  
**Scope:** Product and UX spec for a live Dinero chain visualizer / operator dashboard  
**Inspiration:** [bsv.lol](https://bsv.lol/) for "live chain feel"; adapted to Dinero's actual operational needs
**Companion docs:**

- [`DINERO_LIVE_VISUALIZER_WIREFRAMES.md`](./DINERO_LIVE_VISUALIZER_WIREFRAMES.md)
- [`DINERO_LIVE_VISUALIZER_API_CONTRACT.md`](./DINERO_LIVE_VISUALIZER_API_CONTRACT.md)

## 1. Goal

Build a Dinero-native live dashboard that makes the chain feel alive **and** makes node/fleet health obvious.

This should not be a generic block explorer clone. It should be a **live operational cockpit** for:

- operators
- developers
- testers
- power users

The dashboard should answer, at a glance:

- Is the chain healthy?
- Is my node healthy?
- Is the fleet in sync?
- Is mining safe right now?
- Is Utreexo healthy right now?
- What kinds of transactions are flowing?
- Are there mempool or template hazards?

## 2. Product Positioning

### What it is

- a real-time Dinero chain activity visualizer
- a node and fleet health dashboard
- a debugging surface for Utreexo, mempool, mining, and activation issues

### What it is not

- not a replacement for a canonical block explorer
- not a wallet
- not a signing surface
- not a single-provider source of truth

## 3. Core Principle

**Dinero should copy the energy of `bsv.lol`, not its trust model.**

The dashboard should feel live and visual, but the truth model must be:

1. local node first
2. fleet comparison second
3. third-party/provider views optional and clearly labeled

## 4. Primary Users

### Operator

Needs:

- fleet parity
- mining safety
- activation countdowns
- mempool hazards
- block-template health

### Protocol developer

Needs:

- Utreexo root movement
- proof coverage health
- mempool/template divergence
- witness mix
- rejection reasons

### Wallet / QA tester

Needs:

- recent tx visibility
- confirmation flow
- address activity
- sync state
- RPC health

### Public observer

Needs:

- blocks
- transactions
- live activity
- simple network status

## 5. Page Layout

The main page should be a single-screen dashboard with six bands:

1. **Top Status Bar**
2. **Live Chain Stream**
3. **Mempool + Candidate Block**
4. **Node / Fleet Health**
5. **Utreexo / Consensus Health**
6. **Recent Blocks + Alerts**

## 6. Top Status Bar

Always visible. High signal only.

Fields:

- chain: `mainnet | testnet | regtest | v7-testnet`
- local height
- fleet median height
- best hash short form
- peer count
- mempool tx count
- tx/s
- mining safety: `safe | paused | degraded`
- Utreexo health: `healthy | degraded | recovering`
- activation banner if relevant

Color semantics:

- green = healthy
- amber = watch
- red = active hazard
- blue = informational transition

## 7. Live Chain Stream

This is the part that gives the dashboard life.

A horizontally or vertically animated stream of the last `N` transactions.

Each transaction card/bar shows:

- txid short form
- type
- total output amount
- size / weight
- fee
- age
- confirmation state

### Transaction classes

For v5:

- transparent
- Taproot
- OP_RETURN-heavy
- freeze-rejected candidate
- coinbase

For v7 later:

- P2MR receive
- P2MR spend
- scheme id
- witness weight tier

### Interaction

Clicking a tx opens a right-side detail drawer with:

- full txid
- inputs / outputs summary
- output types
- mempool ancestry info if available
- rejection reason if not accepted
- block inclusion if confirmed

## 8. Mempool + Candidate Block

This is one of the most important Dinero-specific panels.

### Left: Mempool Health

Show:

- tx count
- bytes / weight
- arrival rate
- fee histogram
- oldest tx age
- rejected tx count in last 10m
- top rejection reasons

Important rejection categories:

- freeze fork gate
- non-standard script
- template self-verification failure
- missing ancestors
- witness too large
- invalid proof / signature

### Right: Candidate Block

Show:

- next block height
- current candidate size / weight
- candidate tx count
- total fees
- coinbase output
- candidate Utreexo root
- candidate composition by tx type

Critical feature:

- **show which txs are excluded and why**

This is where Dinero can beat a generic explorer. A user should be able to see:

- in mempool but excluded from template
- excluded because of policy
- excluded because mining safety gate
- excluded because activation/freeze rule

## 9. Node / Fleet Health

Dinero has repeatedly hit fleet divergence, stale daemons, and local-only weirdness. This needs to be first-class.

For each configured node:

- name: `Mac`, `CN`, `VA`, `LA`, `MO`
- version / commit
- active height
- best hash
- headers
- peer count
- mempool count
- IBD state
- mining safety state
- wallet availability if applicable

### Fleet parity section

Derived summary:

- all nodes on same tip: yes/no
- all nodes on same binary commit: yes/no
- same mempool count: yes/no
- same best hash: yes/no
- same activation view: yes/no

If not, show the exact outlier node(s).

## 10. Utreexo / Consensus Health

This is the Dinero-specific heart of the dashboard.

### Utreexo panel

Show:

- current accumulator root
- root changed on last block: yes/no
- proof generation latency
- proof refresh latency
- proof cache hits/misses if available
- missing UTXO count if any recovery/diagnostic path detects it
- rebuild status

### Safety indicators

Show explicit states:

- `proof_coverage_ok`
- `chainstate_safe_mode`
- `reindex_required`
- `recovery_marker_present`
- `forest_root_mismatch`
- `template_self_verify_ok`

This must surface the kind of problems that previously only appeared as buried log lines.

## 11. Recent Blocks

Show the last `10-20` blocks with:

- height
- hash short form
- age
- miner
- tx count
- total fees
- block size / weight
- candidate vs final delta
- Utreexo root short form

For v7 later, also show:

- P2MR spend count
- average witness weight
- scheme mix

## 12. Alerts Panel

Pinned, operator-friendly, human-readable alerts.

Examples:

- `Mac is 7 blocks behind fleet`
- `LA daemon version differs from fleet baseline`
- `Mining paused: initial block download still active`
- `Mining paused: chainstate safe mode active`
- `Template assembly excluding 3 transactions due to freeze fork`
- `Utreexo proof coverage degraded`
- `Activation in 267 blocks`
- `Activation live: private-lane actions frozen`

Alerts should have:

- severity
- start time
- current status
- suggested next action

## 13. Dinero-Specific Views

The dashboard should have optional tabs or filters for:

### A. Mining

- current miner status
- accepted templates
- rejected templates
- hashrate
- blocks found
- candidate assembly latency

### B. Wallet Test Mode

- recent sends
- recent receives
- confirmation tracker
- rejected wallet actions

### C. Activation / Fork View

- current activation height
- blocks remaining
- gates active/inactive
- what changes when active

### D. v7 / PQ View

Only after v7 exists.

Show:

- P2MR address count
- P2MR UTXO count
- ML-DSA verification counts
- witness-weight distribution
- scheme registry active rows

## 14. Data Sources

Use existing RPCs where possible and add dedicated dashboard endpoints only where aggregation is needed.

Likely source categories:

- blockchain info
- peer info
- mempool info
- mining template / mining safety
- privacy / activation status
- Utreexo proof and chainstate health
- wallet activity summaries

### Proposed architecture

- **collector layer**
  - polls local RPC
  - polls configured fleet nodes
  - normalizes into a single dashboard model

- **UI layer**
  - renders snapshot + live stream
  - never computes consensus truth on its own

## 15. Trust Model

Every value shown should be labeled by provenance:

- `local`
- `fleet`
- `derived`
- `external`

If a metric is provider-dependent, say so in the UI.

Examples:

- local mempool count = `local`
- fleet median height = `derived`
- public explorer comparison = `external`

## 16. Non-Goals

- no transaction signing
- no seed export/import
- no wallet unlock
- no consensus decision-making in the UI
- no single-node hidden assumptions presented as absolute truth

## 17. MVP

The first useful version should ship with:

- top status bar
- recent tx live feed
- recent blocks
- node/fleet table
- mempool summary
- mining safety status
- Utreexo health summary
- alerts drawer

That is enough to make Dinero operationally better immediately.

## 18. Stretch Features

- animated candidate-block assembly
- tx replay speed controls
- mempool timeline scrubber
- compare-two-nodes mode
- “what changed in the last block?” diff
- export incident snapshot JSON
- embedded log snippets for active alerts

## 19. Success Criteria

The dashboard is successful if it reduces time-to-answer for:

- “why did mining pause?”
- “which node is stale?”
- “is the fleet on one tip?”
- “is Utreexo healthy?”
- “why is this tx not in the candidate block?”
- “did activation just change wallet behavior?”

## 20. Dinero-Specific Product Bet

Do not build a prettier explorer.

Build the **best live operational dashboard for a Utreexo-based chain**:

- alive like `bsv.lol`
- trustworthy like a node console
- explanatory like a test harness
- fleet-aware like an operator tool

That is the version that fits Dinero.
