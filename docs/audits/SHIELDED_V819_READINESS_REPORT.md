# Shielded v8.1.9 Readiness Report

Date: 2026-08-30
Baseline: `v8.1.9` (`a2f071fd3311ed7367be6d3cb1d0c4ffb8128314`)

## Executive status

The shielded consensus implementation is active on mainnet, but fund-moving
wallet RPCs and the Qt surface are intentionally locked. They MUST remain
locked. A separate spend-authority activation/reset is now implemented and
dormant; no production height has been selected.

The highest-priority restart/reorg defect is fixed locally. Anchor-history
persistence now retains the bounded eviction journal required to reconstruct
the exact pre-connect window after a process restart. Legacy v1 records remain
readable and the DSR2 state fingerprint remains byte-compatible.

Readiness verdict: **mechanically improved, but not activation-ready**. Keep
the mainnet wallet/UI lock until external review, live empty-pool policy,
cross-platform gates, and complete post-cutover wallet construction are done.

## Work completed

1. **Persistent anchor restoration**
   - Added a versioned v2 persistence envelope containing roots and the bounded
     eviction journal.
   - Preserved v1 fingerprint/snapshot serialization and added v1 migration.
   - Updated normal connect, disconnect, promotion, and reindex database writes.
   - Added restart/disconnect, migration, and malformed-input atomicity tests.
2. **Activation-boundary coverage**
   - Re-ran recipient ownership, sender rejection, wrong scalar, zero scalar,
     proof-version mismatch, and legacy-address rejection coverage.
   - Added a distinct spend-authority reset, paired it with activation, routed
     it through connect/disconnect/reindex/restart/mempool logic, and made
     consensus verification require recipient-authority proof version `0x05`.
3. **Consolidated local evidence path**
   - `scripts/shielded-readiness.sh` builds/runs the readiness suites and writes
     JUnit/provenance evidence.
   - `tools/shielded_activation_readiness.py` checks the source lockouts and,
     when given a live CLI, verifies canonical state, shielded tip-marker, root,
     and empty-pool conditions. Source checks now verify reset lifecycle,
     activation pairing, and actual verifier consumption.
4. **Normative documentation and client sizing audit**
   - Corrected the address format to 75 bytes and approximately 132 Bech32m
     characters, and clarified mainnet activation versus wallet/UI lockout.
   - Added versioned 192-byte wallet FFI structures, raised the DPI limit, and
     made legacy parsing fail instead of truncate.
   - Audited the production clients: Xcode DineroDPI retries the prover-kit
     address call with its reported capacity; Android DineroDPI transports and
     stores shielded addresses as dynamic strings. Tauri is not a release path.
5. **Security test infrastructure**
   - Added one libFuzzer entry point for bundle, proof-container, encrypted-note,
     and shielded-address decoders.
   - Added local ASan/UBSan, Linux TSan, and fuzz orchestration. macOS uses
     Homebrew LLVM because Apple Clang does not ship the libFuzzer runtime.
6. **Determinism and worst-case tooling**
   - Added Debug/Release deterministic trace comparison, compiler/architecture
     discovery, worst-case bundle/tree/witness measurements, and valid 200-spend
     plus 200-output proof-verification measurements.
7. **CI-ready local gate**
   - Added a workflow that invokes the same readiness/security scripts and
     uploads evidence. It is wiring only until GitHub Actions access returns.

## Evidence collected

- Anchor history: 19/19 tests passed.
- Shielded circuit: 22/22 tests passed.
- Shielded validation includes a direct pre/post spend-authority proof gate.
- Chain-parameter/reset tests cover pairing, distinctness, both epoch walls,
  and reset disconnect restoration.
- Full daemon and relevant shielded targets compile locally on macOS arm64.
- Native Xcode DineroDPI Debug built successfully for the generic iOS
  Simulator with ShieldedProverKit linked. Android DineroDPI completed
  `testDebugUnitTest assembleDebug`, packaging arm64-v8a and x86_64 JNI builds.
- Apple-Clang ASan+UBSan: serialization, anchor-history, and derivation suites
  passed (3/3 CTest entries, no sanitizer finding).
- Post-cutover wallet validation and note-store migration also pass under
  ASan+UBSan (2/2 CTest entries, including addressed auth change).
- Homebrew-LLVM libFuzzer+UBSan smoke: 16,760,516 executions in 61 seconds,
  100 covered counters / 443 features, 53 corpus units, no crash or timeout,
  and 40 MiB peak RSS.
- Clang Debug/Release: normalized protocol-vector, derivation, and circuit test
  traces matched byte-for-byte. Both configurations passed all included tests;
  only runner timing metadata differed.
- Representative benchmark: maximum 200-spend/200-output serialized bundle
  142,508 bytes; 100 parses took 4.112 ms (0.0411 ms/bundle); 1,000 tree appends
  took 26.54 ms; 100 old-note witness constructions took 2,549.34 ms.
- Valid proof verification on this arm64 host: 200 output proofs took 1,569 ms
  (7.85 ms each); 200 spend proofs took 8,789 ms (43.94 ms each). Serial
  verification of a maximum 200+200 bundle therefore projects to about 10.36 s
  before transaction/state overhead; policy needs an explicit verification-cost
  budget or measured parallel strategy.

A dense shielded-chain fixture and on-device mobile RSS run still require
approved datasets/devices. The performance campaign records these as `NOT RUN`
rather than silently treating them as passing.

## Open issues and opportunities

### Release blockers

- Post-cutover self-shield and shielded change now use fresh wallet-owned
  addressed recipients. The note store persists the diversifier with the auth
  secret so the exact spend witness survives restart; migration defaults all
  historical notes to the zero diversifier used by their legacy convention.
- No production activation/reset height or non-empty-pool policy is approved.
- Native mobile builds and focused canonical-address tests pass. The Android
  payment parser's former 100-character ceiling was fixed and its >128-byte
  canonical address/URI round-trip is covered. Physical iPhone and Pixel builds
  launch, with baseline allocation/memory evidence captured. Remaining device
  evidence is camera/display QR confirmation, activation-boundary behavior on
  both production clients, and peak prover RSS/time during a real funded-note
  transfer. `mobile-tauri/` is a deprecated prototype and is not a blocker.
- A live-node activation preflight must demonstrate an aligned tip marker/root
  and define what happens if the pool is non-empty.

### Platform coverage still required

- TSan on Linux; macOS TSan is not a dependable release gate for this codebase.
- Genuine GCC versus Clang builds (macOS `gcc`/`g++` are Clang aliases).
- x86_64 versus arm64 using hermetic dependencies.
- Dense-activity reindex, parallel-prover equivalence, and mobile peak-memory
  measurements.
- A 24–48 hour fuzz campaign. The local runner is ready; CI is not required.

## Recommended order

1. Independently review the reset, persisted-diversifier migration,
   proof-version, and recipient-authority wallet cutover.
2. Build and test Xcode DineroDPI and Android DineroDPI on native targets;
   record address/QR parity and prover peak-memory evidence.
3. Continue the 24–48 hour fuzz campaign on the current
   commit, retaining corpus/crashes and exact build provenance.
4. Produce the approved dense-chain fixture and adopt a verification-cost budget
   informed by the measured ~10.36-second serial worst case.
5. Run Linux GCC/Clang + TSan and both CPU architectures in hermetic builders.
6. When GitHub Actions access returns, make CI invoke the same local scripts;
   avoid creating a second test definition in workflow YAML.
