# Utreexo Status

Dinero's Utreexo compact-state node now replays archived chain history to exact bridge parity, with proof-serving, restart, reorg, stale-proof recovery, and live-spend sync all validated.

## Freeze Points

- Backend freeze commit: `b78d10340662c814742efa09fe242e9c40437917`
- App freeze commit: `ed60aeea9b52bf89f52b25e6e17aa93e8c615749`
- Release tag: `v1.0.0-Utreexo`

## Verified Claims

- A fresh CSN replayed archived chain history from genesis to live bridge tip.
- Final CSN tip hash matched both archival bridges exactly:
  - `000000006b341c0fa6846b24b6a8a1376b31ff1ee512b7848a9af507acd365c8`
- Final CSN stump commitment matched both archival bridges exactly:
  - `91386e3bb75b6cc763001b0483d0a83f30cd5c2f28d69d88d872d44bae99c370`
- The archival replay completed without `NOTFOUND`, `missing-utreexo-data`, or proof validation failures.
- Restart during sync, restart after tip, and checkpoint restore were validated.
- Reorg handling under stump state was validated.
- Stale-proof rejection and immediate proof re-fetch were validated.
- Bridge-assisted spend flow from the CSN path was validated.
- Fresh CSN sync while live spend traffic was being produced on the network was validated.

## Validation Gates

- `tests/integration/test_csn_archival_mainnet_replay.sh`
- `tests/integration/test_csn_restart_resume_checkpoint.sh`
- `tests/integration/test_csn_reorg_churn.sh`
- `tests/integration/test_csn_bridge_assisted_spend_flow.sh`
- `tests/integration/test_csn_spend_reorg_reconciliation.sh`
- `tests/integration/test_csn_sync_live_spend_traffic.sh`
- `tests/integration/test_bridge_csn_historical_range_soak.sh`
- `tests/network/test_csn_stale_proof_retry.cpp`
- `tests/network/test_transition_only_proof_mode.cpp`
- `tests/network/test_transition_proof_forest_sync.cpp`

## Recovery Guidance

- Older CSN state created before the stump-backed architecture fix is not trusted by default.
- If a node has pre-fix or otherwise contaminated CSN state, wipe the local CSN chain state and resync unless exact parity has been independently proven.
- The canonical Utreexo state for a CSN is the confirmed-chain stump only; mempool or cached proof state must not be treated as authoritative.
