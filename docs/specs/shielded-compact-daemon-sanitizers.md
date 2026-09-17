# Compact daemon/wallet sanitizer qualification

This gate exercises the experimental compact format through the real daemon,
wallet, full/CSN Utreexo paths and recovery. It extends the earlier codec/native
sanitizer qualification; that earlier success did not qualify these daemon
paths. No production implementation, activation setting or proof rule changes.

## Command and scope

On Linux, after installing the standard build dependencies and pinned OpenSSL:

```sh
OPENSSL_VERSION=3.5.7 bash scripts/build-openssl-vendored.sh
bash scripts/compact-regtest-sanitizers.sh
```

The evidence directory must be new. `COMPACT_SANITIZER_BUILD_DIR`,
`COMPACT_SANITIZER_EVIDENCE_DIR` and `COMPACT_SANITIZER_JOBS` select locations and
build parallelism. `--test-only` audits and tests an already configured/built
instrumented binary; it does not waive any audit or evidence check.

The `Compact daemon sanitizers` workflow runs the same command on Linux x86-64.
It compiles first-party code with AddressSanitizer and UndefinedBehaviorSanitizer,
frame pointers and no recovery from reported undefined behavior. A compile-command
audit checks 30 required source files across the daemon, wallet, parsing, proof
verification, ChainDB, Utreexo and test drivers. A linked-symbol audit additionally
checks the actual daemon binary and records its hash and source provenance.

The selected CTest entries are exactly:

- `ShieldedResourceLimits`
- `CompactRegtestFixedVectors`
- `CompactRegtestVectorOracle`
- `ShieldedReindexEquivalence`
- `ShieldedAuthRelayLifecycle`
- `CompactRegtestLifecycle`
- `CsnManualInvalidation`
- `CSNShieldedReorgInvertibility`

This includes ordinary Auth and compact wallet proving/relay/mining, a signed
transparent spend of an unshield output, Utreexo inclusion/spentness and state
equality, activation-crossing reorg, manual recovery, reindex and restart. The
Python oracle remains an independent byte checker, not instrumented C++ code.

## Preventing false green results

Before the build, two tiny programs deliberately trigger a heap use-after-free
and signed integer overflow. Both must exit unsuccessfully and retain the
expected sanitizer diagnostic via `log_path`. A broken runtime fails this
preflight within 30 seconds; it cannot appear as an uneventful daemon test.
These intentional reports live separately from actual qualification reports.

For the actual tests, sanitizer reports go outside temporary daemon data
directories, so harness cleanup cannot erase them. The final evidence gate
requires a successful CTest exit, exactly the eight expected executed tests,
no failed/skipped/disabled results, complete instrumentation coverage and no
runtime reports. This catches a report during shutdown even if a shell helper
waits for a child process without propagating its exit status.

Ten fast negative-control tests cover missing evidence, omitted/duplicate tests,
failed/skipped/malformed JUnit, summary failures, nonzero process status, shutdown
reports, stdout diagnostics and missing/disabled instrumentation. Removing the
runtime-report rejection branch makes its targeted control fail; restoring the
original guard restores green.

## Current evidence and limits

Local macOS ARM64 built all five required instrumented binaries. The audit found
all 30 source files instrumented (99 compile-command entries). The evidence-gate
self-tests pass. Actual local ASan daemon tests are **not qualified**: Apple
Clang 17's ASan runtime deadlocks during startup on this host, before `main`.
A standalone tiny probe reproduces it; a process sample shows reentrant ASan
initialization while inspecting dyld mappings. The affected processes were
stopped, and the missing/aborted test results correctly make the gate fail.
Linux is the qualification platform for this run.

Pinned prebuilt OpenSSL and RocksDB's isolated ExternalProject build are not
instrumented by these flags. This is a first-party ASan/UBSan gate, not complete
dependency coverage or ThreadSanitizer daemon qualification. Linux enables leak
detection; Apple ASan does not provide it. No suppressions are added.

The separate fixed-byte gate already passed on Linux x86-64 and ARM64 in run
35164299168. Both artifacts match the local fixture SHA-256s and have 3/3 executed
CTest entries with no skips/failures. GitHub tested merge commit
`7f12f361dbe3b39d802a9b60e26a65ab4a15afaa`; its tree
`d8c0043dc1f52afaba4cb59cd1e56d511efaaa35` equals PR #761 head
`95f235998f65324d937300b622bf501d72298642`. Those results establish fixed-byte
cross-architecture agreement, not sanitizer success.

The Linux daemon sanitizer result must be recorded separately when completed.
Independent format/consensus review, remaining package-admission boundaries,
pool-protocol qualification and reviewed production activation remain separate
gates. This work does not authorize a deployment or activation.
