# Shielded integration status — 2026-09-09

This is the entry point for the combined shielded work. It distinguishes code that exists from verification and activation. The integration worktree is `dinero-v8-shielded-integration`, branch `codex/shielded-integration`. Original contributor worktrees are preserved. The combined branch is now [published on GitHub](https://github.com/DineroLabs/dinero-v8/tree/codex/shielded-integration); it is not merged to main.

## What is already on GitHub

The fetched `origin/dinero-main` is `8f2604c10764bb71b8e2d1424d48abe8f72deb70`.

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

The complete Release all-target build passes. All 44 focused CTest suites pass on the fixed build (52.83 seconds). The outgoing-recovery lifecycle passes (42.65 seconds), and the previously failing epoch-reset boundary passes (56.02 seconds), including persisted undo after restart, cross-reset reconnect and reindex. The two-node Auth relay/mining/restart/reorg/final-recipient-spend lifecycle passes (123.96 seconds). All three daemon lifecycles pass. These are 44 focused tests plus three lifecycle tests, not a claim that all 572 registered tests were executed. This is a local macOS Release development build with system OpenSSL, not a packaged release qualification. Exact source and daemon identity and test names are recorded in [the verification manifest](SHIELDED_INTEGRATION_VERIFICATION.json). The initial Release configuration registered 572 tests; Gate D adds two, bringing the current total to 574. Source inspection confirms that both Auth resource checks and DNRS checks survived in live block validation and reindex. All six overlapping files were reviewed: chain parameters/header, block validation, reindex, daemon options and test CMake registration. The first clean all-target build exposed missing `gtest_main` dependencies in standalone test targets using archive-path links. The integration fixes all seven instances of that pattern, including the shielded replay and delta-parity targets. This is a pre-existing build-order defect exposed by the fresh build, not a consensus failure. The all-target build also exposed a new outgoing-recovery library-boundary defect: standalone shielded pool/adversarial binaries could not resolve the envelope helpers. Those pure helpers now live in `dinero_shielded` beside `shielded_wallet_ops`, rather than only in `dinero_wallet`, eliminating the missing dependency without duplicate implementations. An inventory of all registered worktrees found no other uncommitted files with shielded/snapshot/state-commitment names outside the captured recipient-authority worktree.

The remaining-gate ledger (updated after Gate D verification):

1. **Gate D is now verified locally:** direct binding-helper tests and all six independent forged-snapshot mutations pass, with five additional controls. The tests exposed and fixed binding rejection occurring after state import. See [Gate D evidence](SHIELDED_GATE_D_VERIFICATION.md), code commit `3e3349696`. This closes the missing local test evidence, not independent review or cross-platform qualification.
2. External-miner `getblocktemplate` DNRS support and coordinator TODO path completion before non-regtest state-commitment activation. The internal mining paths are implemented; fail-closed rejection alone does not make external mining ready.
3. Independent cryptographic/protocol review; target-device memory/latency qualification (the measured proof process peaks around 802 MB); physical hardware-wallet firmware support and validation. A host mock is not a device implementation.
4. Cross-platform/release CI, deployment compatibility plan, and separately reviewed activation decisions.
5. Convergence investigations remain separate: [#717](https://github.com/DineroLabs/dinero-v8/issues/717) is open; [#709](https://github.com/DineroLabs/dinero-v8/issues/709) is marked closed on GitHub, but its latest comment explicitly rejects treating post-#712 green runs as proof of resolution. No root-cause resolution was found in those comments. Passing a rerun does not establish a fix.

This is an integrated development candidate, not a production activation or release sign-off.

## Reconnect/root mismatch investigation

Claude's report is valid, not a resolved old finding. On the combined tree before the fix, `ShieldedEpochResetBoundary` reproduces a rejection at height 113 after invalidating below the reset at 115 and reconsidering. The committed root differs from the recomputed root; the diagnostic reports `anchors_bytes=3572` (99 entries rather than the 100-entry window).

A minimal unit test reproduces this without mining, proof generation or peer timing: populate beyond the anchor-window depth, capture/reset/restore, roll back two more heights and reconnect one. The restored history differs from an independent never-reset history. `CaptureShieldedEpoch` stored only `SerializeBytes()` (active window); the eviction journal was dropped. Crossing the reset restored the visible window but could not refill it on further rollback. The same capture/restore code exists on `origin/dinero-main`: Gate E exposes an existing state-restoration defect through newly enforced DNRS equality.

Fix commit `9b32673b3` captures the existing full persistence envelope and restores through its backward-compatible reader. Consensus root serialization remains the original active-window representation. A second test pins legacy-v1 undo readability. Both pass in the 10-test epoch-reset unit suite; the regression was demonstrated red before the fix. The boundary lifecycle now restarts before disconnect to exercise persisted undo. Old records that never stored the eviction journal cannot recover that absent information merely by upgrading; regenerating such historical undo through replay/reindex is a separate operational prerequisite if those deep cross-reset rollbacks must be supported. The fix protects newly captured undo; it does not claim retroactive repair of already lossy records.

Current GitHub checks confirm all three main workflows green at `8f2604c10`; #707, #717 and #718 remain open. This evidence concerns main, not Gate E or this integration. The report that no gates branch was pushed is superseded by the published integration branch, which contains the eight Gate E commits.
