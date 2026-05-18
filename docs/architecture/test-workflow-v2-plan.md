# Test Workflow v2 Plan

Test Workflow v2 is the safety net required before moving `dinero_core` target
ownership or other load-bearing boundaries.

Generated after the `dinero_core` source-map checkpoint at `73c5e492`.
Updated during the initial `.github/workflows/tests.yml` rollout after the
first full-build `ctest` pass exposed pre-existing runtime/integration debt.

## Purpose

The existing `.github/workflows/ci.yml` answers the fast question:

- Does the daemon configure?
- Does `dinerod` build and link?
- Does the binary start well enough for a smoke check?

That workflow is intentionally short and cheap. It is the right default signal
for small source-list and build-system changes.

Test Workflow v2 answers a different question:

- Did a source file get silently dropped from compilation while its header is
  still consumed?
- Did a behavior test regress?
- Did a broader target graph fail when the whole tree is built instead of only
  `dinerod`?

Those are different failure modes. They need a deeper workflow instead of
turning the fast daemon CI into a slow catch-all.

## Proposed Triggers

The eventual workflow should run on:

- Pull requests targeting `dinero-main`.
- Pushes to `dinero-main`.
- Manual `workflow_dispatch`.

It should use concurrency with cancel-in-progress for the same branch or pull
request, matching the fast CI workflow's iteration style.

Recommendation: do not make the workflow a required merge check immediately.
Let it soak for one week after it is green on `dinero-main`, then promote it to
a required check once failures are understood and documented.

## Proposed Scope

The eventual workflow should:

- Use `ubuntu-latest`.
- Use `permissions: contents: read`.
- Use the same dependency cache posture as `ci.yml` where practical.
- Configure with vendored dependencies and gRPC disabled unless a later design
  explicitly requires gRPC coverage.
- Build the full default target graph rather than only `--target dinerod`.
- Run `ctest --output-on-failure`.
- Exclude only known-broken or quarantined tests through a documented regex.
- Set a timeout suitable for full test execution.

Current command shape:

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DDINERO_USE_VENDORED_DEPS=ON \
  -DENABLE_GRPC=OFF
cmake --build build-tests -j$(nproc)
  ctest --test-dir build-tests \
  --output-on-failure \
  --label-exclude 'integration|gate|release|canonicality|fuzz|packaging'
```

## Known-Broken Exclusion Policy

The first full `ctest` run after the full target graph built successfully
surfaced broad pre-existing runtime and integration debt. The initial workflow
therefore starts with the stable full-build subset and makes the quarantine
explicit.

Excluded labels:

| Label | Status | Required follow-up |
| --- | --- | --- |
| `integration` | Broad pre-existing integration/runtime harness failures | Split into focused harness/runtime repair issues before re-enabling. |
| `gate` | Release/readiness gate tests are not stable enough for first soak | Decide whether each gate is CI-ready, nightly-only, or obsolete. |
| `release` | Full `ReleaseSuite` is a heavy manual/release gate with baseline parity, P2P storm, IBD torture, Utreexo, canonical recovery, and mempool stress stages | Keep the full suite quarantined until each heavy stage has an explicit CI-vs-manual ownership decision. |
| `canonicality` | Canonical recovery/readiness tests fail in the first full pass | Track deterministic failures and re-enable by category. |
| `fuzz` | Fuzzer-style tests are not suitable for the first required-style subset | Move to a dedicated fuzz/nightly plan or make deterministic. |
| `packaging` | Packaging tests are outside the first Linux full-build subset | Add package workflow coverage separately. |

Excluded test names: none. All formerly exact-name-quarantined tests have
graduated to the active Test Workflow v2 subset.

This map marks the current quarantine boundary. It should shrink over time; it
should not grow without the same level of evidence.

See `docs/architecture/quarantined-test-classification.md` for the first
per-test classification pass. That pass found that `CsnProofRefresh` was already
running and passing because the old exclusion used the binary name
`test_csn_proof_refresh`, not the registered CTest name `CsnProofRefresh`.
Subsequent passes split cheap canonical vector/restart tests away from the broad
`canonicality` quarantine, fixed and graduated `ShieldedDerivation`, split
`WalletMainnetReadiness` into per-subcase CTest entries, and graduated several
exact-name quarantines:

- `ArchivalBlockReader` by making its synthetic reindex fixture write a valid
  height-1 Utreexo commitment.
- `UtreexoE2ERelay` by fixing CSN transition-proof stump advancement for
  deletion-bearing proofs.
- `WalletMainnetReadiness_Bip86DeterminismProperty1000RandomMnemonics` by
  fixing stale 1447 coin-type pins in the local test helpers and giving the
  1000-mnemonic property case its own 300-second CTest timeout.
- `WalletMainnetReadiness_EncryptionRoundTripRestoreAndDerivationPersistence`
  by updating the stale restore expectation to the current 20-address receive
  gap-window contract and asserting continued derivation from index 20.
- `WalletMainnetReadiness_RestoreResetsLegacyEncryptionStateBeforeReEncrypt`
  by exercising the restore API's explicit `replace_existing` path for
  overwrite restore.
- `WalletMainnetReadiness_ReorgSelfSpendWithChangeRestoresSpentStateAndHeightMetadata`
  by recording wallet-affecting self-spend/change history when a confirmed
  block scan has no pre-existing send row to confirm.
- `ConsensusFormalVerification` by correcting the stale end-of-epoch supply
  formula check to use the last block of the epoch, not the first block of the
  next epoch.

See `docs/architecture/remaining-test-label-classification.md` for the current
label-only quarantine map after the exact-name list reached zero. That map is
the handoff point from named-test repair to label taxonomy.

The first label split graduated `ShieldedDerivation` and
`ShieldedReindexEquivalence` from broad `canonicality` into
`shielded-canonical-smoke`. `ShieldedPoolRoundTrip` and
`ShieldedAdversarialHardening` remain quarantined because their helper binaries
are stale against current shielded APIs and do not compile in the local full
build.

`ReleaseSuiteConfigSmoke` is intentionally outside the `release`/`gate`
quarantine. It verifies the release-suite wiring, build directory, `dinerod`
binary, required scripts, and `ctest` availability without running the heavy
release stages or requiring a baseline binary.

Rules for adding exclusions:

1. Each excluded test needs a reason in the workflow or a companion document.
2. Each excluded test needs a GitHub issue with a fix-or-delete plan.
3. Exclusions should be added by pull request, not by emergency drive-by edits.
4. "Shut it up" is not a valid reason.
5. Flaky tests should be marked as flaky/quarantined and tracked separately from
   deterministic known-broken tests.
6. Retire exclusions one category or test at a time through focused PRs.
7. Keep the workflow green while reducing the quarantine; do not bundle broad
   test repair with unrelated architecture work.

## Test Retirement Policy

Quarantine is a holding state, not a final disposition. A quarantined test should
be classified before it is re-enabled, rewritten, converted to a smoke check, or
deleted.

Use this decision record for obsolete-looking tests:

```text
Test:
Current status:
Original purpose:
Still relevant: yes/no/partial
If obsolete, why:
Replacement coverage:
Action: keep quarantined / convert to smoke / rewrite / delete
```

A quarantined test may be deleted only when:

- The feature or behavior it covers is no longer supported.
- No current code path depends on the old behavior.
- Replacement coverage exists, or the lost safety signal is explicitly accepted.
- The deletion PR explains the rationale.

Harness debt is not obsolescence. For example, a test that assumes the wrong
build directory should be fixed or converted, not deleted. Heavy release,
network, or readiness checks may belong in manual, nightly, or release-gate
ownership instead of normal PR CI. Tests for behavior replaced by newer protocol
or wallet design should usually be rewritten around the current behavior before
deletion is considered.

## First-Run Expectations

The first full workflow runs were expected to fail. That did not mean the plan
was wrong; it meant the deeper workflow finally observed failures the fast daemon
build did not cover.

Observed timing after cache warm-up and initial fixes:

- Fast daemon CI: 2m42s.
- Test Workflow v2 full build plus stable `ctest` subset: 8m26s.

Observed first-run failure classes:

- Missing Linux packages similar to earlier `libudev`/`libusb` dependency fixes.
- Real Linux-specific build or link issues in non-daemon targets.
- Tests that were already broken but not executed by fast CI.

Iteration should follow the proven `ci.yml` pattern: inspect logs, make the
smallest justified change, rerun, and document each exclusion or dependency fix.

## Failure-Mode Policy

| Failure class | Policy |
| --- | --- |
| Known-broken test | Add to the exclusion list only with a documented reason and tracking issue. |
| Real regression | Block the PR and fix the regression. |
| Missing dependency | Add the package/tooling dependency if it is broadly required and not masking a code issue. |
| Flaky test | Quarantine with a flaky label/reason and open a triage issue. |
| Timeout | Increase timeout only after confirming useful work is still progressing; otherwise reduce scope or fix the hang. |

## Rollout Sequence

1. PR 1: add `tests.yml` with full build and `ctest --output-on-failure`, using
   the documented initial exclusion list.
2. First green on `dinero-main`: begin a one-week soak period.
3. During soak: track every failure by issue and classify it as regression,
   known-broken, dependency, flaky, or timeout.
4. PR 2+: retire quarantined labels/tests through focused fixes.
5. After one clean week: mark the workflow as a required check for
   `dinero-main`.
6. Ongoing: prune exclusions as tests are fixed or deleted.

## Success Criteria

The workflow is ready to become required when:

- It is green on `dinero-main`.
- All exclusions are documented with reasons.
- Every excluded or flaky test has a GitHub issue.
- One week of soak produces no unexplained failures.
- Runtime is understood well enough that contributors know when to expect a
  fast CI signal versus a deep test signal.

## Relationship To Architecture Work

No load-bearing `dinero_core` target ownership move should depend only on the
fast daemon CI. Before moving `dinero_core` ownership, RPC boundaries, service
hubs, or consensus-adjacent files, the project should have this deeper workflow
available as a second signal.

Recommended ordering:

1. Land Test Workflow v2.
2. Soak and make it required.
3. Create an RPC boundary plan before moving `rpc_registry` or RPC-adjacent
   handlers.
4. Create a service-boundary plan before moving public service hubs.
5. Treat the consensus cluster as a separate future project.
6. Only after the graph and tests support it, consider splitting `dinero_core`
   into smaller libraries.
