# Shielded integration status — 2026-09-09

This is the entry point for the combined shielded work. It distinguishes code that exists from verification and activation. The integration worktree is `dinero-v8-shielded-integration`, branch `codex/shielded-integration`. Original contributor worktrees are preserved.

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

Integration verification is recorded below when complete.

The following are still open, regardless of successful integration tests:

1. Gate E dedicated binding-proof oracle/unit tests and end-to-end forged-snapshot mutation rejection evidence. The binding helpers currently have no direct test callers in this snapshot. Existing snapshot format-policy tests do not establish loader rejection of all forged v5 variants.
2. External-miner `getblocktemplate` DNRS support and coordinator TODO path completion before non-regtest state-commitment activation. The internal mining paths are implemented; fail-closed rejection alone does not make external mining ready.
3. Independent cryptographic/protocol review; target-device memory/latency qualification (the measured proof process peaks around 802 MB); physical hardware-wallet firmware support and validation. A host mock is not a device implementation.
4. Cross-platform/release CI, deployment compatibility plan, and separately reviewed activation decisions.
5. Known convergence investigations [#709](https://github.com/DineroLabs/dinero-v8/issues/709) and [#717](https://github.com/DineroLabs/dinero-v8/issues/717) remain separate. Passing a rerun does not close them.

This is an integrated development candidate, not a production activation or release sign-off.
