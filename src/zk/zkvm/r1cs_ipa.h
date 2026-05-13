// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * R1CS → IPA Bridge
 *
 * Converts R1CS satisfaction into an Inner Product Argument.
 *
 * Given an R1CS with m constraints and witness z:
 *   For each constraint i: (A_i · z) * (B_i · z) = C_i · z
 *
 * The bridge:
 *   1. Evaluates wire values: w_L[i] = A_i · z, w_R[i] = B_i · z, w_O[i] = C_i · z
 *   2. Uses a random linear combination challenge α to reduce Hadamard check
 *      to a single inner product: Σ α^i * (w_L[i] * w_R[i] - w_O[i]) = 0
 *   3. Encodes as IPA vectors of length 2m (padded to power of 2):
 *      a = (α^0·w_L[0], ..., α^{m-1}·w_L[m-1], α^0, ..., α^{m-1}, 0...)
 *      b = (w_R[0], ..., w_R[m-1], -w_O[0], ..., -w_O[m-1], 0...)
 *   4. Proves <a, b> = 0 via IPA
 *
 * Soundness: By Schwartz-Zippel, if any constraint is violated,
 * the random linear combination is non-zero with probability 1 - m/|F|.
 * The IPA binding prevents the prover from using different vectors.
 *
 * Zero-knowledge: The IPA protocol is honest-verifier ZK. The committed
 * vectors (a, b) hide the wire values behind the Pedersen commitment.
 *
 * For relaxed R1CS (after Nova folding):
 *   (A_i · W) * (B_i · W) = u * (C_i · W) + E[i]
 */

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/ipa.h"
#include "zk/zkvm/transcript.h"

namespace dinero {
namespace zk {
namespace zkvm {

/**
 * R1CS IPA proof output — Nova Decider Proof.
 *
 * Proves that the FOLDED RELAXED R1CS INSTANCE from Nova is satisfiable:
 *   Given (commit_W, commit_E, u, x) from the Nova committed instance,
 *   ∃ W, E such that:
 *     commit_W = <W, H[0..n-1]> + r_W * Q
 *     commit_E = <E, H[n..n+m-1]> + r_E * Q
 *     ∀i: (A_i·z)(B_i·z) - u*(C_i·z) - E_i = 0   where z = (1 || x || W)
 *
 * The IPA uses extended vectors of length N = next_pow2(n + m):
 *   l = (M(α)·W - u*c(α) || α^0, ..., α^{m-1} || 0...0)
 *   r = (W || -E || 0...0)
 *   <l, r> = Σ_i α^i [(A_i·z)(B_i·z) - u*(C_i·z) - E_i] = 0
 *
 * A combined Schnorr proof opens the target = commit_W - commit_E:
 *   <r, H[0..N-1]> + (r_W - r_E) * Q = commit_W - commit_E
 */
struct R1CSIPAProof {
    // Witness binding: H(ρ || nonce) committed to transcript before α,
    // where ρ = (W || E) is the extended witness.
    std::vector<uint8_t> witness_hash;  // 32 bytes: SHA256(W || E || nonce)
    std::vector<uint8_t> witness_nonce; // 32 bytes: random nonce (for ZK)

    // Circuit identity: SHA256 of the R1CS structure (A, B, C matrices).
    std::vector<uint8_t> circuit_hash;  // 32 bytes: SHA256(R1CS structure)

    // Generator identity: SHA256(G[0]||H[0]||...||G[N-1]||H[N-1]).
    std::vector<uint8_t> gen_hash;  // 32 bytes: SHA256(IPA generators)

    Point wire_commitment; // Pedersen vector commitment to IPA vectors
    Point commitment;      // Same as wire_commitment (used by IPA verifier)
    IPAProof ipa;          // The inner product argument proof
    Scalar inner_product;  // The claimed inner product (zero for satisfied relaxed R1CS)

    // Logarithmic Pedersen vector opening proof.
    //
    // Proves: <r, H[0..N-1]> + r_combined * Q = target
    // where target = commit_W - commit_E (verifier-computable).
    //
    // Uses IPA-style folding: at each round, commit to left/right halves,
    // get challenge, fold. After log(N) rounds, verify with a single scalar.
    // Proof size: O(log N) group elements + 2 scalars ≈ 1KB (vs 530KB).
    struct PedersenOpeningProof {
        std::vector<Point> L;    // Left commits per round
        std::vector<Point> R;    // Right commits per round
        Scalar a_final;          // Final folded scalar
        Scalar blind_final;      // Final blinding
    };
    PedersenOpeningProof opening_proof;

    // Commitment binding: r_combined = r_W - r_E (32 bytes).
    //
    // The verifier derives the IPA commitment P from known values:
    //   P = <l, G> + (commit_W - commit_E) - r_combined * Q
    // where l is reconstructed from the R1CS structure + challenge α.
    //
    // The IPA verify against this derived P implicitly proves that
    // <r, H> = commit_W - commit_E - r_combined*Q, binding the IPA
    // witness to the Nova commitments WITHOUT an O(N) Schnorr response.
    //
    // This replaces the previous O(N) combined Schnorr opening proof
    // (schnorr_R, schnorr_response[N], schnorr_response_blind) with
    // a single scalar, reducing proof size from ~530KB to ~2KB.
    Scalar r_combined;  // r_W - r_E (blinding factor difference)
};

/**
 * Generate a Nova decider proof: prove the FOLDED relaxed R1CS instance
 * from Nova is satisfiable.
 *
 * Uses the extended-vector IPA encoding with r = (W || -E || 0) and
 * a combined Schnorr opening proof for commit_W and commit_E.
 *
 * @param cs              R1CS structure (A, B, C matrices) — used for constraint evaluation
 * @param W               Folded witness vector from Nova's running_witness_.W
 * @param E               Folded error vector from Nova's running_witness_.E
 * @param u               Folded relaxation scalar from Nova's running_instance_.u
 * @param commit_W        Nova's committed witness (verified by fold chain)
 * @param commit_E        Nova's committed error (verified by fold chain)
 * @param r_W             Blinding factor for commit_W
 * @param r_E             Blinding factor for commit_E
 * @param num_variables   Number of R1CS variables (determines generator split)
 * @param gens            Generator set (size >= next_pow2(n + m))
 * @param transcript      Fiat-Shamir transcript
 * @param ctx             secp256k1 context
 * @return                The decider proof
 */
R1CSIPAProof r1cs_ipa_prove(
    const R1CS& cs,
    const std::vector<Scalar>& W,
    const std::vector<Scalar>& E,
    const Scalar& u,
    const Point& commit_W,
    const Point& commit_E,
    const Scalar& r_W,
    const Scalar& r_E,
    size_t num_variables,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
);

/**
 * Verify a Nova decider proof.
 *
 * Verifies that the folded relaxed R1CS instance is satisfiable by checking:
 * 1. Circuit hash matches expected (prover used the correct constraint system)
 * 2. Generator hash matches (same IPA generators)
 * 3. Derives IPA commitment P from R1CS + commitments (logarithmic binding)
 * 4. IPA proof: <l, r> = 0 for the relaxed R1CS encoding
 *
 * The verifier reconstructs the left vector l from the R1CS structure and
 * challenge α, then computes P = <l, G> + (commit_W - commit_E) - r_combined*Q.
 * The IPA verify against this derived P implicitly proves commitment binding.
 *
 * @param proof               The decider proof
 * @param verifier_cs         Reconstructed R1CS (for building the l vector)
 * @param num_constraints     Number of R1CS constraints
 * @param num_variables       Number of R1CS variables
 * @param expected_circuit_hash  SHA256 of expected R1CS structure
 * @param commit_W            Nova's committed witness (verified by fold chain)
 * @param commit_E            Nova's committed error (verified by fold chain)
 * @param u                   Nova's folded relaxation scalar
 * @param gens                Generator set (same as used by prover)
 * @param transcript          Fiat-Shamir transcript (must match prover's)
 * @param ctx                 secp256k1 context
 * @return                    true if the proof is valid
 */
bool r1cs_ipa_verify(
    const R1CSIPAProof& proof,
    const R1CS& verifier_cs,
    size_t num_constraints,
    size_t num_variables,
    const std::vector<uint8_t>& expected_circuit_hash,
    const Point& commit_W,
    const Point& commit_E,
    const Scalar& u,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
);

// ---------------------------------------------------------------------------
// Pedersen vector commitment opening proof (logarithmic)
// ---------------------------------------------------------------------------

/**
 * Generate a logarithmic proof that <r, H[0..N-1]> + blind*Q = target.
 *
 * IPA-style folding: at each round, commit to left/right halves of r
 * against the opposite halves of H, get a Fiat-Shamir challenge, fold.
 * After log(N) rounds, the opening reduces to a single scalar.
 *
 * Cost: O(N) prover (one pass through r and H), O(log N) proof size.
 */
R1CSIPAProof::PedersenOpeningProof pedersen_opening_prove(
    const std::vector<Scalar>& r,
    const Scalar& blind,
    const Point& target,
    size_t N,
    const GeneratorSet& gens,
    secp256k1_context* ctx
);

/**
 * Verify a logarithmic Pedersen vector commitment opening proof.
 *
 * Checks that the folding is consistent and the final scalar satisfies
 * a_final * H_final + blind_final * Q == target_folded.
 */
bool pedersen_opening_verify(
    const R1CSIPAProof::PedersenOpeningProof& proof,
    const Point& target,
    const Scalar& r_combined,
    size_t N,
    const GeneratorSet& gens,
    secp256k1_context* ctx
);

/**
 * Compute a deterministic hash of the R1CS constraint structure (A, B, C).
 *
 * This hash identifies the "circuit" independent of the witness. Two R1CS
 * instances with the same constraints (same A, B, C matrices) will produce
 * the same hash. Used for P1 Audit Fix #3: the verifier independently
 * computes this hash from the public circuit definition and compares it
 * against the prover's committed value.
 *
 * @param cs  The R1CS constraint system (only structure is hashed, not witness)
 * @return    32-byte SHA256 hash
 */
std::vector<uint8_t> hash_r1cs_structure(const R1CS& cs);

/**
 * Compute the next power of 2 >= n.
 */
inline size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
