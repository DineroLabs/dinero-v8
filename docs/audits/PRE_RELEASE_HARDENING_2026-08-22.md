# Pre-release hardening audit — 2026-08-22

## Scope and release posture

This audit covers the eight pre-release workstreams requested after the
height-92895 network incident. It is a preventive engineering change set, not
an emergency chain intervention. It does not deploy binaries, cut a release,
change the three mandatory fleet-anchor roles, or activate unfinished covenant
rules on mainnet.

The work is intentionally carried by one branch and one pull request so its
consensus, recovery, networking, build, and test-integrity effects are reviewed
as a single tree.

## Results by workstream

| # | Area | Result |
|---|---|---|
| 1 | Qt shutdown | The disconnect-time null dereference was fixed and merged independently in PR #619. |
| 2 | Covenant readiness | Official BIP119 transaction vectors now exercise outer witness handling. Incomplete CTV/CCV covenant activation is explicitly deferred on mainnet instead of silently activating at height 100,000; protocol documents and resource-limit claims were reconciled with implementation. |
| 3 | AssumeUTXO | Forward-connect replay, promotion, corrupt/incomplete checkpoint recovery, pre-base spends, late wallet import, and snapshot rotation remain covered. Peer ordering for historical backfill now prefers measured low latency while retaining retry rotation. |
| 4 | CSN/light recovery | Canonical replay and transition-forest fixtures were corrected to model the forest at the spend height. Stored side-branch bodies can be adopted from durable flat-file metadata rather than downloaded repeatedly while acceptance is busy. |
| 5 | Block scheduling | A stored-and-connecting body suppresses stale `getdata`; genesis is always present in capped block locators; disconnected peers are immediately removed from scheduler eligibility; in-flight attempts no longer masquerade as completed peer successes; compact reconstruction treats a missing mempool as empty and requests the absent transactions. |
| 6 | Reproducible builds | Reproducible mode now requires an explicit `SOURCE_DATE_EPOCH`, normalizes first-party and isolated vendored RocksDB source/build paths plus the compiled development schema fallback and Linux linker metadata, preserves the loader-required Mach-O UUID, pins the Rust MSRV, compares independent artifacts, and runs two isolated builders in CI. |
| 7 | Test integrity | Release tests retain live assertions and include a compile/runtime tripwire. P2P fixtures use ephemeral ports and no `SO_REUSEPORT`. Integration topologies share a CTest resource lock and use scoped port allocation/process cleanup, preventing cross-test daemon collisions and false greens. |
| 8 | Security/DoS | Both inventory parsers validate complete CompactSize encodings before allocation, enforce the shared protocol count limit, reject byte-count mismatches/trailing payloads, and bounds-check fixed-width reads. Adversarial serialization tests cover truncation and oversized-count cases. |

## Consensus and compatibility assessment

- The covenant change is fail-closed deferral, not activation and not a new
  consensus rule for existing blocks.
- BIP119 vectors pin the corrected future witness interpretation. The
  consensus-loosening outer-stack correction is staged behind the deferred CTV
  flag, so this PR does not make mixed-version mainnet nodes accept different
  script-path spends.
- AssumeUTXO/CSN changes concern recovery, scheduling, and test-model accuracy;
  they do not alter proof commitments or accepted accumulator roots.
- The locator, inventory, compact-block, and peer-selection changes affect
  transport/recovery behavior only. Inventory limits use the existing shared
  P2P protocol limit.
- No static anchor was removed or demoted by this change set.

## Validation contract

The branch must not merge unless all of the following hold:

1. A complete local build succeeds.
2. Focused covenant, recovery, scheduler, header-sync, inventory, block-relay,
   wallet, and Rust MSRV/Clippy tests pass.
3. The broad non-meta CTest matrix passes with assertions enabled.
4. Shell syntax, whitespace, and the raw-assertion ratchet pass.
5. GitHub's Linux compile/test lanes, root-hygiene gate, and two-builder
   reproducibility comparison are green on the same PR head.

The long release/meta suites remain merge-gate responsibilities in CI; a green
unit executable with assertions compiled out is explicitly not evidence.

## Local validation evidence

The final tree was validated on macOS arm64 with:

- a complete `cmake --build build-stability -j8` build;
- 435/435 broad non-integration/non-soak CTest registrations passing with
  assertions enabled (1,348.82 seconds);
- a final 22/22 focused matrix covering covenant/script paths, recovery,
  header sync, block scheduling, relay, inventory parsing, and Utreexo after
  the activation gate was finalized;
- all 525 registered CTest executables present;
- the raw-assert ratchet at 2,316 assertions across 125 files, with no growth;
- Rust 1.75.0 MSRV Clippy and current-toolchain Clippy passing with warnings
  denied;
- changed-shell syntax, workflow YAML parsing, root hygiene, whitespace, and
  reproducibility-comparator positive/negative self-tests passing; and
- the repaired consensus hygiene guard passing against the current
  `rpc_server.cpp` GBT implementation and promoted into the Linux CI lane.

## Residual risks and follow-up

- Reproducibility is toolchain-scoped: identical pinned inputs/toolchains are
  required. Cross-OS binaries are not expected to be byte-identical.
- CompactSize canonical-encoding policy outside the audited inventory message
  families should receive a separate protocol-wide review; this patch closes
  the unbounded-allocation paths in scope.
- The mandatory anchors remain an availability backstop. Dynamic AddrMan/DNS
  peer diversity must continue to grow so anchors are not a trust bottleneck.
- Release signing, macOS notarization, canary deployment, and fleet rollout are
  deliberately outside this engineering PR and require the normal release
  procedure after merge.
