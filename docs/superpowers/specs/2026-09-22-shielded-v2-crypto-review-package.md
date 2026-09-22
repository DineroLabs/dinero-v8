# Shielded v2: independent cryptographic review package

Prepared 2026-09-22 for an independent reviewer. Research branch `claude/shielded-v2` (commit range up to
5bd8cdd92). Nothing here is wired into consensus; no activation, sunset, migration or approval exists.
This package states exactly what is claimed, what is measured, and what is NOT established.

## 1. What is being asked

Review, against an arbitrary (malicious) prover, the soundness and zero-knowledge of:
1. the **bundle statement** (spec §3.1 as amended by §10.2), and
2. the **E-less Spartan profile** used to prove it (`omit_error_term`), including transcript/profile
   separation, encoding rules, and the trusted verifier context binding.

Deliverable: a written verdict per item in §6, with any required change to the statement, the
profile, the encodings or the tests.

## 2. The statement (exact)

Hashes are Poseidon-2 over the secp256k1 scalar field (`poseidon2_gadget`, the same primitive the
live circuits use). Public inputs, allocated first and in this order:
`sighash, vb_pos, vb_neg, (anchor_i, nullifier_i) for i in spends, commitment_j for j in outputs`.

Per spend i (private: `ask, nfk, d, value, rcm, leaf_index, merkle_path[32]`):
- `hk = P(ask, HK_TAG)` with `HK_TAG` = ASCII `"DIN/v7/shielded/v2/hk"` left-aligned in 32 bytes, a circuit constant
- `pk_d = P(hk, d)`
- `nfk_c = P(nfk, NFK_TAG)` with `NFK_TAG = NullifierKeyTag()`, a circuit constant
- `pk = P(pk_d, nfk_c)`
- `cm = P(P(P(ADDR_TAG, P(d, pk)), value), rcm)` with `ADDR_TAG = AddrBindTag()`, a circuit constant
- Merkle path of depth 32 from `cm` to `anchor_i` (public), using `leaf_index` bits for direction
- `nullifier_i = P(nfk, leaf_index)` (public)
- `value` range-checked to 64 bits

Per output j (private: `value, pk, rcm, d`): `cm_j = P(P(P(ADDR_TAG, P(d, pk)), value), rcm)` equals the public
`commitment_j`; `value` range-checked to 64 bits.

Balance: `vb_pos + Σ value_i = vb_neg + Σ value_j`, with `vb_pos`, `vb_neg` range-checked to 64 bits and derived
by the verifier from the bundle's signed `value_balance` (exactly one of them non-zero).

Sighash binding: `sighash` is a public input that appears in one constraint (`sighash · 1 = sighash`) so its
column is non-zero in the matrices, and it is absorbed into the Fiat–Shamir transcript first
(`BindBundleTranscriptV2`: sighash, vb_pos, vb_neg, n_spends, n_outputs, then each anchor/nullifier, then each
commitment). Transcript domain string: `dinero.shielded.bundle.v2`.

Reference implementation of the statement: `tests/spike/bundle_circuit_v2.h` (spike) and the production form
specified in the phase-1 plan Task 3 (`BuildBundleCircuitV2`). Measured size for 2-in-2-out: 56,790 constraints.

## 3. The E-less profile (exact)

Spartan (sum-check over relaxed R1CS `A·z ∘ B·z = u·(C·z) + E`) with Hyrax commitments, as in
`src/zk/zkvm/r1cs_spartan.{h,cpp}`. The profile is selected by the caller (`omit_error_term = true` on
prove, verify and deserialize); it is never inferred from proof bytes.

Prover (`r1cs_spartan.cpp:428-436`, `:558`): requires `u == 1` and `E ≡ 0`, otherwise returns an empty proof;
emits no `comm_E` (empty commitment, zero rows) and no `eval_E`; sets `has_error_term = false`.

Verifier (`r1cs_spartan.cpp:611-616`, `:633`, `:794`): requires `has_error_term == false`, `comm_E` empty with
`n_rows == 0`, `Ez_claim == 0`, `u == 1`; skips the E identity check (there is no E) and the E opening; the
outer sum-check final check therefore verifies `eq(τ,rx)·(Az·Bz − Cz − 0) == outer_final`. Everything else
(τ derivation, outer and inner sum-checks, M̃(rx,ry) evaluation from the verifier's own matrices,
public-input binding `z̃(ry) = io(ry) + W̃(ry)`, Hyrax opening of W) is unchanged from the legacy profile.

Serialization (`SpartanProof::serialize/deserialize`): with the profile, `comm_E` and `eval_E` are absent and
trailing bytes are rejected; without it, the legacy byte layout and its accepted language are unchanged
(differential regression `tests/zk/test_spartan_profile_compat.cpp`).

Trusted verifier context (`R1CSVerifierMatrices`, `src/zk/zkvm/r1cs_verifier_matrices.{h,cpp}`): immutable
(private state, `Build(const R1CS&)` factory, const accessors); carries `circuit_hash =
spartan_hash_r1cs_structure(cs)` and CSR matrices. A verify that receives one requires
`proof.circuit_hash == context.circuit_hash()` and, if the caller supplies an expected hash,
`expected == context.circuit_hash()`, before dimensions (`r1cs_spartan.cpp:745-748`). The hash covers
constraint count, variable count and every term (index, coefficient) of A, B, C; it does not cover the
public-input layout separately (the layout is implied by the R1CS the hash was computed from).

## 4. Claims (what the author asserts)

C1. With `u = 1` and no error term, the relation verified is exactly ordinary R1CS satisfiability
`A·z ∘ B·z = C·z`; dropping the E commitment/opening removes no check that constrains an arbitrary prover,
because the verifier fixes `Ez_claim = 0` itself and never trusts a prover-supplied E.
C2. The two profiles are domain-separated: a proof made under one is rejected under the other
(`has_error_term` gate, distinct byte layouts, and the E identity check in the legacy verifier).
C3. Zero-knowledge is unchanged for W: Hyrax's blinding on `comm_W` and the IPA are the same as in the legacy
profile; the removed E commitment carried no witness information (E ≡ 0).
C4. The sighash is bound both algebraically (non-zero column) and via the transcript, so a proof cannot be
transplanted to another transaction.
C5. The statement's ownership relation (Poseidon preimage of `ask`) cannot be satisfied by a sender who knows
`pk_d_spend` and `nfk_c` from the address, nor by a full viewer holding `hk` and `nvk`.

## 5. Evidence and its limits

- Profile tests (regression, not proofs): honest accepted; unsatisfied witness rejected under both profiles;
  non-zero E refused by the prover and a tampered `Ez_claim` rejected; cross-profile rejection both ways;
  tampered bytes rejected; trailing bytes rejected only under the profile; matrix evaluation reached by an
  invalid case (proof for circuit A verified against circuit B of equal dimensions) and rejected on both the
  walk and CSR paths; parallel and CSR evaluations verdict-identical on 17k–20k-constraint fixtures with
  mixed coefficients; same-dimension twin circuit refused by identity. 27 tests, `test_spartan_soundness`.
- Statement tests (spike): 8 negatives unsatisfiable (wrong `ask`, viewer with `ask = 0`, wrong `nfk`, wrong `d`,
  value + 1, fee + 1, wrong leaf, wrong sibling); public-input mutations rejected; 0 of 1,342 (M4) and 0 of
  664 (Linux) proof-byte mutations of live v1 transactions accepted by the node (that last item concerns the
  v1 verifier, not v2).
- Performance is irrelevant to this review and is reported elsewhere.
- Not established by any of the above: soundness of C1 as a statement about all provers (the tests exercise
  specific dishonest strategies), the knowledge-soundness/extractor argument for the modified transcript,
  zero-knowledge of the E-less profile as a formal property, canonical-encoding completeness (every accepted
  byte string has one parse), and the adequacy of the structure hash as the sole identity of the trusted
  context.

## 6. Questions for the reviewer (each needs a written answer)

Q1. Is dropping the E commitment/opening sound for `u = 1`? Specifically: with `Ez_claim` fixed to 0 by the
verifier, can a prover of a false statement satisfy the outer final check `eq(τ,rx)·(Az·Bz − Cz) = outer_final`
together with the inner sum-check and the Hyrax opening of W with non-negligible probability?
Q2. Is the transcript sequence (u, comm_W rows, [no comm_E], circuit hash, τ, sum-check messages, claims, ρ, …)
still a sound Fiat–Shamir instantiation when the `hE` absorptions are absent? Is the profile itself absorbed
adequately (it is not absorbed explicitly; the byte layout and `has_error_term` gate separate it)? Should a
profile byte be absorbed?
Q3. Zero-knowledge: does omitting E leak anything about W through `Ez_claim = 0` or the outer claims? Does
the Hyrax/IPA blinding on W alone suffice for the honest-verifier ZK the design assumes?
Q4. Encoding: are there distinct byte strings that deserialize to the same proof (or the same proof from
different strings) under either profile, and does that matter for any consumer (caching keys, txids)?
Q5. Trusted context identity: is `spartan_hash_r1cs_structure` (structure + all coefficients) sufficient as
the identity anchor, or must the profile and public-input layout be included explicitly? Is deriving the
expected hash from the context (rather than from proof data) the right direction of trust?
Q6. Statement: is `pk = P(P(P(ask,HK), d), P(nfk, NFK))` a sound ownership relation (unforgeable without `ask`,
unlinkable across `d`, no key-recovery path from viewing material)? Is the nullifier `P(nfk, leaf_index)`
adequate against double-spend and against nullifier prediction by the sender who knows `nfk_c` but not `nfk`?
Q7. Public-input binding: is the `z = (1, io, W)` split with io reconstructed by the verifier (CONFIRMED-CRIT-05)
sound for this statement's io layout (variable-length io by shape)?
Q8. Anything that must change before any consensus wiring.

## 7. How to reproduce

Build: `cmake --build build-spike --target test_spartan_soundness spike_shielded_v2_bench`.
Run: `./build-spike/test_spartan_soundness` (27 tests); `./build-spike/spike_shielded_v2_bench --selftest`,
`--negatives`, `--profile-soundness`. Source: `src/zk/zkvm/r1cs_spartan.{h,cpp}`, `src/zk/zkvm/r1cs_verifier_matrices.{h,cpp}`,
`tests/zk/test_spartan_profile_compat.cpp`, `tests/spike/bundle_circuit_v2.h`, `tests/spike/shielded_v2_bundle_bench.cpp`.
