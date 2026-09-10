# Convergence investigation — 2026-09-10

Status: #709 and #717 remain unresolved. No convergence/consensus fix is
claimed, and no timeout or passing criterion was relaxed.

## Baseline and reproduction

Current main: 5c0458c77596673e2e2344d5fd851b39f31324c6.
Mac daemon rebuilt from the activation-hardening worktree; src/ and include/
match main. The worktree includes diagnostic-only harness changes and Qt/docs
changes. Mac binary SHA-256:
eae19e2a4c3302ce85bb1ba5b9f9b0737b2d590ff15b4f87c7c8f3dfed008d95.
Linux candidate runtime source: 8038aeba6fe1e9d82078672634c3311a6c8c8014;
its src/ and include/ also match current main. Previously qualified Linux
binary SHA-256:
a476051d6a4d8bbdc38cdb112df371bacc50d62760cd4365f9772f5c08f9b2b7.

| Scenario | Mac | Linux |
|---|---:|---:|
| CSN spend/reorg reconciliation, default 240-second limit | 16/16 pass | 10/10 pass |
| Dual-node mining, balanced and both asymmetric profiles | 15/15 pass | 3/3 pass |

Mining parameters matched the short churn gate: target heights 80/95/110,
miners 6/6, 6/3 and 3/6, settle 6 seconds, convergence timeout 120 seconds.
The mining component was exercised directly, not the entire three-stage gate.
Linux runs used temporary regtest data and transient systemd units, each capped
at two CPU equivalents and 4 GiB. Mac mining and CSN runs overlapped; Linux
series also overlapped. These are bounded scheduling-load samples, not a
recreation of every GitHub runner condition. All runs finished and cleanup ran.
Production services, wallets and network configuration were not modified.

Evidence: local shielded-integration-evidence/convergence-investigation/,
including per-run CSN logs, mining cycle logs and summary TSVs, Linux copies,
binary identity, and original CI attempt-1 log for run 34268193545.

## Findings and limits

- #709's mining "cycle 2" starts fresh nodes/data directories with the a-heavy
  profile. It does not reopen cycle 1's database. Investigating only persisted
  restart state would miss the actual scenario.
- The historical nodes were frozen on differing tips. Cumulative work and peer
  state are needed to distinguish a legitimate equal-work fork, disconnected
  peers, missing bodies, or stalled validation. Heights alone cannot order
  competing chains, and a higher-work reorg can lower height.
- Current mining harness already mines a tie-break block on equal-height forks.
  This investigation did not change that behavior.
- Original #717 CI output stops inside the bridge failure dump. The helper's
  early-closing truncation pipeline could abort cleanup under pipefail before
  CSN diagnostics printed. Existing local commit 11269a48c fixes that and adds
  pre-teardown CSN RPC captures plus CI upload coverage. Its failure-log
  regression passed 15/15 checks during this investigation.
- Added full getblockchaininfo, getpeerinfo and mining.getstatus capture for
  both mining nodes at convergence failure. Named convergence-state.log files
  survive cleanup and match the existing churn CI artifact glob. Shell syntax
  and diff checks passed. No cookies or wallet files are collected.
- Presence of #712 in a failing build demonstrates that it was not sufficient
  to prevent that failure; chronology alone does not establish or exclude its
  causal involvement. No causal attribution to #712 or #693 is made here.

## Next discriminating evidence

Do not close either issue on these passes. Land the diagnostic changes so the
next CI occurrence retains both nodes' state and logs. At a failure, compare
active chainwork with best-header work, then identify the first missing body or
proof, peer state, and any validation/reorg-plan rejection. Reduce that captured
ordering to a deterministic reproducer before altering consensus behavior.
The historical original failures lack sufficient retained state to establish
those mechanisms; this investigation did not reproduce them.
