# Shielded integration status — 2026-09-09

This is the entry point for the combined shielded work. It distinguishes code that exists from verification and activation. Review and release work is tracked in [PR #720](https://github.com/DineroLabs/dinero-v8/pull/720); the existing-main epoch repair is isolated in [PR #719](https://github.com/DineroLabs/dinero-v8/pull/719). The integration worktree is `dinero-v8-shielded-integration`, branch `codex/shielded-integration`. Original contributor worktrees are preserved. The combined branch is now [published on GitHub](https://github.com/DineroLabs/dinero-v8/tree/codex/shielded-integration); it is not merged to main.

## Completion on the verified runtime

The four implementation priorities are complete on runtime
`ce100c9867c1c410c22e7147d1fb848e4e727a04`. The verification manifest below
records the exact source tree, local executable, CI merge tree, platform
artifacts, proof measurements and named-test coverage. A documentation-only
follow-up records these results; it does not replace the verified binaries.
All nine workflows also pass on test-only follow-up `25fe32473`, including
all 515 previously covered CI names and the expanded proof qualification.

1. The existing-main epoch-reset undo repair is merged in **PR #719**.
2. The combined candidate is built and verified across Linux, Windows and both
   macOS architectures. Named coverage accounts for all 575 locally registered
   tests: **574 executed and passed** across current-candidate CI, the local
   complement, strict IBD and ReleaseSuite. `CsnArchivalMainnetReplay` is an
   explicit external opt-in soak and was **not executed**. This is distributed
   coverage, not a claim of one full local sweep. ReleaseSuite ran the optional
   Phase 2 and restart/churn gates. `ReleaseIdentityFreeze` was not run
   (`RC_MODE=0`); no production RC is claimed.
3. Gate D has direct binding tests, six independent forged-snapshot mutations
   and five controls. DNRS-aware external mining and the supported coordinator
   are implemented and tested with real solved blocks and post-state equality.
4. The user-designated cryptographic/consensus review, hardware targets and
   compatibility/rollout plan are complete for this dormant development
   candidate. The reviewer also implemented fixes; this is not independent
   third-party assurance or production activation approval.

The 167,935-byte recipient-plus-change transfer now fits the bounded Auth relay
and consensus profile. The full recipient-authority relay, mining, restart,
reorg and recipient-spend lifecycle passes. The strengthened deep-fork scenario
also passes: all five nodes end at height 3022 with equal hash/work and no
missing bodies, after competing-fork adoption, return and a new block.

The additional deep-anchor, checkpoint rewind, dropped-header ownership and
stored-body quarantine repairs are in **PR #720**, not yet on main. These have
specific reproductions; they do not establish that #709/#717 share a cause.
Old lossy undo needs replay/reindex to reconstruct absent historical data.

Recommended qualification targets are **4 modern physical cores / 16 GiB RAM /
SSD for validators**, and **8 cores / 16 GiB for desktop proving wallets**
(**32 GiB** when sharing an archival node). The expanded 24-process
qualification passes on both hosts. Linux's slowest
measured eight-proof mix verifies in 23.261 s, with peak proof-process RSS
859,324,416 bytes. These results qualify the measured proof process, not every
minimum-spec machine or whole-node production load.

Still outside this completed implementation: actual fleet qualification and
canary deployment, pool migration or empty-pool evidence, burial-risk approval,
and a separately reviewed future activation-height commit. New mainnet/testnet
DNRS and Auth upgrades remain dormant. No release tag, production deployment
or network activation was performed.

See [final verification](SHIELDED_INTEGRATION_FINAL_VERIFICATION.json),
[named-test coverage](SHIELDED_INTEGRATION_FINAL_COVERAGE.json),
[review](SHIELDED_INTEGRATION_REVIEW.md),
[hardware profile](../specs/shielded_hardware_profile.md), and
[rollout plan](../specs/shielded_upgrade_rollout.md).

## What is already on GitHub

The fetched `origin/dinero-main` is `adb9643477e6ba2190a56ccf141bd6cca2621101`, including the independently landed epoch-undo repair in PR #719. Its 468-test main suite, 22 serial e2e tests, durability and remaining required checks passed before merge.

| Work | GitHub evidence |
|---|---|
| Shielded state commitment root definition and validation | [Merged PR #691](https://github.com/DineroLabs/dinero-v8/pull/691) |
| Shield/unshield UI safety | [Merged PR #699](https://github.com/DineroLabs/dinero-v8/pull/699) |
| Startup RPC status | [Merged PR #701](https://github.com/DineroLabs/dinero-v8/pull/701) |
| Mainnet-scale replay/root evidence | [Merged PR #713](https://github.com/DineroLabs/dinero-v8/pull/713) |
| Independent shielded protocol vectors | [Merged PR #714](https://github.com/DineroLabs/dinero-v8/pull/714) |
| Outgoing-view specification and vectors | [Merged PR #715](https://github.com/DineroLabs/dinero-v8/pull/715) |

The old outgoing-view branch has three commits that differ from the squash commit by patch identity, but all four changed specification/oracle/workflow/vector files match main exactly. It must not be merged again as a separate implementation. The independent-vector commit is patch-equivalent to main.

## What this branch combines

| Component | Preserved source | What exists |
|---|---|---|
| Recipient authority, outgoing recovery, hardware host boundary, resource policy | Snapshot `d1c4df999914e74f7dd40899ba88eca9cdceb400` of 74 changed/new files from `dinero-v8-recipient-authority` at base `be80b4b7e` | Recipient spend/view separation; authenticated commitments; outgoing v3 envelopes and persistence; locked discovery/unlock hydration; wallet RPCs; host capability checks; bounded v6 transaction relay and consensus resources |
| State-commitment activation gates | Eight original commits through `0e80be4af` from `feat/state-commitment-gates` | DNRS coinbase enforcement in live validation and reindex; three internal mining paths; snapshot v5 writer/loader, merkle binding and burial checks; replay comparison; fixture triage |
| Integration | Merge `06582fa89` plus follow-up review | Both enforcement families coexist; machine-specific absolute OpenSSL symlink removed |

The original recipient-authority worktree still shows uncommitted files intentionally: its bytes were committed through an independent Git index, without changing another task's index or working files. The snapshot commit and integration branch preserve them. Gate E's original eight commits are retained in merge history.

## What is active

| Rules | Mainnet | Testnet | Regtest |
|---|---|---|---|
| Existing shielded pool | Height 8650 | Dormant | Height 0 |
| Existing input binding | Height 32300 | See chain parameters | Height 0 |
| Existing value binding / shielded epoch reset | Height 61000 | See chain parameters | Dormant unless explicitly overridden |
| New recipient spend authority, outgoing recovery and Auth resource profile | Dormant | Dormant | Dormant by default; lifecycle explicitly activates |
| New DNRS state commitment / snapshot binding | Dormant | Dormant | Height 1 |

“Activation remains disabled” in the recipient-authority report refers to the new upgrade, not the already deployed shielded pool. No production activation height is selected by this integration.


## Retained earlier evidence

Earlier component results and 572/574-test inventories remain in the original
[verification manifest](SHIELDED_INTEGRATION_VERIFICATION.json). They are not
substituted for the final runtime's named coverage above. The first full sweeps
and release meta run found real recovery defects and test-fixture errors; the
[review](SHIELDED_INTEGRATION_REVIEW.md) records their controls and disposition.
The earlier Linux proof run that failed the provisional 20-second budget is
retained under its original failed result. The current 30-second profile and
its engineering rationale are explicit in the hardware document.
