/**
 * Shielded pool ZK circuits — Spartan proofs over Poseidon-2 R1CS.
 * See include/consensus/shielded/shielded_circuit.h.
 */

#include "consensus/shielded/shielded_circuit.h"
#include "consensus/shielded/pedersen_generators.h"  // PedersenGeneratorsReady

#include "crypto/evp_secp256k1.h"
#include "zk/zkvm/ec_gadget.h"             // cv-binding EC scalar-mults (Critical #1)
#include "zk/zkvm/gadgets.h"
#include "zk/zkvm/hyrax.h"
#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/r1cs_spartan.h"
#include "zk/zkvm/scalar.h"
#include "zk/zkvm/secp256k1_fe_gadget.h"  // range_check_limb (Phase 2 wave 4)
#include "zk/zkvm/transcript.h"
#include "zk/zkvm/poseidon_gadget.h"

#include <secp256k1.h>
#include <secp256k1_generator.h>

namespace dinero::consensus::shielded {

using zk::zkvm::R1CS;
using zk::zkvm::Variable;
using zk::zkvm::LinearCombination;
using zk::zkvm::Scalar;
using zk::zkvm::SpartanProof;
using zk::zkvm::Transcript;
using zk::zkvm::GeneratorSet;
using zk::zkvm::HyraxParams;
using zk::zkvm::poseidon2_gadget;
using zk::zkvm::poseidon2_native;
using zk::zkvm::ECPoint;
using zk::zkvm::Uint256;
namespace gadgets = zk::zkvm::gadgets;

// Implemented in pedersen_generators.cpp — the cached 64-byte libsecp
// generator V (the Pedersen *value* generator). data[0..32) = x (big-endian),
// data[32..64) = y (big-endian), per secp256k1_generator_save().
const secp256k1_generator* PedersenGeneratorVInternal();

namespace {

// Legacy (pre-cv-binding) proof version bytes. UNCHANGED so historical
// pre-activation proofs keep deserializing/verifying under the old circuit.
constexpr uint8_t kSpendProofVersion = 0x01;
constexpr uint8_t kOutputProofVersion = 0x02;

// Audit Critical #1: cv-bound proof version bytes. Distinct from the legacy
// bytes so a legacy proof can never be presented where a cv-bound proof is
// required (post-activation) and vice-versa — the version check rejects the
// mismatch before any circuit work.
constexpr uint8_t kSpendProofVersionCv = 0x03;
constexpr uint8_t kOutputProofVersionCv = 0x04;

// ── cv-binding helpers (Audit Critical #1) ──────────────────────────────
//
// cv = rcv·G + val·V, where (out of circuit, libsecp's secp256k1_pedersen_commit):
//   G = the STANDARD secp256k1 generator (via secp256k1_ecmult_gen) — exactly
//       the generator ec_scalar_mul_gen()/fixed_base_table use (secp256k1_Gx/Gy).
//   V = the Pedersen value generator (PedersenGeneratorVInternal()).
// See third_party/secp256k1-zkp/src/modules/generator/pedersen_impl.h
// (secp256k1_pedersen_ecmult: "sec*G + value*G2").

// Read the value generator V's affine (x, y) from the cached libsecp generator.
// Both coordinates are stored big-endian in the 64-byte struct.
bool ValueGenCoords(Uint256& vx, Uint256& vy) {
    const secp256k1_generator* g = PedersenGeneratorVInternal();
    if (!g) return false;
    vx = Uint256(reinterpret_cast<const uint8_t*>(g->data));        // [0..32)
    vy = Uint256(reinterpret_cast<const uint8_t*>(g->data) + 32);   // [32..64)
    return true;
}

// Reconstruct the affine (x, y) of a 33-byte Pedersen value commitment,
// replicating secp256k1_pedersen_commitment_load EXACTLY:
//   x   = cv[1..33)  (big-endian)
//   y   = sqrt(x^3 + 7)  via the fe_sqrt exponentiation root (set_xquad)
//   if (cv[0] & 1) y = p - y
// (set_xquad == secp256k1_fe_sqrt == raising to (p+1)/4, the same root the
// in-circuit bip340_lift_x computes; the parity bit then matches _load.)
bool LoadCvPoint(const ValueCommitment& cv, Uint256& out_x, Uint256& out_y) {
    // x must be a valid field element on the curve (parse contract). We don't
    // re-check on-curve here; a bogus cv yields a point the in-circuit
    // val·V+rcv·G can never equal, so the proof fails closed.
    Uint256 x(cv.data() + 1);  // big-endian 32 bytes
    using zk::zkvm::uint256_mul_mod_p;
    using zk::zkvm::uint256_add_mod_p;
    using zk::zkvm::uint256_sub_mod_p;
    using zk::zkvm::uint256_p;
    Uint256 x2 = uint256_mul_mod_p(x, x);
    Uint256 x3 = uint256_mul_mod_p(x2, x);
    Uint256 rhs = uint256_add_mod_p(x3, Uint256(uint64_t(7)));
    // y = rhs^((p+1)/4) mod p (square-and-multiply, exp big-endian bytes).
    // (p+1)/4 = 3FFFFFFF...BFFFFF0C — identical constant to bip340_lift_x.
    static const uint8_t exp[32] = {
        0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xBF, 0xFF, 0xFF, 0x0C};
    Uint256 y(uint64_t(1));
    Uint256 base = rhs;
    for (int i = 0; i < 256; ++i) {
        int byte_idx = 31 - (i / 8);
        int bit_idx = i % 8;
        if ((exp[byte_idx] >> bit_idx) & 1) {
            y = uint256_mul_mod_p(y, base);
        }
        base = uint256_mul_mod_p(base, base);
    }
    if (cv[0] & 1) {
        y = uint256_sub_mod_p(uint256_p(), y);
    }
    out_x = x;
    out_y = y;
    return true;
}

// Allocate the bundle's cv as a PUBLIC-INPUT EC point. MUST be called during
// the public-input allocation phase (before any cs.alloc()), because
// R1CS::alloc_input inserts and shifts all later variables.
ECPoint AllocCvInputPoint(R1CS& cs, const Uint256& x, const Uint256& y) {
    ECPoint pt;
    for (int i = 0; i < 4; ++i) pt.x.limbs[i] = cs.alloc_input(Scalar(uint64_t(x.limbs[i])));
    for (int i = 0; i < 4; ++i) pt.y.limbs[i] = cs.alloc_input(Scalar(uint64_t(y.limbs[i])));
    pt.is_identity = cs.alloc_input(Scalar::zero());  // commitments are never identity
    return pt;
}

// Enforce cv == val·V + rcv·G in-circuit. Call AFTER all public inputs are
// allocated (only alloc()/constraints here). `val` is the SAME note-value
// Variable that feeds the range check and note commitment — the soundness
// spine: the cv is bound to the exact value the note encodes.
void EnforceCvBinding(R1CS& cs, Variable val, const Hash& rcv_blind,
                      const ECPoint& cv_pub, const Uint256& vx, const Uint256& vy,
                      const std::string& prefix) {
    // val (64-bit, already range-checked) decomposed and re-constrained to the
    // SAME val Variable, then padded to 256 bits with the const-zero Variable
    // so the high 24 windows are skipped structurally.
    std::vector<zk::zkvm::Variable> val_bits = gadgets::to_bits(cs, val, 64, prefix + "_cvvalbits");
    std::vector<zk::zkvm::Variable> val_bits256(256, cs.const_zero());
    for (int i = 0; i < 64; ++i) val_bits256[i] = val_bits[i];

    // rcv blinding, range-decomposed to 256 bits (== the in-circuit rcv scalar,
    // which is HashToScalar(rcv) reduced mod the scalar field n — matching the
    // out-of-circuit secp256k1_scalar_set_b32 reduction of the same 32 bytes).
    Variable rcv_var = cs.alloc(Scalar(rcv_blind.data()));  // == HashToScalar, defined below
    std::vector<zk::zkvm::Variable> rcv_bits = gadgets::to_bits(cs, rcv_var, 256, prefix + "_cvrcvbits");

    ECPoint val_V = zk::zkvm::ec_scalar_mul_fixed(cs, val_bits256, vx, vy, prefix + "_valV");
    ECPoint rcv_G = zk::zkvm::ec_scalar_mul_gen(cs, rcv_bits, prefix + "_rcvG");
    ECPoint cv_circ = zk::zkvm::ec_add_complete(cs, val_V, rcv_G, prefix + "_cvadd");

    Variable eq = zk::zkvm::ec_equal(cs, cv_circ, cv_pub, prefix + "_cveq");
    cs.enforce_equal(LinearCombination(eq),
                     LinearCombination(Scalar::one(), zk::zkvm::VAR_ONE),
                     prefix + "_cvbind");
}

Scalar HashToScalar(const Hash& h) {
    return Scalar(h.data());
}

Scalar U64ToScalar(uint64_t v) {
    return Scalar(v);
}

secp256k1_context* ResolveCtx(secp256k1_context_struct* ctx) {
    return ctx ? reinterpret_cast<secp256k1_context*>(ctx)
               : dinero::crypto::GetSecp256k1ContextSignVerify();
}

const GeneratorSet& ShieldedGenerators(const R1CS& cs, secp256k1_context* ctx) {
    const size_t gens_need = std::max<size_t>(
        4, std::max(HyraxParams::from_n(cs.num_variables()).n_cols,
                    HyraxParams::from_n(cs.num_constraints()).n_cols));
    return GeneratorSet::cached(gens_need, ctx);
}

std::vector<Scalar> ZeroErrorVector(const R1CS& cs) {
    return std::vector<Scalar>(cs.num_constraints(), Scalar::zero());
}

void BindSpendTranscript(Transcript& transcript, const SpendPublicInputs& pub,
                         bool cv_bound) {
    transcript.append_scalar("nf", HashToScalar(pub.nullifier));
    transcript.append_scalar("an", HashToScalar(pub.anchor));
    if (cv_bound) {
        // Bind the Pedersen value commitment (prefix + x) into Fiat-Shamir
        // (Critical #1). cv is also a bound public input; this is supplementary.
        transcript.append_u64("cv0", static_cast<uint64_t>(pub.cv[0]));
        transcript.append_scalar("cvx", Scalar(pub.cv.data() + 1));
    }
}

void BindOutputTranscript(Transcript& transcript, const OutputPublicInputs& pub,
                          bool cv_bound) {
    transcript.append_scalar("cm", HashToScalar(pub.commitment));
    if (cv_bound) {
        transcript.append_u64("cv0", static_cast<uint64_t>(pub.cv[0]));
        transcript.append_scalar("cvx", Scalar(pub.cv.data() + 1));
    }
}

bool DeserializeShieldedProof(const std::vector<uint8_t>& proof_bytes,
                              uint8_t expected_version,
                              SpartanProof& out,
                              secp256k1_context* ctx) {
    if (proof_bytes.empty() || proof_bytes[0] != expected_version) {
        return false;
    }
    if (proof_bytes.size() == 1) {
        return false;
    }
    std::vector<uint8_t> payload(proof_bytes.begin() + 1, proof_bytes.end());
    return SpartanProof::deserialize(payload, out, ctx);
}

SpendWitness DummySpendWitness() {
    SpendWitness witness{};
    witness.secret_key.fill(0);
    witness.value.fill(0);
    witness.randomness.fill(0);
    for (auto& sibling : witness.merkle_path) {
        sibling.fill(0);
    }
    return witness;
}

OutputWitness DummyOutputWitness() {
    OutputWitness witness{};
    witness.value.fill(0);
    witness.public_key.fill(0);
    witness.randomness.fill(0);
    return witness;
}

// Merkle path verification gadget: starting from leaf, hash up with
// siblings using Poseidon, checking direction bits from leaf_index.
// Returns the computed root variable.
Variable merkle_path_gadget(R1CS& cs,
                            Variable leaf,
                            Variable leaf_index_var,
                            const std::array<Hash, TREE_DEPTH>& siblings,
                            const std::string& prefix) {
    Variable current = leaf;
    const auto leaf_index_bits = gadgets::to_bits(
        cs, leaf_index_var, TREE_DEPTH, prefix + "_leaf_index_bits");

    for (size_t depth = 0; depth < TREE_DEPTH; ++depth) {
        Variable sibling = cs.alloc(HashToScalar(siblings[depth]));
        Variable is_right = leaf_index_bits[depth];

        // Poseidon(left, right) where left/right depend on the leaf-index bit.
        Variable left = gadgets::select(
            cs, is_right, sibling, current,
            prefix + "_select_left_" + std::to_string(depth));
        Variable right = gadgets::select(
            cs, is_right, current, sibling,
            prefix + "_select_right_" + std::to_string(depth));

        current = poseidon2_gadget(cs, left, right,
            prefix + "_merkle_" + std::to_string(depth));
    }

    return current;
}

} // namespace

// ── Spend circuit ────────────────────────────────────────────────────

R1CS BuildSpendCircuit(const SpendWitness& witness,
                       const SpendPublicInputs& pub,
                       bool cv_bound) {
    R1CS cs;

    // Public inputs
    Variable nullifier_pub = cs.alloc_input(HashToScalar(pub.nullifier));
    Variable anchor_pub    = cs.alloc_input(HashToScalar(pub.anchor));

    // Audit Critical #1: cv public-input EC point. MUST be allocated here,
    // during the input phase (alloc_input shifts all later variables), before
    // any cs.alloc() below. The legacy circuit (cv_bound=false) skips this
    // entirely and is byte-for-byte identical to the pre-fix circuit.
    ECPoint cv_pub{};
    Uint256 v_x, v_y;  // value generator V coordinates (filled when cv_bound)
    if (cv_bound) {
        Uint256 cv_x, cv_y;
        LoadCvPoint(pub.cv, cv_x, cv_y);
        cv_pub = AllocCvInputPoint(cs, cv_x, cv_y);
        ValueGenCoords(v_x, v_y);
    }

    // Private witness
    Variable sk   = cs.alloc(HashToScalar(witness.secret_key));
    Variable val  = cs.alloc(HashToScalar(witness.value));
    Variable rand = cs.alloc(HashToScalar(witness.randomness));
    Variable d    = cs.alloc(HashToScalar(witness.d));
    Variable idx  = cs.alloc(U64ToScalar(witness.leaf_index));

    // Phase 2 wave 4: range-prove `val` to 64 bits. Without this, the
    // value scalar can be ~2^254 (the field order), enabling per-output
    // values far beyond the 21M DIN supply assumption. Cross-bundle
    // supply integrity (sum_in == sum_out + value_balance enforced
    // inside the proof) requires Pedersen value commitments, which is
    // a future phase — see shielded_validation.h header note.
    zk::zkvm::range_check_limb(cs, val, 64, "spend_value_range");

    // 1. Derive public key: pk = Poseidon(sk, 0)
    Variable zero_var = cs.alloc(Scalar::zero());
    Variable pk = poseidon2_gadget(cs, sk, zero_var, "derive_pk");

    // 2. Phase 2 wave 5: address-binding tag.
    //    addr_bind = Poseidon(ADDR_TAG, Poseidon(d, pk))
    Variable tag = cs.alloc(HashToScalar(AddrBindTag()));
    Variable d_pk = poseidon2_gadget(cs, d, pk, "addr_d_pk");
    Variable addr_bind = poseidon2_gadget(cs, tag, d_pk, "addr_bind");

    // 3. Compute note commitment:
    //    cm = Poseidon(Poseidon(addr_bind, val), rand)
    Variable inner = poseidon2_gadget(cs, addr_bind, val, "note_inner");
    Variable cm    = poseidon2_gadget(cs, inner, rand, "note_cm");

    // 3. Merkle path: verify cm is in the tree with root == anchor
    Variable computed_root = merkle_path_gadget(
        cs, cm, idx, witness.merkle_path, "spend");

    // Assert computed_root == anchor (public input)
    cs.enforce_equal(LinearCombination(computed_root),
                     LinearCombination(anchor_pub));

    // 4. Compute nullifier: nf = Poseidon(sk, leaf_index)
    Variable nf = poseidon2_gadget(cs, sk, idx, "nullifier");

    // Assert nf == nullifier (public input)
    cs.enforce_equal(LinearCombination(nf),
                     LinearCombination(nullifier_pub));

    // Audit Critical #1: bind cv to the SAME `val` (line 150 region) that
    // already feeds the 64-bit range check and the note commitment `cm`.
    // Enforces cv == val·V + rcv·G. Closes the mint-from-nothing hole.
    if (cv_bound) {
        EnforceCvBinding(cs, val, witness.rcv, cv_pub, v_x, v_y, "spend");
    }

    return cs;
}

std::vector<uint8_t> ProveSpend(const SpendWitness& witness,
                                 const SpendPublicInputs& pub,
                                 secp256k1_context_struct* ctx,
                                 bool bind_public_inputs,
                                 bool cv_bound) {
    auto* sctx = ResolveCtx(ctx);
    // cv-bound proving needs the Pedersen value generator V; fail closed if
    // the generator derivation hasn't succeeded.
    if (cv_bound && !PedersenGeneratorsReady()) {
        return {};
    }
    auto cs = BuildSpendCircuit(witness, pub, cv_bound);
    if (!cs.is_satisfied()) {
        return {};
    }

    Transcript transcript("dinero.shielded.spend.v1");
    BindSpendTranscript(transcript, pub, cv_bound);

    const auto& gens = ShieldedGenerators(cs, sctx);
    SpartanProof proof = zk::zkvm::r1cs_spartan_prove(
        cs, ZeroErrorVector(cs), Scalar::one(), gens, transcript, sctx, bind_public_inputs);

    std::vector<uint8_t> proof_bytes;
    proof_bytes.push_back(cv_bound ? kSpendProofVersionCv : kSpendProofVersion);
    auto serialized = proof.serialize(sctx);
    proof_bytes.insert(proof_bytes.end(), serialized.begin(), serialized.end());
    return proof_bytes;
}

// AUDIT-ONLY (SUSPECTED-01 PoC). Identical to ProveSpend EXCEPT the circuit is built
// from `pub_committed` (the real note → satisfiable) while the Fiat-Shamir transcript
// is bound to `pub_present`. This isolates the public-input-binding property: if the
// verifier truly binds the on-chain inputs to the committed witness, a proof produced
// here must be rejected when verified/presented with `pub_present`. NOT a production path.
std::vector<uint8_t> ProveSpend_AuditDesync(const SpendWitness& witness,
                                            const SpendPublicInputs& pub_committed,
                                            const SpendPublicInputs& pub_present,
                                            secp256k1_context_struct* ctx,
                                            bool bind_public_inputs) {
    auto* sctx = ResolveCtx(ctx);
    auto cs = BuildSpendCircuit(witness, pub_committed);
    if (!cs.is_satisfied()) {
        return {};
    }

    Transcript transcript("dinero.shielded.spend.v1");
    BindSpendTranscript(transcript, pub_present, /*cv_bound=*/false);  // <-- desync: bind PRESENTED, not committed

    const auto& gens = ShieldedGenerators(cs, sctx);
    SpartanProof proof = zk::zkvm::r1cs_spartan_prove(
        cs, ZeroErrorVector(cs), Scalar::one(), gens, transcript, sctx, bind_public_inputs);

    std::vector<uint8_t> proof_bytes;
    proof_bytes.push_back(kSpendProofVersion);
    auto serialized = proof.serialize(sctx);
    proof_bytes.insert(proof_bytes.end(), serialized.begin(), serialized.end());
    return proof_bytes;
}

bool VerifySpend(const std::vector<uint8_t>& proof_bytes,
                 const SpendPublicInputs& pub,
                 secp256k1_context_struct* ctx,
                 bool bind_public_inputs,
                 bool cv_bound) {
    auto* sctx = ResolveCtx(ctx);
    // cv-bound verification needs V; without it the verifier circuit can't be
    // reconstructed, so reject (fail closed) — never silently fall back.
    if (cv_bound && !PedersenGeneratorsReady()) {
        return false;
    }
    SpartanProof proof;
    const uint8_t expected_version = cv_bound ? kSpendProofVersionCv : kSpendProofVersion;
    if (!DeserializeShieldedProof(proof_bytes, expected_version, proof, sctx)) {
        return false;
    }

    const SpendWitness dummy = DummySpendWitness();
    const auto verifier_cs = BuildSpendCircuit(dummy, pub, cv_bound);
    Transcript transcript("dinero.shielded.spend.v1");
    BindSpendTranscript(transcript, pub, cv_bound);

    const auto& gens = ShieldedGenerators(verifier_cs, sctx);
    const auto circuit_hash = zk::zkvm::spartan_hash_r1cs_structure(verifier_cs);
    return zk::zkvm::r1cs_spartan_verify(
        proof, verifier_cs, verifier_cs.num_constraints(),
        verifier_cs.num_variables(), circuit_hash, Scalar::one(),
        gens, transcript, sctx, bind_public_inputs);
}

// ── Output circuit ───────────────────────────────────────────────────

R1CS BuildOutputCircuit(const OutputWitness& witness,
                        const OutputPublicInputs& pub,
                        bool cv_bound) {
    R1CS cs;

    // Public input
    Variable cm_pub = cs.alloc_input(HashToScalar(pub.commitment));

    // Audit Critical #1: cv public-input EC point (input phase — see spend).
    ECPoint cv_pub{};
    Uint256 v_x, v_y;
    if (cv_bound) {
        Uint256 cv_x, cv_y;
        LoadCvPoint(pub.cv, cv_x, cv_y);
        cv_pub = AllocCvInputPoint(cs, cv_x, cv_y);
        ValueGenCoords(v_x, v_y);
    }

    // Private witness
    Variable val  = cs.alloc(HashToScalar(witness.value));
    Variable pk   = cs.alloc(HashToScalar(witness.public_key));
    Variable rand = cs.alloc(HashToScalar(witness.randomness));
    Variable d    = cs.alloc(HashToScalar(witness.d));

    // Phase 2 wave 4: range-prove `val` to 64 bits (mirrors spend
    // circuit). See spend-circuit comment for the cross-bundle gap.
    zk::zkvm::range_check_limb(cs, val, 64, "out_value_range");

    // Phase 2 wave 5: address-binding tag — must match spend circuit.
    Variable tag = cs.alloc(HashToScalar(AddrBindTag()));
    Variable d_pk = poseidon2_gadget(cs, d, pk, "out_addr_d_pk");
    Variable addr_bind = poseidon2_gadget(cs, tag, d_pk, "out_addr_bind");

    // commitment = Poseidon(Poseidon(addr_bind, val), rand)
    Variable inner = poseidon2_gadget(cs, addr_bind, val, "out_inner");
    Variable cm    = poseidon2_gadget(cs, inner, rand, "out_cm");

    // Assert cm == commitment (public input)
    cs.enforce_equal(LinearCombination(cm),
                     LinearCombination(cm_pub));

    // Audit Critical #1: bind cv to the SAME `val` that feeds the range check
    // and the note commitment. Enforces cv == val·V + rcv·G.
    if (cv_bound) {
        EnforceCvBinding(cs, val, witness.rcv, cv_pub, v_x, v_y, "out");
    }

    return cs;
}

std::vector<uint8_t> ProveOutput(const OutputWitness& witness,
                                  const OutputPublicInputs& pub,
                                  secp256k1_context_struct* ctx,
                                  bool bind_public_inputs,
                                  bool cv_bound) {
    auto* sctx = ResolveCtx(ctx);
    if (cv_bound && !PedersenGeneratorsReady()) {
        return {};
    }
    auto cs = BuildOutputCircuit(witness, pub, cv_bound);
    if (!cs.is_satisfied()) {
        return {};
    }

    Transcript transcript("dinero.shielded.output.v1");
    BindOutputTranscript(transcript, pub, cv_bound);

    const auto& gens = ShieldedGenerators(cs, sctx);
    SpartanProof proof = zk::zkvm::r1cs_spartan_prove(
        cs, ZeroErrorVector(cs), Scalar::one(), gens, transcript, sctx, bind_public_inputs);

    std::vector<uint8_t> proof_bytes;
    proof_bytes.push_back(cv_bound ? kOutputProofVersionCv : kOutputProofVersion);
    auto serialized = proof.serialize(sctx);
    proof_bytes.insert(proof_bytes.end(), serialized.begin(), serialized.end());
    return proof_bytes;
}

// AUDIT-ONLY (CONFIRMED-CRIT-05 regression, declared in src/test/shielded_audit_desync.h).
// Output-circuit analogue of ProveSpend_AuditDesync: build the circuit from
// `pub_committed` (real, satisfiable) but bind the transcript to `pub_present`, so the
// commitment-binding property can be tested the same way as anchor/nullifier binding.
std::vector<uint8_t> ProveOutput_AuditDesync(const OutputWitness& witness,
                                             const OutputPublicInputs& pub_committed,
                                             const OutputPublicInputs& pub_present,
                                             secp256k1_context_struct* ctx,
                                             bool bind_public_inputs) {
    auto* sctx = ResolveCtx(ctx);
    auto cs = BuildOutputCircuit(witness, pub_committed);
    if (!cs.is_satisfied()) {
        return {};
    }

    Transcript transcript("dinero.shielded.output.v1");
    BindOutputTranscript(transcript, pub_present, /*cv_bound=*/false);  // <-- desync: bind PRESENTED, not committed

    const auto& gens = ShieldedGenerators(cs, sctx);
    SpartanProof proof = zk::zkvm::r1cs_spartan_prove(
        cs, ZeroErrorVector(cs), Scalar::one(), gens, transcript, sctx, bind_public_inputs);

    std::vector<uint8_t> proof_bytes;
    proof_bytes.push_back(kOutputProofVersion);
    auto serialized = proof.serialize(sctx);
    proof_bytes.insert(proof_bytes.end(), serialized.begin(), serialized.end());
    return proof_bytes;
}

bool VerifyOutput(const std::vector<uint8_t>& proof_bytes,
                  const OutputPublicInputs& pub,
                  secp256k1_context_struct* ctx,
                  bool bind_public_inputs,
                  bool cv_bound) {
    auto* sctx = ResolveCtx(ctx);
    if (cv_bound && !PedersenGeneratorsReady()) {
        return false;
    }
    SpartanProof proof;
    const uint8_t expected_version = cv_bound ? kOutputProofVersionCv : kOutputProofVersion;
    if (!DeserializeShieldedProof(proof_bytes, expected_version, proof, sctx)) {
        return false;
    }

    const OutputWitness dummy = DummyOutputWitness();
    const auto verifier_cs = BuildOutputCircuit(dummy, pub, cv_bound);
    Transcript transcript("dinero.shielded.output.v1");
    BindOutputTranscript(transcript, pub, cv_bound);

    const auto& gens = ShieldedGenerators(verifier_cs, sctx);
    const auto circuit_hash = zk::zkvm::spartan_hash_r1cs_structure(verifier_cs);
    return zk::zkvm::r1cs_spartan_verify(
        proof, verifier_cs, verifier_cs.num_constraints(),
        verifier_cs.num_variables(), circuit_hash, Scalar::one(),
        gens, transcript, sctx, bind_public_inputs);
}

} // namespace dinero::consensus::shielded
