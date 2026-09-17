# Compact daemon/wallet sanitizer qualification

This gate exercises the experimental compact format through the real daemon,
wallet, full/CSN Utreexo paths and recovery. It extends the earlier codec/native
sanitizer qualification; that earlier success did not qualify these daemon
paths. Activation settings and proof rules remain unchanged. The first Linux
run exposed the packed-header serializer defect documented below. The next run
passed that regression and exposed an empty-field storage-reader defect.

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

- `PackedHeaderAlignment`
- `SerializationEmptyBuffers`
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
requires a successful CTest exit, exactly the ten expected executed tests,
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

## First Linux finding: packed block-header alignment

Run `35166249675` failed: three of eight tests passed, and eight UBSan reports
identified the same reference-binding error at
`include/common/serialization.h:237`. A packed `BlockHeader` has alignment one;
its 64-bit timestamp sits at offset 100. Passing that member directly to
`VectorWriter::write(const T&)` binds an incorrectly aligned reference. The
daemon's existing startup self-check reaches it before RPC readiness, and the
reindex test reaches it while storing the genesis header. Those early aborts
explain the dependent lifecycle failures and readiness timeouts.

The fix loads the four integer fields into naturally aligned local values
before passing references to the writer. It retains the packed struct, field
types/order, reserved bytes, 128-byte layout and Utreexo commitment bytes.
There is no sanitizer suppression or increased readiness timeout.

`PackedHeaderAlignment` constructs headers at all eight byte placements and
compares serialization/deserialization with an independent literal 128-byte
vector, including distinct Utreexo-root bytes and high timestamp bits. It is
registered in the default test inventory and added to this sanitizer gate.
The workflow also runs it under UBSan before the expensive full build.

On macOS ARM64, UBSan alone reproduces the original failure, passes after the
fix, and fails again against a temporary copy of the old serializer. This is
separate from the unavailable local ASan runtime. Linux run `35169782354`
passed the early UBSan regression and the registered packed-header test. Four
of nine selected tests passed; the other five encountered the next defect.

## Second Linux finding: empty serialized fields

All eight runtime reports from `35169782354` identify
`Reader::read` calling `memcpy` with a null destination. `readBytes()` allocates
an empty vector for a valid zero-length field, whose `data()` may be null.
The C library requires non-null pointers even when the copy count is zero.
Reindex reaches this through `ChainDB::getCoin`; daemon startup reaches it
through `ChainDB::forEachUTXO` while loading the initial UTXO state. These aborts
cause the dependent lifecycle failures and readiness timeouts. The reader
predates the compact work.

The fix makes a zero-length raw read a no-op after the existing bounds check.
The writer also returns before forming a payload range for an empty raw write.
Length prefixes and nonempty payloads are unchanged. The fix changes no
transaction/proof format, validation rule, Utreexo leaf or commitment.

`SerializationEmptyBuffers` covers a literal sequence of empty and nonempty
fields, raw null/zero reads and writes, cursor position, destination preservation,
missing lengths, truncated payloads and rejection of an invalid cursor. It is
registered in the default CI inventory and required by this daemon gate; both
serializer regressions also run under UBSan before the expensive full build.

A local Linux ARM64 container with GCC 12 reproduced the exact null-pointer
diagnostic before the fix. Both serializer regressions passed with ASan/UBSan
after the fix; removing only the zero-length guards in a temporary source copy
reproduced the failure. The registered macOS UBSan tests also passed, but the
Apple libc declaration did not reproduce this particular diagnostic before the
fix, so the Linux control supplies the regression evidence. These focused
results do not qualify the full daemon: a fresh full Linux run is required and
may expose further findings.

Independent format/consensus review, pool-protocol qualification and reviewed
production activation remain separate gates. The package-admission gate passed
separately in run `35169304739`; it does not substitute for daemon sanitizer
qualification. This work does not authorize a deployment or activation.
