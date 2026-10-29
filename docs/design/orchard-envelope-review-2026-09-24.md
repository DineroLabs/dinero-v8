# Draft Orchard envelope review disposition — 2026-09-24

This review applies to the staged component in public PR #813. It changes no
live transaction parser, activation height, legacy transaction rule or wallet.

## Changes and verification

1. **scriptSig:** profile 1 now requires an empty field for every input. The
   constructor rejects before copying; both decoder modes reject a nonzero
   script length before copying its bytes. The new rejection test failed on
   the prior implementation. Transparent authorization will use supported
   native witness programs; host admission still needs those script rules.
2. **Size:** replaced the one-MiB draft ceiling with 100,000 bytes. The library
   itself compiles assertions against the host general transaction, block and
   worst-case weight ceilings, even with tests disabled. Tests accept exactly
   100,000 bytes and reject a structurally well-framed 100,001-byte envelope
   through construction, exact decoding and stream-prefix decoding.
3. **Profile:** the outer header now uses an alias of the same wire-profile
   constant already serialized in signing D and compared with Rust's protocol
   descriptor. The accepted outer and inner profiles cannot vary independently.
   Existing domain-separated signing bytes do not change. Any future outer
   format must change this joint profile and its signing vectors together.
4. **Signatures:** checked the pinned Orchard 0.15.5 -> reddsa 0.5.2 ->
   pasta_curves 0.5.2 verification path. Signature parsing stores bytes;
   verification validates the point and scalar encodings before its equation.
   Four rejection cases cover noncanonical R and S in both spend and binding
   signatures. All eleven Rust cases and the expanded C++ suite pass.
5. **Independent outer vectors:** Python re-generated the envelope and both
   identity vectors with empty scriptSig fields; check mode passes. The inner
   signed bundle and its digest/effect fixtures are unchanged. This remains an
   independent framing/hash check, not an independent Orchard effect oracle.

Native macOS Release: both required CTest entries pass. Rust formatting and
Clippy with warnings denied pass. A separate C++ ASan/UBSan build is checked
with the existing release Rust static library; it does not instrument Rust.
LeakSanitizer is unsupported on this macOS toolchain, so no leak-check result
is claimed. Exact-source Linux CI must pass separately before qualification.

## Findings that remain integration requirements

- The component returns Orchard authorization only. Host admission must resolve
  authenticated coins, require supported native witness programs, reject
  unexpected witness, and enforce all script and stack rules. Synthetic fixture
  scripts/witnesses do not demonstrate a valid transparent spend. This gate is
  **open**, not satisfied by requiring an empty scriptSig.
- Multiple wtxids for a txid are an intentional witness-identity property.
  Admission and caches need bounded resource usage and validated witness state;
  changing a witness is not automatically a valid spend or a cache hit.
- Txid includes proof/signature bytes. A newly generated valid randomized proof
  can change it. Wallets must finish proving/signing and durably save exact bytes
  before announcing the txid or creating children. Re-proving is a replacement
  transaction. A changed parent identity invalidates the old child reference,
  not the ability to spend the parent's output at its actual outpoint.
- The signature source review and malformed-component tests are evidence about
  encoding enforcement. They do not replace cryptographic review or establish
  that a message has only one valid randomized signature.

## Reproduction

From the repository root:

```sh
cmake -S rust/orchard_backend -B build-orchard-backend -DCMAKE_BUILD_TYPE=Release
cmake --build build-orchard-backend --parallel 2
ctest --test-dir build-orchard-backend --output-on-failure
python3 rust/orchard_backend/tests/make_envelope_vector.py --check
cargo +1.91.1 fmt --manifest-path rust/orchard_backend/Cargo.toml -- --check
cargo +1.91.1 clippy --locked --release --jobs 2 --manifest-path rust/orchard_backend/Cargo.toml --target-dir build-orchard-backend/cargo --all-targets -- -D warnings
```
