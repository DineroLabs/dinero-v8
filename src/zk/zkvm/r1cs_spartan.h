// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Spartan-lite Decider for Relaxed R1CS
 *
 * Replaces the O(N) IPA-based decider (r1cs_ipa) with an O(√n) verifier
 * using the Hyrax matrix commitment scheme + multilinear sum-check reduction.
 *
 * The current IPA decider (r1cs_ipa.cpp) verifies a 2N+1 point MSM where
 * N = next_pow2(n_vars + n_constraints) ≈ 524,288, taking ~900ms cold.
 *
 * This Spartan-lite decider replaces that with:
 *   • Two bilinear sum-check protocols (outer log_m rounds + inner log_n rounds)
 *   • One Hyrax eval proof for the witness W̃(ry)   [O(√n_vars) EC ops]
 *   • One Hyrax eval proof for the error vector Ẽ(rx_m) [O(√m) EC ops]
 *
 * Verifier cost: O(m * sparsity) field ops  +  O(√n + √m) EC ops
 * For the hybrid circuit: ~4M field ops + ~1428 EC ops ≈ 5–15 ms cold.
 *
 * Protocol (Wahby et al., "Spartan: Efficient and general-purpose zkSNARKs
 * without trusted setup", CRYPTO 2020, adapted for relaxed R1CS):
 *
 *  1. Commit:   comm_W = hyrax_commit(z, H_z)
 *               comm_E = hyrax_commit(E, H_E)
 *  2. Outer SC: Prove ∑_i eq(τ,i)·[(A_i·z)(B_i·z) − u·(C_i·z) − E_i] = 0
 *               produces challenge rx_m ∈ F^{log m}.
 *  3. Claims:   Prover sends Az, Bz, Cz, Ez at rx_m.
 *  4. Batch:    ρ ← transcript.  vz = Az + ρ·Bz + ρ²·Cz.
 *  5. Inner SC: Prove ∑_j M̃(rx_m,j)·z(j) = vz   (M̃ = Ã+ρB̃+ρ²C̃)
 *               produces challenge ry ∈ F^{log n}.
 *  6. Eval:     hyrax_eval_prove(z, ry)  →  proof.eval_W
 *               hyrax_eval_prove(E, rx_m) →  proof.eval_E
 *
 *  Verifier checks:
 *   - Both sum-checks are valid.
 *   - eq(τ,rx_m)·(Az·Bz − u·Cz − Ez) = final_outer_claim.
 *   - M̃(rx_m, ry)·eval_W.claimed = final_inner_claim.
 *   - hyrax_eval_verify(comm_W, ry, proof.eval_W, ...) passes.
 *   - hyrax_eval_verify(comm_E, rx_m, proof.eval_E, ...) passes.
 *   - proof.eval_E.claimed == Ez.
 */

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/hyrax.h"
#include <array>
#include <vector>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// Spartan proof structure
// ---------------------------------------------------------------------------

struct SpartanProof {
    HyraxCommitment comm_W;     // Hyrax commitment to witness z (n_vars entries)
    HyraxCommitment comm_E;     // Hyrax commitment to error E (m entries)

    std::vector<uint8_t> circuit_hash;  // SHA256 of R1CS structure

    // Outer sum-check: log_m rounds × 4 evaluations each (at X=0,1,2,3, degree-3)
    std::vector<std::array<Scalar, 4>> outer_sc;

    // Claims at end of outer sum-check (oracle queries at rx_m)
    Scalar Az_claim;   // Ã(rx_m, ·) · z
    Scalar Bz_claim;   // B̃(rx_m, ·) · z
    Scalar Cz_claim;   // C̃(rx_m, ·) · z
    Scalar Ez_claim;   // Ẽ(rx_m)

    // Inner sum-check: log_n rounds × 3 evaluations each
    std::vector<std::array<Scalar, 3>> inner_sc;

    // Hyrax evaluation proofs
    HyraxEvalProof eval_W;   // z̃(ry) at inner sum-check challenge
    HyraxEvalProof eval_E;   // Ẽ(rx_m) at outer sum-check challenge

    std::vector<uint8_t> serialize(secp256k1_context* ctx) const;
    static bool deserialize(const std::vector<uint8_t>& data, SpartanProof& out,
                            secp256k1_context* ctx);
};

// ---------------------------------------------------------------------------
// Spartan prover — replaces r1cs_ipa_prove
// ---------------------------------------------------------------------------

/**
 * Prove relaxed R1CS satisfaction using Spartan + Hyrax.
 *
 * Witness is taken from cs.witness(). Error vector E must have
 * cs.num_constraints() entries. u is the relaxation scalar (1 for standard).
 *
 * The transcript must already contain context from the caller (e.g., Nova
 * committed instance, public statement). The function appends the Hyrax
 * commitments and all sum-check messages to the transcript.
 */
SpartanProof r1cs_spartan_prove(
    const R1CS& cs,
    const std::vector<Scalar>& E,  // error vector, length == cs.num_constraints()
    const Scalar& u,               // relaxation scalar
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx,
    // CONFIRMED-CRIT-05: when true (default), commit/open only the private witness and
    // have the verifier bind the public inputs (z=(1,io,W) split). false reproduces the
    // pre-fix behavior (full-z commit, public inputs UNBOUND) — retained ONLY to validate
    // pre-activation-height history under the old consensus rule. Never use false for new proofs.
    bool bind_public_inputs = true
);

// ---------------------------------------------------------------------------
// Spartan verifier — replaces r1cs_ipa_verify
// ---------------------------------------------------------------------------

/**
 * Verify a Spartan proof for relaxed R1CS.
 *
 * @param proof         Proof from r1cs_spartan_prove
 * @param verifier_cs   R1CS with the constraint structure (witness not used)
 * @param num_constraints Expected number of constraints
 * @param num_variables   Expected number of variables (witness length)
 * @param expected_circuit_hash  SHA256 from hash_r1cs_structure(cs)
 * @param u             Relaxation scalar (must match prover's u)
 * @param gens          Generator set (same as prover's)
 * @param transcript    Must match prover's transcript (same public prefix)
 * @param ctx           secp256k1 context
 * @return              true if proof is valid
 */
bool r1cs_spartan_verify(
    const SpartanProof& proof,
    const R1CS& verifier_cs,
    size_t num_constraints,
    size_t num_variables,
    const std::vector<uint8_t>& expected_circuit_hash,
    const Scalar& u,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx,
    // CONFIRMED-CRIT-05: must match the prover. true (default) reconstructs the public-input
    // contribution to z̃(ry) and binds it; false is the pre-fix unbound rule, retained only
    // for validating pre-activation-height history.
    bool bind_public_inputs = true
);

/**
 * Hash the R1CS structure for circuit identity binding.
 * Same function as in r1cs_ipa.h (shared implementation).
 */
std::vector<uint8_t> spartan_hash_r1cs_structure(const R1CS& cs);

} // namespace zkvm
} // namespace zk
} // namespace dinero
