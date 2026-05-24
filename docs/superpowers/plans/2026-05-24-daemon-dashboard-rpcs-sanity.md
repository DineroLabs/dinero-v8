# Daemon Dashboard RPCs — Sanity Log

**Date:** 2026-05-24T21:15:57Z
**Branch:** `feature/daemon-dashboard-rpcs`

## Automated results (this session)

| Check | Result |
|---|---|
| Full `dinerod` build | `[100%] Built target dinerod` |
| ctest `DashboardRpcsSmoke` (2-node regtest) | PASS — 3 assertions (node_id_hex 40-char hex + per-peer ping_ms/quality_score + dynamic_p2p.observe shape) |

## Manual canary on the live daemon — DEFERRED

The plan's Task 8 Steps 3-5 (back up the running `dinerod` binary, swap in the new one, query the live daemon, restore) is deferred to the user. Reason: the user's running daemon at `build-rc14-quic/dinerod` powers the live dinero-qt session including their Cmd+K dashboard testing. Swapping binaries would briefly disconnect them — explicit user-initiated action is more appropriate than an automated swap.

To run the manual canary when ready:

```bash
# Back up the current binary
cp /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod \
   /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod.pre-dashboard-rpcs-$(date +%s)

# Swap in the new one
cp /private/tmp/dinero-v8-daemon-rpcs-impl/build-daemon/dinerod \
   /Users/haydarevich/src/dinero-v8/build-rc14-quic/dinerod

# Graceful restart via RPC stop (never hard-kill)
/Applications/dinero-qt.app/Contents/Resources/dinero-cli \
    -datadir="$HOME/Library/Application Support/Dinero" -rpcport=20998 stop
sleep 5
# ...relaunch via your usual command line...

# Verify the 3 new RPCs return real data
CLI="/Applications/dinero-qt.app/Contents/Resources/dinero-cli"
$CLI getnetworkinfo | grep node_id_hex
$CLI getpeerinfo | python3 -c "import sys,json; p=json.load(sys.stdin)[0]; print(p.get('ping_ms'), p.get('quality_score'))"
$CLI dynamic_p2p.observe | python3 -m json.tool | head -15
```

After PR #N (this PR) lands and merges, the dinero-qt dashboard side (PR #139 follow-up) can wire these RPCs into the Cmd+K panel to fill the Q score and ping columns.
