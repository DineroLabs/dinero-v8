# Remaining Test Label Classification

Originally generated from `origin/dinero-main` at `25e6f55d` after PR #65.
Updated after PR #71 to reflect the packaging, RPC, mempool/rawtx, and shutdown smoke
graduations.

This document classifies the remaining label-based exclusions in
`.github/workflows/tests.yml`:

```bash
ctest --test-dir build-tests \
  --output-on-failure \
  --label-exclude 'integration|gate|release|canonicality|fuzz'
```

The exact-name quarantine is gone. The remaining work is label taxonomy:
deciding which excluded tests are cheap enough for normal PR CI, which need
fixtures or harness repair, and which belong in release, nightly, fuzz, or
manual ownership.

## Evidence

Local inventory command:

```bash
ctest --test-dir build-cmake-tests --show-only=json-v1
```

Current inventory:

| Metric | Count |
| --- | ---: |
| Registered CTest tests | 377 |
| Selected by current Test Workflow v2 label filter | 299 |
| Excluded by at least one remaining label | 78 |

Per-label counts below are label memberships, not disjoint sets. Several tests
carry more than one excluded label, for example `AcceptanceParity` is both
`gate` and `release`, and many `canonicality` tests are also `integration`.

| Label | Count | Classification | Recommended disposition |
| --- | ---: | --- | --- |
| `canonicality` | 26 | Mixed correctness signal. Contains stale shielded helper-binary tests and heavier restart/reindex/reorg equivalence tests. | Continue splitting deliberately; keep restart/reindex cohorts quarantined until harness ownership is clear. |
| `packaging` | 0 | Retired from the current label quarantine. The three former members graduated into active `packaging-smoke` coverage. | Keep future package-build or distro-output tests under a different label such as `packaging-gate` or a dedicated packaging workflow. |
| `integration` | 70 | Broad runtime harness surface. Contains daemon, wallet, RPC, P2P, Utreexo, CSN, reindex, shielded, mining, and restart tests. | Do not graduate as one label. Split by subsystem and fixture needs. |
| `release` | 3 | Release-readiness ownership. | Keep full release gates out of normal PR CI; preserve or create smoke equivalents where useful. |
| `gate` | 4 | Readiness/blocking gates. | Classify each gate individually as PR-safe, nightly, release-only, or manual. |
| `fuzz` | 2 | Fuzzer-style coverage. | Move to dedicated fuzz/nightly workflow rather than normal PR `ctest`. |

## Important Correction

Removing exact-name quarantines does not mean every formerly fixed test is
currently active in Test Workflow v2. Some resolved tests can still carry an
excluded label. Before PR #67, `ShieldedDerivation` was no longer exact-name
quarantined but was still excluded by broad `canonicality`. PR #67 graduated
`ShieldedDerivation` and `ShieldedReindexEquivalence` into
`shielded-canonical-smoke`.

This is why the next phase must split labels deliberately instead of declaring
all fixed tests active.

## Label: `canonicality`

Tests:

```text
ConnectTipRestartEquivalence
FrontierWriteGapRecovery
FrontierWriteGapRecovery_AtomicPersistOn
HeaderBacklogRestartEquivalence
HeaderCFRestartEquivalence
HeaderFilterReplayEquivalence
InterruptedReorgFailSafe
InvalidityCrashRestartEquivalence
InvalidityImportEquivalence
InvalidityRestartSticky
PositionIndexRestartEquivalence
ReconsiderCrashRestartEquivalence
RecoveryMarkerRestartEquivalence
ReindexChainstateUtreexoEquivalence
ReindexPromotionRestartEquivalence
ReorgMarkerAlignedRestartEquivalence
ShieldedAdversarialHardening
ShieldedDaemonRestartEquivalence
ShieldedPoolRoundTrip
ShieldedReorgDisconnectRestartEquivalence
ShieldedReorgSecondRestartInvalidityEquivalence
ShieldedTipMarkerRestartEquivalence
ShieldedTipPersistRestartEquivalence
TipPersistRestartEquivalence
UnifiedBatchAtomicity
UnifiedBatchAtomicity_AtomicPersistOn
```

Read:

- This is the highest-value label to split first because it is a correctness
  signal, not just a harness bucket.
- It is also too broad to graduate whole. It mixes stale shielded helper-binary
  tests with restart/reindex/reorg equivalence tests that may need datadir
  lifecycle control.
- Two non-integration shielded canonicality tests remain:
  `ShieldedAdversarialHardening` and `ShieldedPoolRoundTrip`. Both are deferred
  because their helper binaries do not currently compile against the current
  shielded API.

Recommended next action:

1. Fix or retire the stale `tools/pq_bench` shielded helper binaries before
   trying to graduate `ShieldedPoolRoundTrip` or
   `ShieldedAdversarialHardening`.
2. Leave restart/reindex/reorg equivalence tests quarantined until their fixture
   and runtime profile is documented.

## Label: `packaging`

Former tests:

```text
CmakeInstallLayout
DineroBackup
DineroPrepareUpgrade
```

Read:

- PR #68 focus-ran all three tests. The whole bucket passed locally in about one
  second, and none required an external service or package builder.
- The tests exercise install layout, backup archive policy, and upgrade rollback
  capture as smoke/property checks, not full distro packaging.

Disposition:

1. Relabeled the three tests from broad `packaging` into active
   `packaging-smoke`.
2. Retired `packaging` from the current exclusion list in
   `.github/workflows/tests.yml`.
3. Future heavyweight package-build checks should use a new label such as
   `packaging-gate` or live in a dedicated packaging workflow.

## Label: `integration`

Tests:

```text
AddressParentChildMempoolLifecycle
BridgeCsnHistoricalRangeSoak
BridgeCsnHistoricalSpendRelay
ChainIdentitySync
CompactBlockRelayE2E
ConnectTipRestartEquivalence
ConsensusWriteBatchLeak
CovenantScriptPath
CrossSeedRestartReorgParity
CsnArchivalMainnetReplay
CsnBridgeAssistedSpendFlow
CsnContaminatedCheckpointRecovery
CsnSpendReorgReconciliation
CsnSyncLiveSpendTraffic
D3FullUserTxMissingUndo
DpiHeaderFilterProofFlow
EscapeHatchTests
ExternalMinerCoinbaseTxnFilterCommitment
FilterCommitmentActivationBoundary
FrontierWriteGapRecovery
FrontierWriteGapRecovery_AtomicPersistOn
HeaderBacklogRestartEquivalence
HeaderCFRestartEquivalence
HeaderFilterReplayEquivalence
InterruptedReorgFailSafe
InvalidityCrashRestartEquivalence
InvalidityImportEquivalence
InvalidityRestartSticky
LaggingPeerCatchupNoOrphans
Mining_PoolPayoutIntegration
Mining_W1_5_Integration
P2PManager_TS1_Integration
P2PServiceNetworkControl
ParallelBlockDownloadTargeted
ParentChildRbfReplacementWithMempoolParent
PeerMetadataRuntimeIdentity
PositionIndexRestartEquivalence
RebuildUndoRange
RebuildUndoRangeCrashOracles
ReconsiderCrashRestartEquivalence
RecoveryMarkerRestartEquivalence
ReindexChainstateUtreexoEquivalence
ReindexCopiedDatadir
ReindexCopiedDatadir_AtomicPersistOn
ReindexLegacyV2ForestFixture
ReindexPromotionRestartEquivalence
ReorgMarkerAlignedRestartEquivalence
RestartChurnBoringnessGate
ShieldedDaemonRestartEquivalence
ShieldedReorgDisconnectRestartEquivalence
ShieldedReorgInvertibility
ShieldedReorgInvertibility_AtomicPersistOn
ShieldedReorgInvertibility_AtomicPersistToggleOffToOn
ShieldedReorgInvertibility_AtomicPersistToggleOnToOff
ShieldedReorgSecondRestartInvalidityEquivalence
ShieldedRpcGetAddress
ShieldedRpcShieldEndToEnd
ShieldedRpcTransferAddressedDetectEndToEnd
ShieldedRpcTransferAddressedEndToEnd
ShieldedRpcTransferEndToEnd
ShieldedRpcTransferMultiEndToEnd
ShieldedRpcUnshieldEndToEnd
ShieldedTipMarkerRestartEquivalence
ShieldedTipPersistRestartEquivalence
TipPersistRestartEquivalence
UndoMetadataRestamp
UnifiedBatchAtomicity
UnifiedBatchAtomicity_AtomicPersistOn
UtreexoMempoolCanonicalSeparation
Wallet_W2_6_SyncIntegration
```

Read:

- `integration` is not a single category. It is a catch-all label for tests that
  cross process, database, RPC, wallet, P2P, chainstate, or restart boundaries.
- Graduating the entire label would likely make PR CI unstable and slow.
- Several integration tests overlap with `canonicality`; those should be handled
  under the canonicality split first so the correctness signal is preserved.

Recommended sub-buckets:

| Sub-bucket | Examples | Recommended owner |
| --- | --- | --- |
| Address/RPC/wallet smoke | `AddressIndexCrossHRP`, `WalletListUnspentExcludesMempoolSpent`, `WsCookiePathResolution` | Graduated in PR #69 as active `rpc-smoke` coverage after three focused local runs. |
| Mempool/rawtx smoke | `AddressBalanceMempoolOverlay`, `TaprootSignRawTransactionRbfOverlay`, `TaprootSignRawTransactionRbfConfirmation`, `WalletCreateRawTransactionScriptPubKeyOutputs`, `RbfPolicyReporting` | Graduated in PR #70 as active `mempool-rawtx-smoke` coverage after three focused local runs. Parent-child variants remain quarantined because they fail runtime mempool-clearing expectations. |
| RPC shutdown smoke | `RpcStopCleanShutdown` | Graduated in PR #71 as active `rpc-shutdown-smoke` coverage after repairing the hardcoded `build/dinerod` harness path and passing three focused local runs. |
| P2P/network runtime | `CompactBlockRelayE2E`, `ParallelBlockDownloadTargeted`, `LaggingPeerCatchupNoOrphans` | Nightly or dedicated network harness first. |
| CSN/Utreexo bridge | `BridgeCsnHistoricalSpendRelay`, `CsnBridgeAssistedSpendFlow`, `CsnSyncLiveSpendTraffic` | Dedicated Utreexo/CSN workflow; some may need external or long-running fixtures. |
| Canonical restart/reindex | `ConnectTipRestartEquivalence`, `ReindexPromotionRestartEquivalence`, `UnifiedBatchAtomicity` | Keep with canonicality plan. |
| Shielded RPC/end-to-end | `ShieldedRpcShieldEndToEnd`, `ShieldedRpcTransferMultiEndToEnd` | Dedicated shielded integration lane or staged smoke split. |
| Mining/pool integration | `Mining_W1_5_Integration`, `Mining_PoolPayoutIntegration` | Mining workflow or nightly lane. |

## Labels: `release` and `gate`

Tests:

```text
AcceptanceParity
IBDTorture
ReleaseSuite
Utreexo_SpendPathValidation_U3
```

Read:

- `AcceptanceParity`, `IBDTorture`, and `ReleaseSuite` carry release/gate
  semantics and should not quietly become normal PR-CI tests.
- `Utreexo_SpendPathValidation_U3` is a critical Utreexo spend-path gate. It may
  be important enough for CI, but should be assessed on runtime and determinism
  before graduating.

Recommended next action:

1. Keep `ReleaseSuite` and `IBDTorture` release/nightly/manual unless split into
   explicit smoke variants.
2. Inspect `AcceptanceParity` separately; it may become a cheaper release-smoke
   check if runtime and fixtures are stable.
3. Inspect `Utreexo_SpendPathValidation_U3` separately as a consensus/Utreexo
   critical gate, not as part of the release bucket.

## Label: `fuzz`

Tests:

```text
ConsensusFuzzer
ConsensusUTXOSetFuzz
```

Read:

- These are fuzz/property-style tests. They are valuable but should not be
  folded into normal PR `ctest` unless they have deterministic bounded modes.

Recommended next action:

1. Keep excluded from Test Workflow v2 for now.
2. Add a separate fuzz workflow or nightly job with explicit time budgets.
3. If the binaries already support deterministic smoke seeds, add separate
   smoke CTest entries with non-`fuzz` labels rather than weakening the fuzz
   jobs themselves.

## Recommended PR Sequence

1. PR #67: graduated `ShieldedDerivation` and `ShieldedReindexEquivalence` into
   `shielded-canonical-smoke`; deferred `ShieldedPoolRoundTrip` and
   `ShieldedAdversarialHardening` because their helper binaries are stale
   against current shielded APIs.
2. PR #68: graduated all three former `packaging` tests into
   `packaging-smoke` and removed `packaging` from the active workflow exclusion
   list.
3. PR #69: graduated `AddressIndexCrossHRP`,
   `WalletListUnspentExcludesMempoolSpent`, and `WsCookiePathResolution` from
   the broad `integration` label into active `rpc-smoke` coverage. The two
   shell tests now receive CTest's build-local `dinerod` path instead of
   assuming `build/dinerod`.
4. PR #70: graduated `AddressBalanceMempoolOverlay`,
   `TaprootSignRawTransactionRbfOverlay`,
   `TaprootSignRawTransactionRbfConfirmation`,
   `WalletCreateRawTransactionScriptPubKeyOutputs`, and `RbfPolicyReporting`
   into active `mempool-rawtx-smoke` coverage. The parent-child mempool tests
   remain excluded because they fail runtime mempool-clearing expectations.
5. PR #71: graduated `RpcStopCleanShutdown` into active `rpc-shutdown-smoke`
   coverage after repairing its hardcoded `build/dinerod` harness path and
   proving clean stop-RPC shutdown behavior in three focused local runs.
6. Later: design dedicated lanes for P2P/network, CSN/Utreexo, shielded E2E,
   release gates, and fuzz.

The rule from the named-test cleanup still applies: shrink the quarantine only
when a smaller ownership boundary proves itself. Do not chase a green badge by
moving broad labels wholesale.
