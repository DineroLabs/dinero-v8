# Test Workflow v2 Plan

Test Workflow v2 is the safety net required before moving `dinero_core` target
ownership or other load-bearing boundaries.

Generated after the `dinero_core` source-map checkpoint at `73c5e492`. This is a
design document only; it does not add or modify GitHub Actions workflows.

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

Expected command shape:

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Release \
  -DDINERO_USE_VENDORED_DEPS=ON \
  -DENABLE_GRPC=OFF
cmake --build build-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure --exclude-regex '<known-broken-regex>'
```

## Known-Broken Exclusion Policy

The initial known-broken exclusion is:

| Test | Status | Required follow-up |
| --- | --- | --- |
| `test_csn_proof_refresh` | Pre-existing failure observed during prior CI/build work | Track with a GitHub issue that decides fix-or-delete. |

Rules for adding exclusions:

1. Each excluded test needs a reason in the workflow or a companion document.
2. Each excluded test needs a GitHub issue with a fix-or-delete plan.
3. Exclusions should be added by pull request, not by emergency drive-by edits.
4. "Shut it up" is not a valid reason.
5. Flaky tests should be marked as flaky/quarantined and tracked separately from
   deterministic known-broken tests.

## First-Run Expectations

The first full workflow runs should be expected to fail. That does not mean the
plan is wrong; it means the deeper workflow is finally observing failures the
fast daemon build does not cover.

Expected timing:

- Full from scratch: roughly 60-90 minutes.
- Warm cache: roughly 15-30 minutes.

Expected first-run failure classes:

- Missing Linux packages similar to earlier `libudev`/`libusb` dependency fixes.
- Tests that were already broken but not executed by fast CI.
- Real Linux-specific build or link issues in non-daemon targets.
- Flaky tests that need quarantine before the workflow becomes required.

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
2. PR 2+: iterate on missing dependencies, deterministic known-broken tests, and
   flaky quarantines.
3. First green on `dinero-main`: begin a one-week soak period.
4. During soak: track every failure by issue and classify it as regression,
   known-broken, dependency, flaky, or timeout.
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
