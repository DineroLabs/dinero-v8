/**
 * Shielded pool ZK circuits — Spartan proofs over Poseidon-2 R1CS.
 * See include/consensus/shielded/shielded_circuit.h.
 */

#include "consensus/shielded/shielded_circuit.h"

#include "crypto/evp_secp256k1.h"
#include "zk/zkvm/gadgets.h"
#include "zk/zkvm/hyrax.h"
#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/r1cs_spartan.h"
#include "zk/zkvm/scalar.h"
#include "zk/zkvm/secp256k1_fe_gadget.h"  // range_check_limb (Phase 2 wave 4)
#include "zk/zkvm/transcript.h"
#include "zk/zkvm/poseidon_gadget.h"

#include <secp256k1.h>

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
namespace gadgets = zk::zkvm::gadgets;

namespace {

constexpr uint8_t kSpendProofVersion = 0x01;
constexpr uint8_t kOutputProofVersion = 0x02;

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

void BindSpendTranscript(Transcript& transcript, const SpendPublicInputs& pub) {
    transcript.append_scalar("nf", HashToScalar(pub.nullifier));
    transcript.append_scalar("an", HashToScalar(pub.anchor));
}

void BindOutputTranscript(Transcript& transcript, const OutputPublicInputs& pub) {
    transcript.append_scalar("cm", HashToScalar(pub.commitment));
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
                       const SpendPublicInputs& pub) {
    R1CS cs;

    // Public inputs
    Variable nullifier_pub = cs.alloc_input(HashToScalar(pub.nullifier));
    Variable anchor_pub    = cs.alloc_input(HashToScalar(pub.anchor));

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

    return cs;
}

std::vector<uint8_t> ProveSpend(const SpendWitness& witness,
                                 const SpendPublicInputs& pub,
                                 secp256k1_context_struct* ctx) {
    auto* sctx = ResolveCtx(ctx);
    auto cs = BuildSpendCircuit(witness, pub);
    if (!cs.is_satisfied()) {
        return {};
    }

    Transcript transcript("dinero.shielded.spend.v1");
    BindSpendTranscript(transcript, pub);

    const auto& gens = ShieldedGenerators(cs, sctx);
    SpartanProof proof = zk::zkvm::r1cs_spartan_prove(
        cs, ZeroErrorVector(cs), Scalar::one(), gens, transcript, sctx);

    std::vector<uint8_t> proof_bytes;
    proof_bytes.push_back(kSpendProofVersion);
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
                                            secp256k1_context_struct* ctx) {
    auto* sctx = ResolveCtx(ctx);
    auto cs = BuildSpendCircuit(witness, pub_committed);
    if (!cs.is_satisfied()) {
        return {};
    }

    Transcript transcript("dinero.shielded.spend.v1");
    BindSpendTranscript(transcript, pub_present);  // <-- desync: bind PRESENTED, not committed

    const auto& gens = ShieldedGenerators(cs, sctx);
    SpartanProof proof = zk::zkvm::r1cs_spartan_prove(
        cs, ZeroErrorVector(cs), Scalar::one(), gens, transcript, sctx);

    std::vector<uint8_t> proof_bytes;
    proof_bytes.push_back(kSpendProofVersion);
    auto serialized = proof.serialize(sctx);
    proof_bytes.insert(proof_bytes.end(), serialized.begin(), serialized.end());
    return proof_bytes;
}

bool VerifySpend(const std::vector<uint8_t>& proof_bytes,
                 const SpendPublicInputs& pub,
                 secp256k1_context_struct* ctx) {
    auto* sctx = ResolveCtx(ctx);
    SpartanProof proof;
    if (!DeserializeShieldedProof(proof_bytes, kSpendProofVersion, proof, sctx)) {
        return false;
    }

    const SpendWitness dummy = DummySpendWitness();
    const auto verifier_cs = BuildSpendCircuit(dummy, pub);
    Transcript transcript("dinero.shielded.spend.v1");
    BindSpendTranscript(transcript, pub);

    const auto& gens = ShieldedGenerators(verifier_cs, sctx);
    const auto circuit_hash = zk::zkvm::spartan_hash_r1cs_structure(verifier_cs);
    return zk::zkvm::r1cs_spartan_verify(
        proof, verifier_cs, verifier_cs.num_constraints(),
        verifier_cs.num_variables(), circuit_hash, Scalar::one(),
        gens, transcript, sctx);
}

// ── Output circuit ───────────────────────────────────────────────────

R1CS BuildOutputCircuit(const OutputWitness& witness,
                        const OutputPublicInputs& pub) {
    R1CS cs;

    // Public input
    Variable cm_pub = cs.alloc_input(HashToScalar(pub.commitment));

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

    return cs;
}

std::vector<uint8_t> ProveOutput(const OutputWitness& witness,
                                  const OutputPublicInputs& pub,
                                  secp256k1_context_struct* ctx) {
    auto* sctx = ResolveCtx(ctx);
    auto cs = BuildOutputCircuit(witness, pub);
    if (!cs.is_satisfied()) {
        return {};
    }

    Transcript transcript("dinero.shielded.output.v1");
    BindOutputTranscript(transcript, pub);

    const auto& gens = ShieldedGenerators(cs, sctx);
    SpartanProof proof = zk::zkvm::r1cs_spartan_prove(
        cs, ZeroErrorVector(cs), Scalar::one(), gens, transcript, sctx);

    std::vector<uint8_t> proof_bytes;
    proof_bytes.push_back(kOutputProofVersion);
    auto serialized = proof.serialize(sctx);
    proof_bytes.insert(proof_bytes.end(), serialized.begin(), serialized.end());
    return proof_bytes;
}

bool VerifyOutput(const std::vector<uint8_t>& proof_bytes,
                  const OutputPublicInputs& pub,
                  secp256k1_context_struct* ctx) {
    auto* sctx = ResolveCtx(ctx);
    SpartanProof proof;
    if (!DeserializeShieldedProof(proof_bytes, kOutputProofVersion, proof, sctx)) {
        return false;
    }

    const OutputWitness dummy = DummyOutputWitness();
    const auto verifier_cs = BuildOutputCircuit(dummy, pub);
    Transcript transcript("dinero.shielded.output.v1");
    BindOutputTranscript(transcript, pub);

    const auto& gens = ShieldedGenerators(verifier_cs, sctx);
    const auto circuit_hash = zk::zkvm::spartan_hash_r1cs_structure(verifier_cs);
    return zk::zkvm::r1cs_spartan_verify(
        proof, verifier_cs, verifier_cs.num_constraints(),
        verifier_cs.num_variables(), circuit_hash, Scalar::one(),
        gens, transcript, sctx);
}

} // namespace dinero::consensus::shielded
