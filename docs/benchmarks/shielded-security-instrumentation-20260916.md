# Shielded security matrix: verify actual instrumentation

The existing security script set `ENABLE_SANITIZERS=ON`, which creates an
opt-in CMake interface target. The four selected shielded test executables and
their crypto/parser libraries did not link that interface. A green native test
run from that configuration would not be an ASan/UBSan qualification.

The dedicated ASan, TSan and fuzz builds now pass explicit C/C++ sanitizer flags
to all sources compiled in those build directories. The pinned prebuilt OpenSSL
library is still uninstrumented. Fuzzer coverage instrumentation remains on the
fuzzer/codec translation units, with memory/UB instrumentation on compiled
library dependencies too. Normal production builds are unaffected.

Before compilation, `check_shielded_sanitizer_coverage.py` checks the generated
compile commands for the compact codec, scalar/Spartan implementations,
shielded serialization/derivation, and applicable test or fuzz harnesses.
Missing sources, missing sanitizer flags, or flags disabled later in the command
fail the gate. This is an instrumentation gate, not proof that a test passed.

Test-first evidence: the prior configuration fails this gate; explicit flags
make it pass. Checks also cover explicit disabling of instrumentation. A
controlled shell-stage failure confirms later independent stages still run
while the aggregate result remains failing. CI uploads per-stage logs,
compile-command coverage receipts, JUnit and provenance even on failure.

## Run on Linux

```sh
SHIELDED_FUZZ_SECONDS=600 scripts/shielded-security-matrix.sh
```

The scheduled/manual `Shielded readiness` workflow invokes this command.
A failing TSan stage cannot suppress ASan/fuzz evidence; any failed stage still
fails the job. Native ASan+UBSan, native TSan, shielded-surface fuzz and compact
fuzz remain separate results. Mac runtime startup failures remain unqualified,
not successful sanitizer coverage. Linux execution for this revision is queued
separately from the local build-wiring checks recorded here.
