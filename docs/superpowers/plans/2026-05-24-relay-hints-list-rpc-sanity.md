# Relay Hints List RPC — Sanity Log

**Date:** 2026-05-25T03:07:05Z
**Branch:** `feature/relay-hints-list-rpc`

## Results

| Check | Result |
|---|---|
| Full `dinerod` build | `[100%] Built target dinerod` |
| New ctest: `RollingCounter24h` (5 internal cases) | PASS |
| New ctest: `RelayHintsListRpc` (2-node regtest) | PASS |
| Adjacent existing ctest: `DashboardRpcsSmoke` (PR #140 baseline) | PASS |
| Standalone CLI: `relay_hints.list` on empty cache | Returns documented shape (ttl=900, max=3, targets=[]) |
| Standalone CLI: `getnetworkinfo.relay` | Both new fields present (blocks_served_24h, bytes_relayed_24h) on relay-on and relay-off nodes |

## Known non-blocker

- 7 `Shielded*` integration tests fail with `dinerod not built at /build/dinerod` — pre-existing path hard-coding issue unrelated to this PR (tests don't use the DINEROD env injection rule). Affects any non-default build dir.

## Phase 2b qt PR follow-up

The next PR (qt-dashboard-phase2b) consumes:
- `relay_hints.list` for the new DiscoverySection (per-target hint cache rows + freshness bar + stoplight glyph + fade-out on evict)
- `getnetworkinfo.relay.blocks_served_24h` for the "Blocks served today" stat (replaces Phase 2a's 0 placeholder)
- `getnetworkinfo.relay.bytes_relayed_24h` for the Decentralization-Score traffic term (replaces Phase 2a's 5min×288 extrapolation)
- Real 24h relay-traffic counter eliminates the documented "honest approximation" in Phase 2a's score formula
