# Covenant sanitizer-fuzz evidence

Assurance artifact for release gate 1 of the covenant activation
(`COVENANT_MAINNET_ACTIVATION_100000.md`), specifically the *sanitizer-fuzz*
evidence.

Companion artifacts: `COVENANT_MUTATION_COVERAGE.md` (mutation),
`tests/consensus/test_ccv_reference_model.cpp` (differential),
`tests/vectors/bip119_ctvhash.json` (upstream CTV vectors).

## What is fuzzed

The **real production paths**, never a reimplementation:

| Target | Path driven |
|---|---|
| CCV witness decoder + transition | crafted witness bytes → `ScriptVerifier::VerifyTaproot` → `DeserializeContractState` → `VerifyContractTransition` |
| CCV Taproot binding context | fuzzed tapscript and control block through the same verifier |
| CTV template hash | arbitrary bytes → `TransactionSerializer::Deserialize` → `TryComputeCTVHash` |

`DeserializeContractState` is file-local to `tapscript_interpreter.cpp`, so it is
reached through the script verifier rather than called directly. That is not a
workaround — it is the path a real peer's transaction takes, and testing it any
other way would be testing a copy.

## Oracles

"It did not crash" is the weakest possible fuzz oracle, so the harness also
asserts:

- **Determinism** — identical bytes must produce an identical verdict on repeat
  evaluation. A consensus verifier whose answer depends on uninitialised memory
  or iteration order would split the network rather than merely fail a test.
- **CTV digest stability** — a template hash that is computable must recompute
  equal. CTV commitments are meaningless if one transaction hashes two ways.

Under the sanitizer build these become memory-safety, UB, and overflow oracles
as well.

## Running it

Bounded gate (runs in normal CI on every push, ~85 ms):

```sh
cmake -S . -B build
cmake --build build --target test_covenant_fuzz -j8
ctest --test-dir build -R '^CovenantFuzz$' --output-on-failure --no-tests=error
```

`--no-tests=error` is required: without it, a rename or de-registration makes
ctest select nothing and exit 0, which reads as a pass. That exact failure is
live in this repo — see issue #486.

Longer campaign, same binary:

```sh
DINERO_COVENANT_FUZZ_ITERATIONS=200000 ./build/test_covenant_fuzz
```

The reproducible two-pass sanitizer campaign is:

```sh
./scripts/covenant-sanitizer-campaign.sh --iterations 200000
```

It runs fail-fast UBSan separately from unsigned-overflow instrumentation. The
second pass records full output, accepts primary diagnostics only from the
documented modular-arithmetic implementations in SHA-256 and secp256k1, and
fails if a primary diagnostic originates anywhere else. Stack frames that call
hashing from covenant code are not misclassified as covenant overflows; the
primary diagnostic location is the classification boundary.

Budget realistically. Each CCV iteration runs `VerifyTaproot` **twice** for the
determinism oracle, and each call performs the internal-key retry search plus a
Taproot tweak, so secp256k1 dominates the cost. Uninstrumented that is cheap;
under a sanitizer it is not. A 200,000-iteration sanitizer campaign does not
finish in a ten-minute window — size scheduled campaigns accordingly rather than
copying a number from this page and assuming it will return.

## Sanitizer build

Use **UBSan**, not ASan. See the ASan limitation below.

```sh
cmake -S . -B build-covfuzz-uio \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-fsanitize=undefined,unsigned-integer-overflow -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=undefined,unsigned-integer-overflow -fno-omit-frame-pointer" \
  -DDINERO_BUILD_QT=OFF -DDINERO_BUILD_MINER=OFF
cmake --build build-covfuzz-uio --target test_covenant_fuzz -j8
DINERO_COVENANT_FUZZ_ITERATIONS=20000 ./build-covfuzz-uio/test_covenant_fuzz
```

Add `-fno-sanitize-recover=undefined` when you want the campaign to **gate**.
By default UBSan prints a diagnostic and continues, so a run can report
undefined behaviour and still exit 0. Omit it when you want to collect every
report for triage, which is what the recorded run below did.

### ASan is currently unusable on this codebase

**Not a covenant defect, and not sanitizer overhead.** An ASan-instrumented
binary takes over three minutes to reach `main`, measured with
`--gtest_list_tests`, which executes no test code at all:

| Build | Startup |
|---|---:|
| Non-sanitized | 0.010 s |
| UBSan only | 0.018 s |
| ASan | > 200 s |

That is a fixed cost paid before any test runs, so every ASan invocation of any
target pays it. It makes ASan campaigns impractical here regardless of iteration
budget. Tracked as issue #487; do not attempt to size an ASan campaign around it.

The consequence for this artifact is stated plainly: the sanitizer evidence
below is **UBSan and unsigned-integer-overflow only**. There is no ASan
memory-safety evidence yet, and this document does not claim any.

### Unsigned overflow specifically

Plain UBSan does **not** flag unsigned wraparound, because unsigned overflow is
well-defined in C++. It must be requested explicitly:

```sh
-fsanitize=undefined,unsigned-integer-overflow
```

This is the check that matters for the decoder. `DeserializeContractState`
compares `offset + dataLen` against `bytes.size()`, where `dataLen` is a 32-bit
attacker-controlled field. The claim that this cannot wrap rests on `size_t`
being 64-bit — true on every platform this project targets, but a platform
assumption rather than a language guarantee. The seed corpus in
`tests/vectors/covenant_fuzz_seeds/length_field_overflow.hex` exists to make
that assumption testable rather than argued.

Caveat: this sanitizer is noisy on cryptographic code, which wraps deliberately
and correctly (SHA-256, PRNGs, curve arithmetic). Expect reports from hashing
internals that are not defects. Triage by call site — only reports inside the
covenant decoder and transition verifier are findings.

## Observed results

Recorded at the commit introducing this document, macOS arm64, Apple clang 17.

An extended rerun on 2026-08-30 used 200,000 iterations per target under
fail-fast UBSan. All four tests passed in 10.7 seconds with a maximum resident
set of approximately 14.3 MB. No undefined-behavior diagnostic was emitted.

The companion unsigned-overflow pass used 20,000 iterations per target. All
four tests passed. Primary diagnostics were confined to the repository SHA-256
implementation and vendored secp256k1 fixed-width modular arithmetic; there
were zero primary diagnostics in `covenants.cpp`,
`tapscript_interpreter.cpp`, or `script_verify.cpp`.

### Unsigned-overflow pass

Build flags `-fsanitize=undefined,unsigned-integer-overflow`, seed corpus plus
2,000 iterations per target. `-fno-sanitize-recover` was deliberately **omitted**
here so the run collects every report for triage instead of halting on the first.

```
runtime errors reported            18
  in src/crypto/sha256.cpp         18
  in covenant decoder / verifier    0
tests                        4 passed
```

Every report is in the SHA-256 round function, which wraps deliberately and
correctly per FIPS 180-4. **Zero reports originate in `covenants.cpp`,
`tapscript_interpreter.cpp`, or the script verifier.**

This is the empirical answer to the `offset + dataLen` question. Prior reasoning
held that the addition cannot wrap because `size_t` is 64-bit; that was a
platform assumption rather than a language guarantee. It is now backed by
instrumentation over a corpus built specifically to provoke it — including
`dataLen` at `0xFFFFFFFF`, the 32-bit wrap candidate, and the sign-bit boundary.

Scope of the claim: no wraparound occurs **for these inputs on this platform**.
It is evidence, not a proof over all inputs.

## Seed corpus

`tests/vectors/covenant_fuzz_seeds/*.hex`, replayed before random search.

| File | Covers |
|---|---|
| `decoder_boundaries.hex` | empty element, one byte below the 72-byte header, exact header, trailing byte, declared-length vs payload mismatch |
| `length_field_overflow.hex` | `dataLen` at `0xFFFFFFFF`, near-max with payload, the 32-bit wrap candidate, the sign-bit boundary, terminal counter with huge length |
| `size_limits.hex` | exactly 448 data bytes, 449, and a 520-byte element |

**Any input that ever crashes or trips a sanitizer must be appended here
permanently.** That is the regression half of fuzzing: a crash found once and
not committed as a seed will be found again.

The corpus ships non-empty and already encodes the decoder's boundary cases, so
the mechanism has teeth on its first run rather than being an empty directory
that only starts mattering after something goes wrong.

## Deliberate limits

- Random search over structured mutations is not a proof of absence. It samples;
  it does not enumerate. Exhaustive reasoning over reduced domains belongs in the
  bounded-state artifact, not here.
- The bounded gate uses fixed seeds, so every push explores the *same* inputs.
  That makes it a regression gate, not a discovery mechanism. Discovery comes
  from the longer campaign with a raised iteration budget.
- Coverage is not measured. The harness does not know which branches of the
  decoder it reached, so "N iterations passed" is not "N iterations of useful
  work".
- The CTV target only reaches `TryComputeCTVHash` for byte strings that happen
  to deserialize as transactions. Most random buffers do not, so effective CTV
  coverage is far lower than the raw iteration count suggests.
