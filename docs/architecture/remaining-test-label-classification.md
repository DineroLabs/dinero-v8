# Remaining Test Label Classification

Generated from `origin/dinero-main` at `25e6f55d` after PR #65.

This document classifies the remaining label-based exclusions in
`.github/workflows/tests.yml`:

```bash
ctest --test-dir build-tests \
  --output-on-failure \
  --label-exclude 'integration|gate|release|canonicality|fuzz|packaging'
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
| Selected by current Test Workflow v2 label filter | 285 |
| Excluded by at least one remaining label | 92 |

Per-label counts below are label memberships, not disjoint sets. Several tests
carry more than one excluded label, for example `AcceptanceParity` is both
`gate` and `release`, and many `canonicality` tests are also `integration`.

| Label | Count | Classification | Recommended disposition |
| --- | ---: | --- | --- |
| `canonicality` | 28 | Mixed correctness signal. Contains cheap shielded/vector-style tests and heavier restart/reindex/reorg equivalence tests. | Split first. Graduate cheap non-integration canonicality tests only after focused runs; keep restart/reindex cohorts quarantined until harness ownership is clear. |
| `packaging` | 3 | Packaging and local maintenance tooling. | Split smoke checks from real package/recovery gates. Candidate for the next small PR after focused validation. |
| `integration` | 79 | Broad runtime harness surface. Contains daemon, wallet, RPC, P2P, Utreexo, CSN, reindex, shielded, mining, and restart tests. | Do not graduate as one label. Split by subsystem and fixture needs. |
| `release` | 3 | Release-readiness ownership. | Keep full release gates out of normal PR CI; preserve or create smoke equivalents where useful. |
| `gate` | 4 | Readiness/blocking gates. | Classify each gate individually as PR-safe, nightly, release-only, or manual. |
| `fuzz` | 2 | Fuzzer-style coverage. | Move to dedicated fuzz/nightly workflow rather than normal PR `ctest`. |

## Important Correction

Removing exact-name quarantines does not mean every formerly fixed test is
currently active in Test Workflow v2. Some resolved tests still carry an excluded
label. Example: `ShieldedDerivation` is no longer exact-name quarantined, but it
still has the `canonicality` label and is therefore excluded by the current broad
label filter.

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
ShieldedDerivation
ShieldedPoolRoundTrip
ShieldedReindexEquivalence
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
- It is also too broad to graduate whole. It mixes likely cheap shielded
  canonical-vector checks with restart/reindex/reorg equivalence tests that may
  need datadir lifecycle control.
- Four tests are not also labeled `integration`:
  `ShieldedAdversarialHardening`, `ShieldedDerivation`,
  `ShieldedPoolRoundTrip`, and `ShieldedReindexEquivalence`.

Recommended next action:

1. Run the four non-integration shielded canonicality tests locally and in a PR.
2. If stable, relabel them away from broad `canonicality` into a narrower active
   label such as `canonical-vectors` or `shielded-canonical-smoke`.
3. Leave restart/reindex/reorg equivalence tests quarantined until their fixture
   and runtime profile is documented.

## Label: `packaging`

Tests:

```text
CmakeInstallLayout
DineroBackup
DineroPrepareUpgrade
```

Read:

- This bucket is small and probably separable.
- `CmakeInstallLayout` sounds like a cheap metadata/layout smoke check.
- `DineroBackup` and `DineroPrepareUpgrade` may be more stateful because they
  touch backup, recovery, rollback, or upgrade paths.

Recommended next action:

1. Focus-run the three tests with timeouts.
2. If `CmakeInstallLayout` is cheap and deterministic, graduate it first.
3. Keep backup/upgrade tests under a packaging or release workflow until their
   filesystem fixture expectations are clear.

## Label: `integration`

Tests:

```text
AddressBalanceMempoolOverlay
AddressIndexCrossHRP
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
RbfPolicyReporting
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
RpcStopCleanShutdown
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
TaprootSignRawTransactionRbfConfirmation
TaprootSignRawTransactionRbfOverlay
TipPersistRestartEquivalence
UndoMetadataRestamp
UnifiedBatchAtomicity
UnifiedBatchAtomicity_AtomicPersistOn
UtreexoMempoolCanonicalSeparation
WalletCreateRawTransactionScriptPubKeyOutputs
WalletListUnspentExcludesMempoolSpent
Wallet_W2_6_SyncIntegration
WsCookiePathResolution
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
| Address/RPC/wallet smoke | `AddressIndexCrossHRP`, `WalletListUnspentExcludesMempoolSpent`, `WsCookiePathResolution` | Candidate PR-CI smoke group after focused runs. |
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

1. PR #67: focus-run and, if stable, graduate the four non-integration shielded
   canonicality tests by relabeling them out of broad `canonicality`.
2. PR #68: focus-run packaging tests; graduate `CmakeInstallLayout` if it is
   cheap and deterministic, and decide whether backup/upgrade belong in a
   packaging workflow.
3. PR #69: split address/RPC/wallet integration smoke candidates from the broad
   `integration` label.
4. Later: design dedicated lanes for P2P/network, CSN/Utreexo, shielded E2E,
   release gates, and fuzz.

The rule from the named-test cleanup still applies: shrink the quarantine only
when a smaller ownership boundary proves itself. Do not chase a green badge by
moving broad labels wholesale.
