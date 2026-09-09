# Shielded integration status — 2026-09-09

This is the entry point for the combined shielded work. It distinguishes code that exists from verification and activation. Review and release work is tracked in [PR #720](https://github.com/DineroLabs/dinero-v8/pull/720); the existing-main epoch repair is isolated in [PR #719](https://github.com/DineroLabs/dinero-v8/pull/719). The integration worktree is `dinero-v8-shielded-integration`, branch `codex/shielded-integration`. Original contributor worktrees are preserved. The combined branch is now [published on GitHub](https://github.com/DineroLabs/dinero-v8/tree/codex/shielded-integration); it is not merged to main.

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

## Verification and remaining work

Before integration, the recipient-authority work passed its Release two-node relay/restart/reorg/recipient-spend lifecycle; 44 validation, 10 outgoing, and 15 prover-kit tests; and focused resource/reindex/P2P tests. Five measured real transaction shapes span 46,373–394,953 bytes. These are component results, not proof of the merged tree. See [resource profile](../specs/shielded_auth_resource_profile.md) and [authority review](SHIELDED_RECIPIENT_AUTHORITY_IMPLEMENTATION_REVIEW.md).

Gate E's local CTest files show the AssumeUtxoReplay failure followed by a passing rerun. The supplied reports and rolling local log files do not establish one final all-green sweep for the exact combined tree.

The complete Release all-target build passes. All 44 focused CTest suites pass on the fixed build (52.83 seconds). The outgoing-recovery lifecycle passes (42.65 seconds), and the previously failing epoch-reset boundary passes (56.02 seconds), including persisted undo after restart, cross-reset reconnect and reindex. The two-node Auth relay/mining/restart/reorg/final-recipient-spend lifecycle passes (123.96 seconds). All three daemon lifecycles pass. These are 44 focused tests plus three lifecycle tests, not a claim that all 572 registered tests were executed. This is a local macOS Release development build with system OpenSSL, not a packaged release qualification. Exact source and daemon identity and test names are recorded in [the verification manifest](SHIELDED_INTEGRATION_VERIFICATION.json). The initial Release configuration registered 572 tests; Gate D adds two, bringing that inventory to 574. StateCommitmentMining subsequently brings the current inventory to 575. Source inspection confirms that both Auth resource checks and DNRS checks survived in live block validation and reindex. All six overlapping files were reviewed: chain parameters/header, block validation, reindex, daemon options and test CMake registration. The first clean all-target build exposed missing `gtest_main` dependencies in standalone test targets using archive-path links. The integration fixes all seven instances of that pattern, including the shielded replay and delta-parity targets. This is a pre-existing build-order defect exposed by the fresh build, not a consensus failure. The all-target build also exposed a new outgoing-recovery library-boundary defect: standalone shielded pool/adversarial binaries could not resolve the envelope helpers. Those pure helpers now live in `dinero_shielded` beside `shielded_wallet_ops`, rather than only in `dinero_wallet`, eliminating the missing dependency without duplicate implementations. An inventory of all registered worktrees found no other uncommitted files with shielded/snapshot/state-commitment names outside the captured recipient-authority worktree.

The current completion ledger (runtime changes through `937819df7ea62a0fd459b21cfaa7a3b07dc45338`):

1. **Gate D:** six independent forged-snapshot mutations and five controls are implemented and locally verified. The staged-import fix prevents rejected snapshots from publishing live state. Review added explicit coinbase-type validation; its regression failed before the change and all seven binding-helper unit tests now pass. See [Gate D evidence](SHIELDED_GATE_D_VERIFICATION.md).
2. **Mining:** canonical `getblocktemplate` DNRS metadata and immutable template rules are implemented. The supported `mining.getjob`/`mining.submit` coordinator shares the canonical assembler. The reported TODO belonged to an uncompiled obsolete prototype, now retired. `StateCommitmentMining` solves actual external/coordinator blocks, compares post-state roots, tests restart and dormancy, and passes locally.
3. **Review and hardware:** the user designated Codex as cryptographic/consensus reviewer. Its [implementation-agent review](SHIELDED_INTEGRATION_REVIEW.md) found and fixed the unconstrained Auth address-domain witness, missing snapshot coinbase-type check and incomplete activation checksum telemetry. This is not independent third-party review. The [hardware profile](../specs/shielded_hardware_profile.md) defines measured acceptance budgets and provisional minimum-host targets. Eighteen fresh-process proof measurements passed locally, including maximum eight-proof blocks. Physical hardware wallets and mobile proving remain outside the supported initial scope.
4. **Verification and rollout:** the [compatibility/rollout plan](../specs/shielded_upgrade_rollout.md) is implemented as a concrete release checklist. Linux, Windows and both macOS artifact builds passed on preceding candidate `a7c23d3e7`; all are rerunning on the deeper undo repair. The preceding Tests workflow failed its assertion ratchet and promotion fixture; its main CTest step was skipped, so it does not supply an all-green main lane. The eleven new relay checks now use always-on failures, and the dormant promotion fixture passes all cases locally after its missing override and macOS log-count command were repaired. Candidate CI is running; no production activation or release publication has occurred. Production fleet measurements, pool migration/empty-pool evidence, burial-risk decisions and future activation heights remain explicit pre-activation requirements.
5. **Full-sweep triage:** the preliminary 574-test run exposed pre-existing harness defects, fractional fee truncation and a missing mock-service dormancy setting; fixes have passing targeted controls. The strict IBD convergence failure also reproduces on separately built main plus only the epoch-undo fix. Logs are preserved under `shielded-integration-evidence/ibd-baseline-control`; the repeated full-batch continuation was isolated with a red-before/green-after side-branch regression and repaired. The original strict scenario passed through the post-heal block. A stronger scenario now requires all consumers to adopt the competing fork before healing, and exposed a second, ordinary deep-anchor undo defect. The repair restores per-block anchor snapshots; its first 2,221-block fork adoption passes. The return-fork run is pending after correcting the harness to observe ongoing body downloads while the canonical tip remains held. Success now requires P2P convergence without manual replay. This does not resolve #709/#717 by attribution. The release meta-gate requires an explicitly identified baseline binary; a separately built baseline is now available. Final results are pending.
6. **Existing convergence investigations remain separate:** [#717](https://github.com/DineroLabs/dinero-v8/issues/717) is open; [#709](https://github.com/DineroLabs/dinero-v8/issues/709) was marked closed, but its latest inspected comment explicitly rejects treating post-#712 green runs as proof of resolution. Passing a rerun does not establish a fix.

This is an integrated development candidate, not a production activation or release sign-off.

## Reconnect/root mismatch investigation

Claude's report is valid, not a resolved old finding. On the combined tree before the fix, `ShieldedEpochResetBoundary` reproduces a rejection at height 113 after invalidating below the reset at 115 and reconsidering. The committed root differs from the recomputed root; the diagnostic reports `anchors_bytes=3572` (99 entries rather than the 100-entry window).

A minimal unit test reproduces this without mining, proof generation or peer timing: populate beyond the anchor-window depth, capture/reset/restore, roll back two more heights and reconnect one. The restored history differs from an independent never-reset history. `CaptureShieldedEpoch` stored only `SerializeBytes()` (active window); the eviction journal was dropped. Crossing the reset restored the visible window but could not refill it on further rollback. The same capture/restore code exists on `origin/dinero-main`: Gate E exposes an existing state-restoration defect through newly enforced DNRS equality.

Fix commit `9b32673b3` captures the existing full persistence envelope and restores through its backward-compatible reader. Consensus root serialization remains the original active-window representation. A second test pins legacy-v1 undo readability. Both pass in the 10-test epoch-reset unit suite; the regression was demonstrated red before the fix. The boundary lifecycle now restarts before disconnect to exercise persisted undo. Old records that never stored the eviction journal cannot recover that absent information merely by upgrading; regenerating such historical undo through replay/reindex is a separate operational prerequisite if those deep cross-reset rollbacks must be supported. The fix protects newly captured undo; it does not claim retroactive repair of already lossy records.

The historical green report at `8f2604c10` is superseded for main by the verified PR #719 merge. It does not establish that convergence investigations #709/#717 are resolved. This evidence concerns main, not Gate E or this integration. The report that no gates branch was pushed is superseded by the published integration branch, which contains the eight Gate E commits.


A second root-restoration defect was found by the stronger scenario after the
epoch repair landed. Ordinary undo depended on a bounded 100-entry eviction
journal; deep rollback exhausted it and reconnecting at height 601 produced a
different DNRS root. Commit `5720355c4` stores/restores pre-block anchor envelopes
in both undo formats and across live, stateless and reindex paths, including
empty blocks. Its minimal deep regression was red before the repair; focused
codec, rejection-atomicity and reindex checks pass. The reviewed repair is in
PR #720; it is not yet on main. See the review and rollout plan for historical
undo regeneration and storage costs. Final full-tree verification is pending.

The frozen predecessor also exposed missing anchor rewind during contaminated
Utreexo checkpoint recovery. Commit `937819df7` restores anchors alongside the
existing frontier/nullifier rewind. The DNRS-active regression now compares the
full shielded root and passes offline recovery plus a second offline restart.
This is a separate concrete recovery defect, not attributed to #709/#717.
