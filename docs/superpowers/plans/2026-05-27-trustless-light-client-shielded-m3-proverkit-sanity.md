# M3 ShieldedProverKit Sanity Log

Branch: `codex/m3-shielded-proverkit`

## Native ABI Slice

Commit:

- `ecc58f755307f3222d036950d13b969958bac095` - `feat(shielded): expose native prover kit ABI`

Local host verification:

```bash
cmake -S . -B /private/tmp/dinero-v8-m3-proverkit-build
cmake --build /private/tmp/dinero-v8-m3-proverkit-build --target test_shielded_prover_kit -j8
ctest --test-dir /private/tmp/dinero-v8-m3-proverkit-build --output-on-failure -R '^ShieldedProverKit$'
ctest --test-dir /private/tmp/dinero-v8-m3-proverkit-build --output-on-failure -R '^(ShieldedProverKit|ShieldedValidation|ShieldedWitness|ShieldedCircuit)$'
cmake --build /private/tmp/dinero-v8-m3-proverkit-build --target dinerod dinero-cli -j8
```

Results:

- `ShieldedProverKit`: pass, 9 ABI tests.
- Combined shielded gate: pass, 4/4 CTest entries (`ShieldedCircuit`, `ShieldedValidation`, `ShieldedProverKit`, `ShieldedWitness`).
- `dinerod` and `dinero-cli`: build pass. Only pre-existing warnings surfaced.

ABI coverage:

- Native commitment helper matches `NoteCommitment(d, pk_note, value, rcm)` for non-zero `d`.
- Native nullifier helper matches `DeriveNoteSpendKey(rcm)` plus `ComputeNullifier(sk_note, leaf_index)`.
- Native unshield bundle validates through daemon `ValidateShieldedBundle`.
- Recipient script mutation fails with `BindingSigInvalid`.
- Recipient value mutation, with fee offset to preserve value balance, fails with `BindingSigInvalid`.
- Fee-only mutation fails with `ValueBalanceMismatch`.
- Wrong Merkle path fails build or daemon validation.
- `fee_una >= value_una` returns an explicit C ABI error.
- Malformed serialized tx returns a deserialize error and remains free-safe.

Wire-envelope note:

- Zero-input one-output unshield envelopes must include the SegWit marker/flag bytes (`00 01`) before the empty input count. Without that, the legacy parser cannot distinguish `vin_count=0, out_count=1` from the marker/flag prefix.

## Packaging Slice

Script added:

```bash
scripts/build-shielded-proverkit-xcframework.sh
```

OpenSSL prerequisites:

```bash
OPENSSL_VERSION=3.3.2 OPENSSL_REBUILD=1 bash scripts/build-openssl-vendored.sh
bash scripts/build_openssl_ios.sh
```

Results:

- macOS OpenSSL 3.3.2 arm64 cache rebuilt at `third_party/openssl-3.3.2/prebuilt/macos-arm64/`.
- iOS device OpenSSL 3.3.2 arm64 cache rebuilt at `third_party/openssl-3.3.2/prebuilt/ios-arm64/`.
- iOS simulator OpenSSL 3.3.2 arm64 cache rebuilt at `third_party/openssl-3.3.2/prebuilt/ios-simulator-arm64/`.
- All three caches contain `libcrypto.a`, `libssl.a`, and `include/openssl/`.
- `lipo -info` reports arm64 for all six OpenSSL archives.
- The vendored OpenSSL cleanup scripts explicitly exclude `prebuilt/`; `find ... -prune ... -delete` is not used because `-delete` implies `-depth` and would evaluate prune too late.

Script syntax/preflight:

```bash
bash -n scripts/build-shielded-proverkit-xcframework.sh
scripts/build-shielded-proverkit-xcframework.sh
```

Result:

- Syntax check passed.
- First artifact run built both native slices but failed after compile because macOS system Bash lacks `mapfile`.
- The packaging script now uses a Bash 3.2-compatible `while read` archive collection loop.
- Second artifact run completed successfully.
- `xcodebuild -create-xcframework` wrote `artifacts/ShieldedProverKit.xcframework`.
- `artifacts/ShieldedProverKit.xcframework.zip` was generated and hashed.
- `lipo -info` reports arm64 for both packaged `libShieldedProverKit.a` slices:
  - `build-shielded-proverkit-ios/slices/ios-arm64/libShieldedProverKit.a`
  - `build-shielded-proverkit-ios/slices/ios-simulator-arm64/libShieldedProverKit.a`

Artifact hash:

```text
df2184868b984ef49f474b8b0d199f403317a044f742a4bfd9ca2cf44d309a6b  /private/tmp/dinero-v8-m3-proverkit/artifacts/ShieldedProverKit.xcframework.zip
```
