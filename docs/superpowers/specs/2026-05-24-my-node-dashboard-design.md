# MyNodeDashboard — Design

**Date:** 2026-05-24
**Author:** Dinero Labs (with Claude Opus 4.7)
**Status:** Spec (pending implementation plan)
**Target repo:** `DineroLabs/dinero-v8` (Qt code at `qt/`; standalone `dinero-qt` repo is archived)
**Target branch (impl):** `feature/my-node-dashboard` (off `qt-main`)
**Related:** dinero-v8 PR #128 (RELAY_HINTS Phase 1a), PR #134/#135/#137 (Dynamic P2P)

---

## Background

The Cmd+K shortcut in `dinero-qt` currently slides in `AiPanel` from the right — a setup/chat surface for the optional DineroAI integration. The panel infrastructure is already excellent: a `QPropertyAnimation` on `panelWidth`, `QStackedWidget` for multiple sub-screens, lives inline in `contentArea`'s layout (not a floating overlay so it composes with the rest of the window).

The slide-in container is well-built but underutilized. Two converging shifts make this real estate ripe for a much higher-value default:

1. **The decentralization vision is now real.** RELAY_HINTS Phase 1a (PR #128) closed the cache half of 3b; the source-side half landed in PR #130. Dynamic P2P (PRs #134/#135/#137) ships peer quality scoring. Every dinero-qt user — embedded miner, mobile-ish home node, fleet operator alike — is genuinely participating in the network with their own opinions about peers, their own hint cache, their own relay contribution. They are operators.

2. **No wallet has ever shown users this view of themselves.** Bitcoin Core has `debug.log` — terrible UX. Electrum hides the network behind two clicks. Wasabi shows Tor circuits but not your routing. **dinero-qt can be the first wallet UI that makes every user feel like a node**, with the same caliber of operator dashboard the fleet operators have, pointed at their own node and their own view of the network.

## Goal

A slide-in Cmd+K panel — replacing `AiPanel` as the default — that gives every user operator-grade visibility into:
- Who they are in the network (identity, NAT posture, relay role, mining status)
- The network as they see it (chain tip race, peer height consensus, mempool)
- Their peers (with Dynamic P2P quality scores, direction, latency, services)
- Their contribution (traffic, circuits, blocks served, hints sent, decentralization score)
- Their discovery cache (RELAY_HINTS table contents with freshness + failure decay)

No privileged access required. Pure RPC consumption against the embedded daemon.

## Non-goals

- Replace existing tabs (Overview, Send, Receive, Mining, etc.). The dashboard is additive in the Cmd+K slide-in space.
- Add new daemon RPCs in v1. Everything the dashboard needs already exists.
- Show fleet-server internals (`LA`/`VA`/`MO`/`CN`). Per the decentralization vision, the dashboard is a node's view of *itself and what it can observe*, not a privileged window into anyone else's box. Fleet nodes appear in the peers table if connected, exactly like any other peer.
- Implement the AI tab. The existing `AiPanel` stays; the new dashboard hosts it as a tab.

## Architecture

### Tabbed container replaces `AiPanel` as the Cmd+K target

The current `AiPanel` becomes one of several tabs inside a new `CmdKPanel` container widget. Cmd+K toggles `CmdKPanel`; the default tab is the new `MyNodeDashboard`.

```
CmdKPanel (QWidget, slide-in via QPropertyAnimation on panelWidth)
├── QTabBar (top — vertical icons + labels, or horizontal tabs)
└── QStackedWidget (existing pattern)
    ├── MyNodeDashboard  (NEW — default)
    ├── AiPanel          (existing — demoted from primary to a tab)
    └── (future tabs slot in here: address book, RPC console, wDIN, etc.)
```

This preserves the existing slide-in animation, keeps the AI feature accessible, and gives every future "panel-worthy" feature a natural home without touching mainwindow.cpp again.

### `MyNodeDashboard` is composed of section widgets

Each section is its own `QWidget` subclass — testable in isolation, scrollable as a unit, no monolithic painter:

```
MyNodeDashboard (QScrollArea wrapping a QVBoxLayout)
├── IdentitySection         ⚡ YOU
├── NetworkSection          📡 NETWORK · as you see it
├── PeersSection            🛰  PEERS (8 connected)
├── ContributionSection     🌐 YOUR CONTRIBUTION
└── DiscoverySection        💡 DISCOVERED · relay-hint cache
```

A shared `NodePoller` (single instance per `MyNodeDashboard`) drives all sections via Qt signals. No section polls the daemon directly.

### Polling, not push

The daemon doesn't push updates. `NodePoller` calls the relevant RPCs on a 5-second `QTimer`, parses results, emits typed signals (e.g., `peersUpdated(QVector<PeerRow>)`, `chainInfoUpdated(ChainInfo)`). Each section connects only to the signals it needs.

Sparklines (Contribution section) require finer granularity. The poller maintains rolling 5-minute / 1-hour ring buffers; sparkline widgets render whatever interval they're configured for.

### No daemon changes in v1

Every needed value is already exposed:
- `getnetworkinfo` — node identity, services, version, our local NAT posture, connection counts, **relay_hints counters from PR #128**, **Dynamic P2P observe metrics from PR #137**
- `getpeerinfo` — full per-peer list with version, services, bytes, ping, height, direction
- `getblockchaininfo` — height, difficulty, mempool, headers, verification progress
- `getrelayinfo` — registrants we're hosting, grace state (added in PR #130)
- `getmempoolinfo` — mempool size + bytes
- `getmininginfo` — hashrate, shares, network difficulty (only consumed if mining is active)

If a future iteration wants the "ping-now" or "dial-hint-now" interactions to be more responsive, daemon-side RPCs may grow (e.g., `relay_hints.dial`), but that's deferred.

---

## Visual layout (canonical)

```
┌──────────────────────────────────────────────────────────┐
│  ⚡ YOU                                       ⚙   ×       │
│  ──                                                      │
│  fd4f c04d f38b acbf  72d4 ecae 451d 1589  570b caba  📋 │
│                                                          │
│   ●  DIRECT  · reachable on 162.200.227.214:20999        │
│   ⤴  RELAYING for 2 peers · 1 in grace                   │
│   ⛏  MINING to EpycOne · 12 shares/min                   │
│                                                          │
│   uptime  2h 47m       v8.0.0-rc16  ·  +12 commits       │
├──────────────────────────────────────────────────────────┤
│  📡 NETWORK · as you see it                              │
│  ──                                                      │
│                                                          │
│      you  ━━━━━━━━━━━━━━━━━━━━━━●  27 402                │
│      net  ━━━━━━━━━━━━━━━━━━━━━━━●  27 403  (+1 ahead)   │
│                                                          │
│  ▓▓▓▓▓▓▓▓░░  8/10 peers at tip                           │
│  ▒▒          2/10 peers at +1   ← block in flight        │
│                                                          │
│  difficulty  0x1d31ffce         mempool 247 tx / 84 KB   │
│  median fee  120 una/vB         next bits  -0.4%         │
├──────────────────────────────────────────────────────────┤
│  🛰  PEERS  (8 connected)                  [Q ▼] [☷ map] │
│  ──                                                      │
│   Q   dir  who                          height   ping   │
│  ●92  ↓   173.249.195.59 · LA          27 402   12 ms   │
│  ●88  ↓   172.93.160.131               27 402   18 ms   │
│  ●75  ↑   72.18.214.120                27 402    8 ms   │
│  ●70  ⇄   relay:fd4f…:a019  (via LA)   27 402    —      │
│  ◐55  ↓   102.18.x.x                   27 401   72 ms   │
│  ◐44  ↑   84.111.x.x                   27 402  154 ms   │
│  ⚠ 22  ↓   38.65.x.x         stalling   27 387   —      │
│  ○      ↑   18.119.x.x        handshake  —       —      │
│                                                          │
│  ↳ click row for details · right-click for actions       │
├──────────────────────────────────────────────────────────┤
│  🌐 YOUR CONTRIBUTION                                    │
│  ──                                                      │
│                                                          │
│   bytes in       ─▁▂▁▃▂▂▁▃▄▃▂▁▂▃▅▄▃▂▁▂  88 KB/s avg     │
│   bytes out      ─▁▁▂▁▁▂▁▂▂▁▁▁▂▃▂▁▁▁▁▁  41 KB/s avg     │
│   relay traffic  ─▁▂▂▃▃▂▂▂▂▂▃▃▃▂▂▂▂▂▂▂  12 KB/s         │
│                                                          │
│   circuits routing through you      3 active             │
│   blocks served                     14 today             │
│   hints I've sent                   247                  │
│   peers who learned of me via gossip   18                │
│                                                          │
│   ▶  Decentralization score · 7.3 / 10                   │
│      "you're carrying real weight"                       │
├──────────────────────────────────────────────────────────┤
│  💡 DISCOVERED · relay-hint cache (12)                   │
│  ──                                                      │
│   target         source     freshness         fails      │
│  ● 931db4ff…   relay@LA     ▓▓▓▓▓▓▓▓▓░  2m   0          │
│  ● ab12cd34…   relay@MO     ▓▓▓▓▓▓▓░░░  4m   0          │
│  ● 78ef9abc…   gossip       ▓▓▓▓▓░░░░░  7m   0          │
│  ⚠ 4c8a91b2…   relay@CN     ▓▓░░░░░░░░ 12m   2          │
│  ✗ fd4fc04d…   relay@VA     ░░░░░░░░░░ 15m   3 → evict  │
│                                                          │
│   ↳ click to dial now · evicted entries fade out         │
└──────────────────────────────────────────────────────────┘
```

Panel width: **520 px**, matching the current `AiPanel` slide width. Sections stack vertically inside a `QScrollArea` so a smaller window still works.

---

## Section details

### `IdentitySection`

**Data sources:** `getnetworkinfo` (services, version, subversion, localaddresses), `getrelayinfo` (am-I-relaying), `getmininginfo` (am-I-mining), uptime via `getuptime`.

**Layout:**
- node_id (20 bytes hex, formatted in 4-char groups), click-to-copy
- One status line per active role, each with a color-coded leading glyph:
  - `●` reachability — DIRECT (NAT open) / BEHIND-RELAY / UNREACHABLE
  - `⤴` relaying — count of registrants + grace count, OFF if not relaying
  - `⛏` mining — pool/destination + shares/min, OFF if not mining
- Footer: uptime · daemon version · commits-since-tag

**Refresh cadence:** every 5s (low-rate; identity rarely changes).

**Empty / error states:**
- Daemon offline → "● UNREACHABLE — daemon not responding" with retry button
- Identity not yet wired (early startup) → spinner

### `NetworkSection`

**Data sources:** `getblockchaininfo` (our tip), `getpeerinfo` (peers' reported heights), `getmempoolinfo`, `getnetworkinfo.median_fee` (or computed from mempool stats), `getblockchaininfo.difficulty`.

**Layout:**
- Two horizontal bars, full panel width, scaled to the max-tip among self+peers:
  - "you" line — your local tip
  - "net" line — the consensus peer-reported tip (mode of peer heights). Delta is highlighted ("+1 ahead" red, "in sync" green, "-2 behind" yellow).
- Histogram of peer heights below the bars: each bucket = a height, fill = peer count at that height. Tip bucket is full-saturation, others fade by distance.
- Three pairs of secondary metrics in a 3-col grid:
  - difficulty (current 32-bit compact) · mempool (tx count + bytes)
  - median fee (una/vB) · next-bits estimate (% delta predicted by DAA)

**Refresh cadence:** 5s for tip/peers; 30s for difficulty (changes slowly per block).

**Why this matters:** the "you vs net" race is *visceral*. Watching the gap close as blocks arrive is information no other wallet surfaces.

### `PeersSection`

**Data sources:** `getpeerinfo`, augmented with Dynamic P2P quality scores from `getnetworkinfo.dynamic_p2p.observe.peers` (from PR #137).

**Layout:**
- Header: total connected count · sort selector (Q descending default, also: height, ping, direction) · `[☷ map]` toggle to switch to topology view
- Rows:
  - **Q column:** stoplight glyph + score (`●92` green, `◐55` yellow, `⚠22` red, `○` handshaking)
  - **dir column:** `↓` inbound · `↑` outbound · `⇄` relay-virtual
  - **who column:** `addr:port` for direct, `relay:<node_id_short>:<circuit_id> (via <relay>)` for relay-virtual. Known fleet IPs annotated with name (LA/VA/MO/CN — derived from app config, not from privilege)
  - **height column:** peer's last-reported height, "stalling" annotation if behind by >5 blocks for >2 min, "syncing" if catching up
  - **ping column:** ms, dash if unmeasured (relay-virtual)
- Click row → inline expand:
  - Services bitmap (decoded: NODE_NETWORK, NODE_WITNESS, NODE_DINERO_V2, NODE_RELAY, NODE_BEHIND_RELAY, NODE_RELAY_HINTS_V2)
  - Version string
  - BIP-155 net type
  - Bytes sent / received (since connection start)
  - Connection age
  - Last message age
- Right-click row → context menu:
  - Disconnect (`disconnectnode`)
  - Ban for 1h / 24h / 30d (`setban`)
  - Pin as addnode (writes to config)
  - "Open in DPI widget" (links to existing `dpiwidget.cpp` if applicable)

**Refresh cadence:** 5s. Rows that don't change between polls keep their expansion state.

**Topology view** (toggleable, `[☷ map]`):
- `QGraphicsScene` with the current node at center
- Inner ring: direct peers as nodes, line thickness = quality score, line color matches stoplight
- Outer ring: relay-virtual peers, dashed lines through their relay
- Hover → tooltip with the same data as the row's expand state
- Empty state when no peers: a single "YOU" node with a faint "waiting for peers" pulse

### `ContributionSection`

**Data sources:** `getnetworkinfo.relay_hints` (sent count, received self/relay counters from PR #128), `getrelayinfo.circuits` (active circuit count + bytes routed, added in PR #130), block-served counter (new internal counter — see below).

**Layout:**
- Three sparklines (5-min rolling, 1-px-per-2.5s = 120 samples):
  - bytes in (across all peers, sum of bytes_recv deltas)
  - bytes out
  - relay traffic (bytes through circuits we're routing)
  - Each labeled with current rate (KB/s) and tooltip on hover showing 5min/1hr/24hr averages
- Stat grid (2 columns):
  - circuits routing through you · blocks served today · hints I've sent · peers who learned of me via gossip
- **Decentralization score row** (large): score / 10, plus a one-line plain-English description

**Refresh cadence:** sparklines update every 2.5s (smooth animation), stat grid every 5s, decentralization score every 30s (smoothed — don't make it jittery).

**"Blocks served today" counter:** requires a new internal Qt-side counter that tracks `BlockRelayManager::HandleGetData` calls. Either tail `debug.log` or add a daemon RPC `getrelaystats` in a follow-up. **For v1, derive heuristically** from `bytes out` peaks during inv/getdata sequences; flag as "approximate" in tooltip. (Or skip the metric in v1 if too noisy.)

### `DiscoverySection`

**Data sources:** new RPC `getrelayhints` (does not yet exist — small daemon addition needed). Returns the contents of `relay_hints_by_target_` with per-record: target_node_id, source (Self/RelayPush/Gossip), learned_at age, consecutive_dial_failures, source_peer_node_id.

**v1 fallback if the RPC isn't ready:** display a "coming with daemon v8.0.0-rc17" placeholder + a single aggregate count from existing `getnetworkinfo.relay_hints.received_*`.

**Layout:**
- Header: count of currently cached targets
- Rows:
  - target_node_id (8 chars + ellipsis)
  - source (relay@<name> for RelayPush, "gossip" for Gossip, "self" for Self)
  - freshness bar (10-segment, depleted by `(age / kHintTtl)` — full = fresh, empty = about to evict)
  - failure count, with `→ evict` annotation if `consecutive_dial_failures >= kHintMaxFailures - 1` (one more failure and it's gone)
- Stoplight glyph: ● fresh (< 5min), ◐ ageing (5-12min), ⚠ near-eviction (failures >=2 OR age >12min), ✗ evicting on next sweep
- Click row → "Dial now" — issues a one-shot dial via existing orchestrator (needs daemon RPC `relayhints.dial` — Phase 3, optional)
- Evicted entries fade out over 1s before being removed from the list (Qt opacity animation)

**Refresh cadence:** 5s.

---

## Decentralization score (formula)

Single number 0-10 designed to:
1. Reward operating behaviors that strengthen the network
2. Be locally computable (no central oracle)
3. Be GAMEABLE in healthy ways (opening a port, becoming a relay, sustaining uptime — all things we want users to do)

```
score = clamp(
    1.0 * reachable_score      +   // 0 or 1.0
    2.0 * relay_active_score   +   // 0 or 2.0
    1.5 * uptime_score         +   // 0 to 1.5 (linear to 30d uptime)
    1.5 * peer_diversity_score +   // 0 to 1.5 (based on unique /16 subnets among peers)
    1.0 * traffic_score        +   // 0 to 1.0 (log-scale of bytes relayed in last 24h)
    1.5 * mining_score         +   // 0 to 1.5 (linear to ratio of fleet hashrate; capped)
    1.5 * gossip_reach_score,      // 0 to 1.5 (peers_who_learned_of_me_via_gossip / 32)
    0, 10)
```

Where:
- `reachable_score` = 1 if `getnetworkinfo.local_relay` is true AND we have any inbound peer; else 0
- `relay_active_score` = 2 if `getrelayinfo.is_relay_active` and we have ≥1 registrant; else 0
- `uptime_score` = `min(uptime_days / 30, 1.0) * 1.5`
- `peer_diversity_score` = `(unique_/16_count / 8) * 1.5`, capped at 1.5
- `traffic_score` = `min(log10(bytes_relayed_24h) / 9, 1.0) * 1.0` (10^9 bytes = 1 GB = full score)
- `mining_score` = `min(local_hashrate / fleet_estimated_hashrate, 1.0) * 1.5` (capped to prevent a single big miner from dominating; signaling "you matter" not "you dominate")
- `gossip_reach_score` = `min(peers_who_learned_of_me_via_gossip / 32, 1.0) * 1.5`

**Plain-English mapping:**
- 0-2: "just observing" (default for a fresh non-listening node)
- 2-4: "consuming responsibly"
- 4-6: "pulling your weight"
- 6-8: "you're carrying real weight"
- 8-10: "you're load-bearing for the network"

Tooltip on click → table showing each component score with "how to improve" hints ("open port 20999 to gain +1.0 reachable score").

**Calibration is a Phase 2 concern.** v1 ships the formula; we tune weights based on real values seen in canary.

---

## Data flow

### `NodePoller` (single instance per MyNodeDashboard)

```
NodePoller (QObject)
  ├── QTimer ticks every 5s
  ├── On tick: issue all RPCs in parallel via existing RpcClient
  ├── Parse responses
  └── Emit typed signals:
      ├── identityUpdated(NodeIdentity)
      ├── chainInfoUpdated(ChainInfo)
      ├── peersUpdated(QVector<PeerRow>)
      ├── contributionUpdated(ContributionStats)
      └── discoveryUpdated(QVector<HintRow>)
```

`NodePoller` also maintains the rolling buffers feeding sparklines:
- 120 samples × 3 series (bytes_in, bytes_out, relay_bytes), updated every 2.5s
- Computed from `getpeerinfo[].bytessent/bytesrecv` deltas
- Sparkline widget emits redraw on buffer push

Each section connects only to the signals it needs (no monolithic state).

### Error handling

- **Daemon offline** (RPC timeout > 3s, 3 consecutive failures) → `NodePoller` enters "degraded" state, emits `daemonStateChanged(false)`. All sections show a "● UNREACHABLE" overlay and freeze last-known values (with a "(stale)" tag). Polling continues at 30s instead of 5s until daemon returns.
- **Partial data** (one RPC fails, others succeed) → emit only the signals for successful RPCs. Sections individually decide whether to update or hold.
- **Cookie auth not found** (fresh datadir, daemon still starting) → same as "daemon offline" + a different message ("daemon starting").
- **RPC parse error** (unexpected JSON shape) → log to `qDebug`, skip the update, continue. Never crash the UI on bad daemon data.

---

## Interaction model

| Action | Target | Result |
|---|---|---|
| Cmd+K | anywhere in mainwindow | Toggle CmdKPanel slide-in. Default tab = Dashboard. Repeats restore last-active tab. |
| Cmd+1..Cmd+N | inside CmdKPanel | Switch to tab N (Dashboard, AI, future) |
| Esc | inside CmdKPanel | Close panel |
| Click node_id (Identity) | text | Copy to clipboard, briefly flash "copied" |
| Click row (Peers) | row | Inline expand (services bitmap, version, bytes, age). Click again to collapse. |
| Right-click row (Peers) | row | Context menu: Disconnect · Ban 1h/24h/30d · Pin as addnode · Open in DPI |
| Click ☷ map (Peers) | toggle | Replace peers table with topology view. Toggle is sticky per session. |
| Click "Relaying" toggle (Identity) | toggle | Confirm dialog → enable/disable advertising as relay (writes config + sends RELAY_REGISTER) |
| Click hint row (Discovery) | row | "Dial now" — issues immediate orchestrator dial (Phase 3, needs RPC) |
| Hover sparkline (Contribution) | sparkline | Tooltip with 5min/1hr/24hr averages |
| Click decentralization score (Contribution) | score widget | Tooltip with formula breakdown + "how to improve" hints |

---

## Testing strategy

### Unit tests (Qt headless, no daemon)

`MockNodePoller` — a `NodePoller` subclass that emits pre-baked signals on demand. Sections become testable in isolation:

- `IdentitySection`: assert glyph + label for each posture (DIRECT/BEHIND-RELAY/UNREACHABLE), assert mining/relaying state visibility, assert node_id formatting
- `NetworkSection`: assert bar lengths for "+1 ahead" / "in sync" / "-2 behind" cases, assert histogram fills match peer-height distribution
- `PeersSection`: assert sort by Q (default), assert stoplight glyph per Q range, assert row expand persists across mock updates
- `ContributionSection`: assert sparkline shape from buffer, assert decentralization score formula for canned inputs
- `DiscoverySection`: assert freshness bar fills, assert glyph transitions (● → ◐ → ⚠ → ✗), assert fade-out animation triggers on evict

Test framework: existing Qt Test (`QtTest`). Tests live in `qt/tests/` (new directory if not present).

### Integration tests (with regtest daemon)

`tests/integration/test_my_node_dashboard.sh`:
1. Spin up 3 regtest nodes (A, B, C) with A as relay
2. Launch `dinero-qt` pointed at node B's datadir, scripted via `QTEST_QPA_PLATFORM=offscreen` + a small Qt remote-control harness OR a snapshot test that just dumps the dashboard's HTML representation after 10s
3. Assert: identity shows correct node_id, peers list shows A + C, network section's tip race shows in sync

### Manual sanity checklist (per release)

- [ ] Cmd+K opens dashboard within 1 frame, no jank
- [ ] All five sections render with real fleet-canary data
- [ ] Sparklines smooth over 5min of observation
- [ ] No crash on daemon stop/start cycle
- [ ] Tab switch to AI panel works; switch back works
- [ ] Memory usage steady over 1hr (no leak from sparkline buffers)

---

## Phasing

### Phase 1 — MVP (3 days)
- `CmdKPanel` container + tab switcher
- `IdentitySection` complete
- `NetworkSection` complete (without sparkline of mempool — just current value)
- `PeersSection` as a static table (sort + expand, no topology view)
- `NodePoller` with 5s cadence
- Wire Cmd+K to `CmdKPanel`, demote `AiPanel` to a tab

Ships immediately useful value: users can SEE themselves and their network position. No new daemon code required.

### Phase 2 — Contribution + Discovery (1 week)
- `ContributionSection` with sparklines (rolling buffer infrastructure)
- `DiscoverySection` once daemon RPC `getrelayhints` lands (or display placeholder until then)
- `DecentralizationScore` widget with tooltip
- "Blocks served today" counter (heuristic OR after daemon RPC lands)

### Phase 3 — Topology + Actions (1 week)
- Topology view via `QGraphicsScene` (`☷ map` toggle on PeersSection)
- Right-click peer actions wired (disconnect/ban/pin)
- Hint cache "dial now" action (requires `relayhints.dial` RPC in daemon)
- Identity "Relaying" toggle wired (writes config, sends RELAY_REGISTER)

Each phase ships on its own PR, each in its own canary release cycle.

---

## Open questions

1. **AI tab demotion intensity.** Is the AI tab truly secondary now, accessible only via tab-click? Or should there be a separate Cmd+Shift+K shortcut that opens the AI tab directly? My recommendation: tab-only for v1, add a direct shortcut later if telemetry shows demand.

2. **Sparkline cadence.** 2.5s per sample = 120 samples in 5min, smooth-ish. Faster (1s) is smoother but doubles redraw load. Decision deferred to a Phase 2 perf test.

3. **Decentralization score weights.** The formula above is a first draft; weights are intuitive guesses. Real calibration requires observing real values across the user spectrum (leaf node, home-relay, fleet-relay). Defer to Phase 2.

4. **"Blocks served today" tracking.** Three options: (a) heuristic from byte-rate peaks, (b) tail `debug.log`, (c) new daemon counter + RPC. Each has tradeoffs. **Recommendation:** ship Phase 1 without this metric; add as a new daemon counter (`relay_stats.blocks_served`) in Phase 2 — it's small and fits naturally next to the existing hint counters from PR #128.

5. **Topology view scope.** Static radial layout (cheap) or force-directed (gorgeous but more expensive)? **Recommendation:** static radial v1, force-directed if we ship it at all later.

---

## Migration & rollout

- **No daemon dependency for Phase 1.** Ships in any dinero-qt release built against v8.0.0-rc16 or later.
- Phase 2's `getrelayhints` RPC ships in a follow-up `dinero-v8` PR, scoped tiny (a new RPC method that exposes existing internal state). dinero-qt detects RPC availability via `getrpcinfo` introspection and degrades gracefully if absent.
- Phase 3's `relayhints.dial` RPC + the "Relaying" config writer are independent additions, also feature-flag-detected.
- The AI panel demotion is purely UX — no functional regression. Existing AI users transition by clicking the AI tab.

---

## Related artifacts

- Existing code: `qt/src/mainwindow.cpp` (Cmd+K wiring at line 1860), `qt/src/aipanel.{h,cpp}` (the slide-in container we're refactoring around)
- Daemon RPCs consumed: see [Architecture › No daemon changes in v1](#no-daemon-changes-in-v1)
- Design context: dinero-v8 spec `docs/superpowers/specs/2026-05-23-relay-hints-lifecycle-design.md` (the lifecycle work that makes the Discovery section meaningful)
- Memory: `project_decentralization_vision.md` — this dashboard is the user-facing surface of that vision
