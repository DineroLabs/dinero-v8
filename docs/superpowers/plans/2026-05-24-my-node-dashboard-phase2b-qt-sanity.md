# MyNodeDashboard Phase 2b qt — Sanity Log

**Date:** 2026-05-25T04:30:39Z
**Branch:** `feature/qt-dashboard-phase2b`

## Results

| Check | Result |
|---|---|
| Full `dinero-qt` app build | `[100%] Built target dinero-qt` |
| ctest -L dashboard | 5/5 PASS (SparklineWidget, DecentralizationScore, ContributionSection, RelayHintsParser, DiscoverySection) |
| App launch (isolated datadir) | Alive >5s, SIGTERM-exits cleanly |

## Pending human verification

- Cmd+K opens dashboard
- DISCOVERY section visible as 5th section (below CONTRIBUTION). Likely empty on a node with no inbound relay-hint gossip.
- Hover any sparkline → tooltip shows "5min avg / 1hr avg / 24hr avg" (1hr and 24hr will read 0 B/s for first hour of uptime)
- Hover the Decentralization Score number → rich tooltip with 7-row breakdown + "how to improve" hints
- Contribution stat grid now shows "Registrants active" (renamed from "Circuits active") and "Blocks served 24h" (real counter, was always 0 placeholder)
- Decentralization score's traffic term now reflects real 24h relay bytes (was 5min×288 extrapolation)

## Approximations REMOVED in this PR

- ✓ circuits_active = 0 placeholder → real registrants_count
- ✓ blocks_served_today = 0 placeholder → real blocks_served_24h
- ✓ bytes_relayed_24h 5min×288 extrapolation → real bytes_relayed_24h counter

## Approximations REMAINING

- peers_who_learned_via_gossip uses `received_relay` counter as proxy. Per-source-peer tracking in RelayHintRecord is parked behind a RELAY_HINTS wire change (out of scope for Phase 2b).
