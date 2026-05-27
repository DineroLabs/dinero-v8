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

Script syntax/preflight:

```bash
bash -n scripts/build-shielded-proverkit-xcframework.sh
scripts/build-shielded-proverkit-xcframework.sh
```

Result:

- Syntax check passed.
- Artifact build did not run because the repository currently lacks iOS vendored OpenSSL slice directories:
  - `third_party/openssl-3.3.2/prebuilt/ios-arm64/libcrypto.a`
  - `third_party/openssl-3.3.2/prebuilt/ios-simulator-arm64/libcrypto.a`

Artifact hash: pending until the iOS OpenSSL slices are present and the packaging script completes.
