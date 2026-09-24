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
- Owned immutable C++ signing context. Verification derives the draft digest
  internally from that context and the effect exported by the same parsed bundle;
  the C++ API has no caller-digest verification overload. Context construction
  checks duplicate inputs, bounded sizes, monetary sums and cross-pool balance.

**This is a cryptographic backend, not a Dinero transaction validator.** The
host must construct the signing context from the actual immutable transaction
and authenticated prevouts, in transaction order. The C++ wrapper binds that
context to its own parsed bundle effect; it does not authenticate the supplied
coin metadata. The host must also check transparent scripts, maturity, anchors,
nullifier membership, pool balance, transaction/body identity and activation.
Raw-FFI success against a caller-supplied digest does not authenticate its origin.
Those host and chainstate callers are not wired yet.

The inner `DNORCH01` codec is still draft (8 Actions / 64 KiB limits). The
eight-action protocol-v1 limit is named in Rust and C/C++; changing it requires
a protocol decision and a new ABI version, not merely wider arrays. It is
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

Native Mac Release (Rust 1.91.1): both CTest entries passed (C++ FFI/ownership checks and
9 Rust tests). Cargo fmt and clippy with warnings denied passed. Tests cover an
honest signed bundle, balance and message mismatches, each authorization check,
codec bounds/truncation/trailing bytes, duplicate nullifiers, FFI output and
ownership, and ABI layout. The C++ lane also compares its derived digest to an
independently generated saved fixture, rejects 13 transaction-context changes,
rejects a changed encrypted payload using its own newly derived digest, and
checks signed balance direction and context resource limits. Corruption rejection tests are regression checks,
not a cryptographic soundness or zero-knowledge proof. Linux has its own required
`Orchard backend component` workflow; do not substitute daemon-only CI for it.
This is a qualification requirement, not a claim about GitHub branch-protection
settings. The workflow covers all PR base branches and pushes to main,
dinero-main and codex branches when component or host money-limit inputs change.

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

### Draft signing context boundary

The context owns copies of the network/genesis/branch domain, lock time, ordered
resolved inputs (outpoint, sequence, amount, script), ordered transparent outputs
and explicit fee. Version 7, fee-presence and the selected draft profile are
fixed by this interface. The derived digest includes those fields, the action
count and the effect commitment. The latter includes the bundle's encrypted
payloads. Verification always derives this digest again from the immutable data;
caller-provided digest bytes cannot select what the C++ wrapper verifies.

The low-level C ABI remains a cryptographic primitive and accepts a digest. It
must not be called directly as transaction validation. No source-tree consensus
caller exists yet. A transaction-to-context adapter, canonical outer encoding,
trusted chain-parameter selection, and coin-view validation are still required.

The exact crate selected by Cargo.lock is the crates.io `orchard` 0.15.5 release,
not a Git tag dependency or a patched local fork. Locked builds select the
`orchard_v2()` bundle profile and `FixedPostNu6_2` circuit key explicitly;
`orchard_v3()` is not used. This pin does not by itself qualify the future wallet
flow, platforms or consensus integration.

### Review hardening

The key version is derived from the selected bundle version. The Rust facts
export test starts with zeroed output and checks all fields, rather than
pre-seeding the expected result. Both Rust and C++ lanes reject corrupted proof,
spend-signature and binding-signature bytes. Both lanes remain mandatory.

Free consumes its handle exactly once and now returns a status, including on a
caught destructor panic. Raw callers must never retry it. The C++ RAII deleter
terminates on a cleanup failure rather than continuing with unknown state; no
such panic is known in the pinned dependency graph. A synthetic destructor test
checks the shared boundary. Unwind containment does not catch process aborts,
out-of-memory aborts, invalid foreign pointers or double frees.

The C++ lane compares Rust-exported action and monetary bounds against its own
constants; its monetary bound is also statically compared with the host constant
read at CMake configuration. The Rust ABI test checks all named C status values.
The compiler is pinned to 1.91.1 and workflow actions to commit IDs. The component
workflow also has a root Linux job that enables the backend, builds dinerod and
the Orchard C++ target, and requires both Orchard test entries before running
them. That job qualifies build coexistence, not full transaction integration.
No mobile/cross-platform qualification is claimed. Dependency advisory scanning and
an independently derived upstream commitment vector remain open review items.

### Main-project build qualification

The root build has been configured on native macOS with the option enabled,
and its Orchard target compiled in a fresh directory. Both root-registered
Orchard CTest entries passed (nine Rust cases plus C++ checks), using the
project's vendored OpenSSL 3.5.7 artifacts read-only. This is not a clean full
daemon or full test-suite claim. The Linux root job builds the daemon too;
inspect its exact-source artifact before claiming that gate passed.

## Draft outer transaction envelope

`TransactionEnvelope` now owns a bounded, canonical outer frame, transparent
inputs/outputs, explicit fee and the parsed Orchard bundle. It derives its own
signing context from those fields and matched previous outputs. An authorization
result retains the exact canonical bytes, txid/wtxid and Orchard authorization;
it is explicitly not a transparent-script or chainstate-validity result.

The draft starts with numeric version 7, two zero bytes, `DNORCHTX`, profile byte
1 and a four-byte payload length. The two zero bytes distinguish this candidate
from ordinary transparent v7 transactions. No shared Transaction parser or live
admission path is changed; old-binary reindex/storage exclusion is still unproven.
This codec is **not frozen or activated**. See [transaction envelope](TRANSACTION_ENVELOPE.md).

The C++ test compares the bytes and both identities with an independent Python
serializer using the existing synthetic signed bundle. It checks exact/prefix
framing, two concatenated frames, every truncated prefix, marker/count/size
rejections, fee-presence, input/coin correspondence, ownership and cryptographic
rejection. One-sided/zero-transparent flow tests establish encoding shape only;
valid shield/send/unshield proofs and wallet flows remain unimplemented.
