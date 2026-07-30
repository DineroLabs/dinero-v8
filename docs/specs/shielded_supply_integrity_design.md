# Shielded Pool — Cross-Bundle Supply Integrity (historical design)

> **Nonnormative and partly superseded.** This document records the design
> process. Its planned aggregated Bulletproof construction and several binding
> equations do not describe deployed consensus. See
> [`shielded_protocol_v1.md`](shielded_protocol_v1.md) for the traced v1
> protocol. Do not implement from this document.

**Status:** **PATH C WAVES 1 + 2 SHIPPED 2026-04-27** (same-day
implementation after approval). Wave 3 has structural test coverage
in tree (`ShieldedReindexEquivalence`, `ShieldedPoolRoundTrip`) and
shield + unshield e2e RPC ctests; remaining Wave 3 work is operator
calendar (testnet/mainnet activation-height selection + soak) plus
the shielded→shielded `wallet.transfer` RPC, which the consensus
layer already supports but no wallet entry point exposes.

**Implementation deviation from memo §3.2/§4:** the memo specifies
two custom generators `(V, R)`. The implementation collapsed to a
single custom generator `V` plus the standard secp256k1 generator
`G` (used as the blinding base). Documented and justified in
`include/consensus/shielded/pedersen_generators.h` lines 9-22 — this
matches Sapling's actual design, matches libsecp256k1-zkp's
`secp256k1_pedersen_commit(commit, blind, value, gen_h)` API shape,
and the security argument is unchanged (no known DL relation between
G and an honestly-derived V). Wherever this memo says "R", read "G".

Paths A and B are closed and should not be revisited unless a
fundamental constraint changes.
**Author:** Phase 3 wave 2 audit, 2026-04-27.
**Supersedes:** the "future phase" hand-wave in
`include/consensus/shielded/shielded_validation.h`.

## Shipped artifacts (post-audit)

- Generator V: `include/consensus/shielded/pedersen_generators.h` +
  `src/consensus/shielded/pedersen_generators.cpp`. DST
  `"DIN/v7/shielded/cv/V/v1"` per §4 #1, derived via
  `secp256k1_generator_generate`. `PedersenGeneratorsReady()` gates
  consumers.
- Pedersen commit primitive:
  `src/consensus/shielded/pedersen_commit.cpp` (49 LoC).
- Range proofs: `src/consensus/shielded/range_proof.cpp` (203 LoC) +
  `aggregated_range_proof` field on ShieldedBundle. Per-bundle
  aggregation cap `kMaxBPAggregationDepth = 32` lives in
  `shielded_tx.h:138`. (Borromean rangeproof bytes today,
  reinterpretable as Bulletproofs aggregated proof when libsecp
  ships the verifier — see comment at `shielded_tx.h:105-107`.)
- Wire format: `ShieldedSpend.cv` and `ShieldedOutput.cv` are
  33-byte compressed Pedersen commitments
  (`shielded_tx.h:54,59,72`). `ShieldedBundle.bvk_commitment`
  (33 bytes) at line 113. `ShieldedBundle.binding_sig` widened to
  64-byte BIP340 Schnorr at line 116.
- Binding signature: `src/consensus/shielded/binding_sig.cpp` (251
  LoC). `ComputeBindingSighash` per §4 #3, `SignBinding` via
  `secp256k1_schnorrsig_sign32`, `VerifyBinding` does
  `secp256k1_pedersen_verify_tally` for the balance equation
  followed by `secp256k1_schnorrsig_verify`. Wired into the
  validator at `shielded_validation.cpp:149`.
- Activation gates UNCHANGED, as required by §6: mainnet
  UINT32_MAX (`chainparams_impl.cpp:78`), testnet UINT32_MAX
  (`chainparams_impl.cpp:241`), regtest 0 (`chainparams_impl.cpp:321`).
- Reindex parity: `tests/consensus/test_shielded_reindex_equivalence.cpp`
  (705 LoC) registered as `ShieldedReindexEquivalence`. Round-trip
  coverage via `ShieldedPoolRoundTrip`.

## Remaining Wave 3 work

1. **Shielded→shielded `wallet.transfer` RPC.** Consensus already
   accepts mixed bundles (any combination of spends/outputs). No
   wallet entry point currently constructs them. Phase 5-shaped
   work, not consensus.
2. **Phase 0 test-vector regeneration against v0.3.0 schema.**
   Required before activation-height selection. Cv / range-proof /
   bvk / Schnorr-binding-sig hex needs to be filled in
   `docs/specs/shielded_derivation.md` §8.
3. **Testnet activation-height selection + release cut.** Pick a
   near-future testnet block, set `chainparams_impl.cpp:241`, cut
   release, soak ≥7 days with shielded txs flowing.
4. **Mainnet activation-height selection + release cut.** Pick
   ≥2-week-future mainnet block, set `chainparams_impl.cpp:78`,
   cut release, fleet updates before activation.

The original effort estimate (§7) said "~2 weeks code, ~3-4 weeks
to mainnet activation." Code time used: same-day. Calendar time
unchanged (≥7-day testnet soak is the long pole).

---

## 1. The problem in one paragraph

A v5 transaction's `bundle.value_balance` is a **plaintext int64** on the
wire. Consensus checks `bundle.value_balance == ctx.transparent_value_delta`
where `transparent_value_delta = transparent_in - transparent_out - fee`.
Nothing inside the per-spend or per-output ZK proofs binds the *shielded*
side of the equation to that plaintext. A sender can publish:

```
spend value = 10 una     (note commits to 10, proof verifies)
output value = 10000 una (note commits to 10000, proof verifies)
bundle.value_balance = -9990  (claims 9990 una flowing from shielded to transparent)
transparent_value_delta = -9990  (sender's transparent envelope agrees)
```

…and consensus accepts it. The chain mints 9990 una. **This is the live
correctness gap that keeps `chainparams.shielded_activation_height` at
UINT32_MAX on mainnet/testnet.**

The Phase 1 activation gate prevents this from being a current risk. The
gate cannot be lowered until consensus actually verifies the shielded
side of the value equation.

---

## 2. The fix-shape Sapling uses (and why it doesn't drop in)

Sapling solves this with **Pedersen value commitments + Schnorr-style
binding signature**:

1. Each spend / output additionally publishes a **value commitment**
   `cv = v·V + rcv·R` where `V`, `R` are independent generators on Jubjub
   and `rcv` is per-note randomness in the witness.
2. The spend / output circuit takes `(v, rcv)` as private inputs, emits
   `cv` as a public input, and proves `cv == v·V + rcv·R`.
3. The note commitment `cm` already commits to `v` (via the existing
   Poseidon path), so binding `cv` to `v` *inside the proof* binds `cv`
   to the same value the note's recipient will see at unshield time.
4. The sender chooses `rcv` values such that
   `bsk = sum(rcv_spend) - sum(rcv_output) (mod q)` is a known scalar.
5. The sender publishes `binding_sig = Schnorr-sign(bsk, sighash || value_balance)`
   using `R` as the signing base. The corresponding public key is
   `bvk = bsk·R = sum(cv_spend) - sum(cv_output) - value_balance·V`.
6. Consensus reconstructs `bvk` from the published cvs and the plaintext
   `value_balance`, then verifies the Schnorr signature against `bvk`.

The equation `bvk = (sum_cv_spend - sum_cv_output) - value_balance·V` is
algebraically equivalent to "the V-component of the bundle's net cv is
exactly value_balance." Any sender who publishes mismatched values
produces a bvk for which they don't know the discrete log under R,
and the binding signature fails.

**Why it doesn't drop in for Dinero:** Sapling's Pedersen lives on
**Jubjub**, a curve embedded in the BLS12-381 scalar field, designed
specifically to be cheap inside R1CS. EC ops in Jubjub R1CS cost
~500 constraints per scalar multiplication.

Dinero is on **secp256k1 with Spartan over its scalar field**. Costs
inside R1CS, measured against the current `ec_gadget.cpp`:

| Op | Constraints |
|---|---|
| `ec_assert_on_curve` | ~3,200 |
| `ec_add_unsafe` | ~4,500 |
| 256-bit scalar mul (double-and-add) | ~1.2M |

In-circuit `cv = v·V + rcv·R` binding would add **~2.4M R1CS constraints
per spend / output**, vs. the current ~8,400-constraint spend circuit.
A spend prove that takes ~250ms today would take **on the order of
minutes** — unacceptable for wallet UX and harmful to fleet block-validation
load.

This is the central architectural problem. Sapling-the-pattern is right;
Sapling-the-implementation doesn't fit.

---

## 3. Three resolution paths

### 3.1 Path A — Embedded ZK-friendly curve

Find or define a Jubjub-shape curve over the secp256k1 scalar field, use
it for value commitments, do EC ops on it inside R1CS at Jubjub-class
cost.

**Status of the search:** secp256k1's scalar field is `q = 2^256 - 2^32 - 977 - 1` (close-to-prime; well-studied). Curves embedded over `q` exist
in principle (any prime-order subgroup of an Edwards or Montgomery curve
defined over `GF(q)`). Picking parameters needs:
- Cofactor 1 or 8 (Jubjub uses 8 with a complete-Edwards form).
- Group order `r ≈ q` such that the discrete-log assumption is credible.
- Generators with no known discrete-log relations (nothing-up-my-sleeve
  derivation).

**Cost:** Multi-week cryptographic engineering. New gadgets, new
generators, new test vectors, new audit surface. Probably needs an
external review pass before anything ships.

**Pros:** structurally cleanest. Closely mirrors Sapling's well-studied
design. Future privacy work (full memo encryption, viewing keys) reuses
the same curve.

**Cons:** biggest scope. Most novel cryptography (we're picking curve
parameters that haven't seen production use). Adds a second curve to the
codebase forever — every shielded operation has to track which curve it's on.

### 3.2 Path B — Native Pedersen + Schnorr-binding (no in-circuit EC math)

Keep the cv commitment on **secp256k1**, but compute it *natively*
(outside the proof) and use the proof to bind cv to the same `v` and
`rcv` the circuit knows about.

**Wire format change:**
```
ShieldedSpend  += Hash cv          // 32-byte x-only secp256k1 point
ShieldedOutput += Hash cv          // same
ShieldedBundle.binding_sig is now a 64-byte Schnorr signature, not the
  current 32-byte SHA-256 structural tag.
```

**Witness change:** OutputWitness and SpendWitness gain a `Hash rcv` field.

**Circuit change:** the spend/output circuits gain *one* additional
constraint per proof — they expose the Poseidon hash of `(v, rcv)` as a
public input:
```
cv_blind_hash = Poseidon(v, rcv)  // both private witnesses
// cv_blind_hash exposed as public input alongside nullifier/anchor (spend)
// or commitment (output)
```

**Consensus check (outside circuit):**
- For each spend/output, recompute the V-component of cv from a Poseidon
  preimage proof of `(v, rcv)`. The clever bit: cv is `v·V + rcv·R`, and
  the proof publishes `Poseidon(v, rcv)`. To bind these, the Schnorr
  binding sig is constructed against a key `bvk_pub` derived as
  `sum(cv_spend) - sum(cv_output) - value_balance·V`, with the message
  also incorporating `Poseidon(v, rcv)` for each note.

This sketch needs sharpening — specifically, the Schnorr message has to
bind every cv to the matching circuit's `(v, rcv)` Poseidon hash so a
sender can't publish (cv1 from circuit1, cv2 from circuit2). One concrete
construction:

> Per output proof, public input includes
> `tag_o = Poseidon(cv_output, "out_v1")` — the on-wire cv Poseidon-bound
> to the proof's circuit instance. Per spend proof, same with
> `tag_s = Poseidon(cv_spend, "spend_v1")`. Binding sig signs
> `(sighash, value_balance, tag_s_1, …, tag_s_n, tag_o_1, …, tag_o_m)`.
> Now any cv-mismatch between the circuit's (v, rcv) and the published
> cv flips a tag, the binding sig sighash changes, signature fails.

Wait — this still doesn't work without the circuit *also* binding to cv.
Otherwise the sender can produce a proof that `(v, rcv)` Poseidon-hash
to some scalar, and *separately* publish a cv they computed natively from
*different* `(v, rcv)`. The Poseidon hash of (v, rcv) doesn't tell
consensus what v actually was.

The fix is to have the circuit compute `cv = v·V + rcv·R` at the level
of *Poseidon-of-the-x-coordinate* — i.e., the circuit proves
`Poseidon(cv.x) == cv_hash_public_input`, and `cv.x` is computed as the
x-coordinate of `v·V + rcv·R` *inside* the circuit. That puts us back
into expensive EC math.

**Conclusion on Path B:** the "no in-circuit EC math" version is harder
to make sound than it looks. Either we accept the in-circuit cost
(unacceptable per §2) or we need a different cryptographic primitive.
**Path B as sketched here is a dead end without further design.**

### 3.3 Path C — Range proofs (Bulletproofs-style)

Replace the per-output `range_check_limb` with Bulletproofs range proofs
over secp256k1, and bundle them into a single per-block aggregated proof.
Bulletproofs natively give:
- A Pedersen commitment `cv = v·V + rcv·R` published alongside the proof
- A range proof that `0 ≤ v < 2^64`, byte-bound to `cv`
- Aggregation: sum of N range proofs is O(log N) extra size

**Pros:** secp256k1-native (no embedded curve). Open-source production
code exists (libsecp256k1-zkp has Pedersen + Bulletproofs). The benchmark
in `src/zk/zkvm/bench_zkvm.cpp:163` already calls `secp256k1_pedersen_commit`
— the primitive is in the build.

**Cons:** Bulletproofs aren't Spartan. Adding a second proof system to
shielded validation increases code surface. Verifier cost for one
Bulletproof range proof is ~3-5ms on Mac M-series — small individually,
but each shielded output now carries two proofs (Spartan output proof +
Bulletproof range proof) unless we replace the Spartan proof's
`range_check_limb` with the BP version.

**Open question:** does Bulletproofs over secp256k1 give us the
Schnorr-binding-sig-shaped bvk verification consensus needs for
cross-bundle balance? Yes — the Pedersen commitments in BP are exactly
the cv values Sapling uses. The aggregated range proof binds each cv to
its v range. Consensus then does the standard `bvk = sum(cv_spend) -
sum(cv_output) - value_balance·V` check and verifies Schnorr.

**This looks like the most viable path.**

---

## 4. Recommendation

**Path C — Bulletproofs range proofs + native Pedersen + Schnorr binding sig.**

Reasoning:
- The Pedersen primitive is already linked (libsecp256k1-zkp).
- It's secp256k1-native — no new curve to audit.
- Range proofs and value commitments are the two things we need; BP
  gives both in one mechanism.
- Cost is acceptable: aggregated BP for N outputs is O(log N) bytes and
  ~10-20ms verifier per block at typical N.
- Schnorr signature against bvk is straightforward libsecp256k1 code.

Sub-path: keep the Spartan output / spend proofs for the *commitment*
and *Merkle path* parts (their job remains "prove ownership and
membership"). Drop `range_check_limb` from the Spartan circuits; the
Bulletproof range proof on cv covers it. Net cost per output: Spartan
output proof shrinks (~64 fewer constraints from removing range_check_limb)
+ one BP range commitment. Aggregated BP per block dominates.

**Pre-implementation lock-ins (must be frozen before any code lands):**

1. **Generator derivation.** `V` and `R` derived nothing-up-my-sleeve
   from RFC 9380 SSWU on secp256k1:
   ```
   V = HashToCurve("DIN/v7/shielded/cv/V/v1")
   R = HashToCurve("DIN/v7/shielded/cv/R/v1")
   ```
   Pin both as 32-byte x-only constants in
   `include/consensus/chainparams.h` so any divergence between
   reference and implementation surfaces at link time, not at
   block-validation time. Generator-derivation test vectors land in
   the same commit as the constants.

2. **Wire format.** ShieldedSpend and ShieldedOutput each gain
   `Hash cv` (32 bytes x-only). ShieldedBundle gains an aggregated
   Bulletproof range proof (variable length, varint-prefixed) and a
   per-bundle aggregation cap (see #5). `binding_sig` widens from 32
   bytes (current SHA-256 tag) to **64 bytes** (Schnorr signature
   over secp256k1: 32-byte R, 32-byte s). All bytes-affecting →
   **bumps the spec to v0.3.0 and forces Phase 0 test-vector
   regeneration before activation**.

3. **Binding signature domain separation.** The binding-sig sighash
   MUST commit to *every* component that could be malleated:
   ```
   binding_msg = "DIN/v7/shielded/binding/v1"
              || tx_sighash                     // BIP143-style over v5 envelope
              || value_balance_LE_8bytes
              || fee_LE_8bytes
              || varint(num_spends)
              || cv_spend[0].x || ... || cv_spend[n].x   // canonical-sorted order
              || varint(num_outputs)
              || cv_output[0].x || ... || cv_output[m].x
   ```
   Without all six components, bundle-swap and partial-malleability
   attacks become possible. The exact transparent `tx_sighash` variant
   (which fields are committed, in what order) MUST be locked in this
   spec before code, and the choice MUST be the SegWit-style BIP143
   variant we already use elsewhere — pin it explicitly so reviewers
   don't have to derive it.

4. **Strict version gating.** Because this is consensus-affecting:
   - Parser must reject any v5 transaction whose bundle byte length
     doesn't decode cleanly under the v0.3.0 schema. **No "soft
     interpretation" of older shapes.**
   - Activation height check fires BEFORE any wire-format parsing of
     post-v0.3.0 fields, so pre-activation v5 txs (currently
     impossible to mine on mainnet anyway) don't accidentally invoke
     the new code paths.
   - Test vectors are frozen at the moment of activation-height
     selection. Any post-freeze change to a generator constant, hash
     domain, or canonical ordering is a chain split.

5. **Bulletproof aggregation cap.** Aggregated BP verification is
   `O(N · log N)` group ops where N is the number of cvs in the bundle.
   Without a cap, a malicious miner could pack a single bundle with
   200 outputs (the existing `kMaxOutputsPerBundle`) and force every
   verifier to do ~200·log(200) ≈ 1500 group ops just for the range
   proof. Acceptable per-bundle, but combined with `kMaxSpendsPerBundle`
   it doubles. **Recommend: aggregate up to 32 cvs per BP, and emit
   multiple BPs per bundle if needed.** This bounds verifier work to
   ~32·log(32)·(spends + outputs / 32) ≈ linear in N with a small
   constant. Pin the constant `kMaxBPAggregationDepth = 32` in
   `shielded_tx.h` next to the existing size limits.

6. **Activation rollout.** Phase 0-3 hardening is safe because the
   activation gate is UINT32_MAX. The Pedersen + BP changes ride the
   same gate. Sequence:
   - Cut a release with the Path C wire-format support compiled in
     and `kShieldedActivationHeight = UINT32_MAX` on mainnet/testnet.
     Operators upgrade.
   - Once the fleet is on the new binary, set testnet's
     `kShieldedActivationHeight` to a near-future block, soak ≥7 days
     with shielded txs flowing.
   - Then set mainnet's `kShieldedActivationHeight` ≥2 weeks future,
     cut another release, and let the fleet update before the
     activation block.

## 5. What this memo does NOT decide

- Whether to ship a new dinerod release with these Phase 3+ wire-format
  changes ahead of testnet activation. (Probably yes — operators need
  to upgrade before testnet activates.)
- Wallet UX details (which RPC takes a Pedersen blinding, how it's stored
  in the note store schema). Phase 5 work.

## 6. Implementation plan — Path C, three waves

Each wave produces a working tree, passing tests, and a clean daemon
build. No wave activates anything; the activation gate stays
UINT32_MAX on mainnet/testnet throughout. Mainnet activation comes
only after Wave 3 + ≥7-day testnet soak.

### Wave 1 — Pedersen + Bulletproofs + generators

- Add `V` and `R` constants per §4 #1; derivation test vectors.
- Wire format: extend ShieldedSpend / ShieldedOutput with `Hash cv`.
  Extend canonical serialization. Bump spec revision in
  `docs/specs/shielded_derivation.md` §11 to 0.3.0-draft and mark
  test vectors as needing regen.
- Witness: add `Hash rcv` to OutputWitness and SpendWitness. Wallet
  fills `rcv` from RNG at note construction; circuit takes it as
  private input.
- Bulletproofs: `aggregated_range_proof` field on ShieldedBundle
  (variable length, varint-prefixed). One BP per ≤32 cvs (§4 #5).
- libsecp256k1-zkp wiring in `src/consensus/shielded/` for native
  Pedersen commit + aggregated BP verify.
- Tests: Pedersen commit round-trip, BP range-proof round-trip with
  honest values, BP rejection on out-of-range value, aggregation cap
  enforcement.
- Activation gate UNCHANGED.

**Deliverable:** wire format and primitives in tree, all paths gated
by activation. Daemon and tests green.

### Wave 2 — Binding signature + sighash wiring + validation rules

- Replace the 32-byte SHA-256 binding tag with the 64-byte Schnorr
  signature per §4 #3. The `binding_sig` field grows; canonical
  serialization updates. Spec revision bump.
- Implement `binding_msg` construction exactly per §4 #3 — every
  component MUST be present and ordered as written. Single function
  with test coverage of every malleation vector (swap a cv,
  reorder cvs, mutate value_balance, mutate fee, mutate tx_sighash).
- Consensus validation rule: bvk reconstruction + Schnorr verify.
  ```
  bvk = sum(cv_spend) - sum(cv_output) - value_balance · V
  Verify Schnorr(R=binding_sig[:32], s=binding_sig[32:], pk=bvk, msg=binding_msg)
  ```
  Replace the existing SHA-256 binding-tag check in
  `ValidateShieldedBundle` with this. The `BindingSigInvalid` enum
  case stays; semantics tighten.
- Spend / output circuits: drop `range_check_limb` (now redundant —
  BP covers it). Circuit shrinks ~64 constraints per output, ~64 per
  spend.
- Tests: replace `BindingSigDetectsValueBalanceMutation` etc. with
  the Schnorr-shaped equivalents. Add cross-bundle balance test:
  honest 1-DIN shield, honest 1-DIN transfer, honest 1-DIN unshield,
  cross-validation of the bvk equation at every step.
- Activation gate UNCHANGED.

**Deliverable:** consensus actually verifies the supply equation.
Daemon and tests green. Wire format frozen at this point — any
further change is a re-bump.

### Wave 3 — End-to-end + reindex + testnet activation

- BlockReindexer parity: reindexed chains rebuild every cv, BP, and
  binding sig byte-identically to the live path. Existing reindex
  test extended.
- Wallet: shielded_wallet_runtime + shielded_wallet_ops construct
  bundles with cv / rcv / aggregated BP / binding sig. Note store
  schema gains `rcv` column (forward compat — existing notes have
  rcv = 0; new notes get fresh rcv).
- DineroDPI + dinero-qt RPC plumbing: `wallet.shield` / `unshield`
  / `transfer` round-trip end-to-end on regtest.
- Phase 0 test-vector regeneration runs against the v0.3.0 schema.
  The 2026-05-10 scheduled-routine fires (or runs early) and fills
  `docs/specs/shielded_derivation.md` §8 with real hex.
- Cut release with `kShieldedActivationHeight` set on **testnet**
  to a near-future block; mainnet stays UINT32_MAX. Soak ≥7 days
  with shielded txs flowing on testnet.
- Once testnet is stable, cut a second release with mainnet's
  `kShieldedActivationHeight` set ≥2 weeks future. Fleet updates
  before activation block.

**Deliverable:** shielded pool actually live on testnet first, then
mainnet, with full Path C protections in force.

## 7. Effort estimate

- Wave 1: ~5 days of focused work (libsecp256k1-zkp wiring + BP is
  the long pole).
- Wave 2: ~3-4 days (Schnorr + binding-sig sighash audit are the
  attention-heavy parts; the code is small).
- Wave 3: ~2-3 days code + calendar time for soak.
- **Total code: ~2 weeks. Total time to mainnet activation: ~3-4
  weeks** including soak. Memo-approval-to-mainnet-activation, with
  no surprises.
