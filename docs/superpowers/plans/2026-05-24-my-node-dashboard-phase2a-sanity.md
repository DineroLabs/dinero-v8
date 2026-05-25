# MyNodeDashboard Phase 2a — Sanity Log

**Date:** 2026-05-25T01:48:50Z
**Branch:** `feature/qt-dashboard-phase2a`

## Results

| Check | Result |
|---|---|
| Full `dinero-qt` app build | `[100%] Built target dinero-qt` |
| ctest (all dashboard suites) | PASS (3/3: SparklineWidget, DecentralizationScore, ContributionSection) |
| App launch (isolated datadir) | Starts, alive >5s, SIGTERM-exits cleanly |

## Pending human verification

- Cmd+K opens dashboard within a frame
- New CONTRIBUTION section visible below PEERS
- 3 sparklines appear (likely empty for first few ticks until traffic accumulates)
- Stat grid populates with circuits/blocks/hints/gossip numbers (likely 0/0 + real hint counts from getnetworkinfo.relay.hints)
- Decentralization score line shows "X.X / 10  \"<bucket label>\"" with non-degenerate value once the node has run a few minutes

## Known approximations (documented in code)

- bytes_relayed_24h is a 5min × 288 linear extrapolation — Phase 2b can add a real 24h buffer if needed
- peers_who_learned_via_gossip = received_relay count (proxy)
- hints_sent = received_self count (proxy)
- circuits_active = 0 + blocks_served_today = 0 placeholders until Phase 2b adds daemon-side accessors
