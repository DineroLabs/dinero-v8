# Staged Orchard backend

This component starts runtime integration on the private v8.1.13 source base.
It uses upstream Orchard **0.15.5**, `BundleVersion::orchard_v2()` and the
`FixedPostNu6_2` verification key. No proof arithmetic is implemented locally.
The dependency graph is locked in Cargo.lock. It does not use the research
candidate-transaction bridge or link any of `tools/research`.

## Implemented

- Bounded, exact inner-bundle decoding into one immutable owned Rust object.
- Canonical field/point decoding, fixed proof length for action count, duplicate
  nullifier and monetary-range checks before proof verification.
- Effect and authorization commitments, anchor, flags, value balance, nullifiers
  and note commitments all obtained from that same object.
- Host-derived balance equality, every Action spend signature, binding signature,
  then one Halo2 proof verification. There is no effects-only verification cache.
- Opaque C ABI with explicit ownership; decode/facts output stays untouched on
  error, panics at fallible ABI entry points become errors.
- C++ RAII ownership and a separately constructed `VerifiedAuthorization` result.
  Parsing cannot construct that result. The result retains the exact signing
  digest used for the check.

**This is a cryptographic backend, not a Dinero transaction validator.** The
host must derive the signing message and required balance from one immutable
transaction plus authenticated prevouts; it must bind the exported effect into
that message. It must also check transparent scripts, maturity, anchors,
nullifier membership, pool balance, transaction/body identity and activation.
Raw-FFI success against a caller-supplied digest does not authenticate its origin.
Those host and chainstate callers are not wired yet.

The inner `DNORCH01` codec is still draft (8 Actions / 64 KiB limits). It is
adapted from the bounded decoder at research commit dae4a6b35; the synthetic
signed fixture and its expected digest/effect come from that same pinned tree.
No real wallet keys or note openings are in the fixtures. This does **not**
freeze the outer transaction wire, final block resource limits, signature
preimage, network profile, or activation height.

## Build and tests

Native Linux/macOS component build (no production datadir or daemon needed):

```
cmake -S rust/orchard_backend -B build-orchard-backend -DCMAKE_BUILD_TYPE=Release
cmake --build build-orchard-backend --parallel 2
ctest --test-dir build-orchard-backend --output-on-failure --no-tests=error
```

The whole-project option `DINERO_BUILD_ORCHARD_BACKEND=ON` adds this target;
it does not add a daemon validation caller or activate Orchard. Cross compilation
is deliberately rejected until toolchains are wired and qualified.

Native Mac Release: both CTest entries passed (C++ FFI/ownership checks and
8 Rust tests). Cargo fmt and clippy with warnings denied passed. Tests cover an
honest signed bundle, balance and message mismatches, each authorization check,
codec bounds/truncation/trailing bytes, duplicate nullifiers, FFI output and
ownership, and ABI layout. Corruption rejection tests are regression checks,
not a cryptographic soundness or zero-knowledge proof. Linux has its own required
`Orchard backend component` workflow; do not substitute daemon-only CI for it.

## Next integration slices

1. Final marked transaction/effects/authentication identity and host signing
   context, including transparent scripts on the same coin snapshot.
2. Orchard anchor/nullifier/frontier and pool updates staged with UTXOs, tip and
   undo in one ChainDB batch; matching disconnect, startup and reindex paths.
3. Wallet new-pool keys/addresses, witness maintenance and builder/prover,
   shield/send/unshield RPCs, encrypted note persistence and recovery from
   interrupted wallet operations.
4. Combined lifecycle/platform/load qualification and independent review.

All legacy shielded carry-forward is waived by the owner. The new pool begins
empty. Historical validation, retirement accounting and transparent funds
remain requirements. No production activation or deployment is part of this
component change.

### Fixture provenance

- `candidate-spend.bundle`: SHA-256 `2e787f3ded6427124f56093ac55a8d4922b8ad696678f92d7dfa7adb3357e2de`.
- `candidate-spend.digest`: SHA-256 `a2c4f9f0cfb91eac7bec548e8017fa8cf9ecdf7a74f1f68a0bab2ec961215d8e`.
- `candidate-spend.effect`: SHA-256 `b8a94e9e16a77bed613d2eedca8b62699b7d6d84809ed2828668ffa12b0e9511`.
