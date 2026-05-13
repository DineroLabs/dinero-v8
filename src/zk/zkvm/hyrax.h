// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Hyrax Matrix Commitment — O(√n) verification
 *
 * Replaces the flat Pedersen vector commitment used in the R1CS IPA with
 * a matrix decomposition that reduces the verifier's MSM from O(n) to O(√n).
 *
 * Construction (Wahby et al., "Doubly-Efficient zkSNARKs", S&P 2018):
 *   Split witness W[0..n-1] into a n_rows × n_cols matrix.
 *   Commit each row independently: C_i = <W_row_i, G[:n_cols_pad]>
 *
 * Evaluation proof at point r = (r_row, r_col):
 *   1. Verifier computes eq_row[i] = eq(r_row, bits(i)) for each row.
 *   2. Verifier computes D = Σ_i eq_row[i] · C_i  (MSM of n_rows points).
 *   3. Prover gives an IPA proof of <d_witness, b_col> = v
 *      where d_witness = Σ_i eq_row[i] · W_row_i (row-weighted sum)
 *      and   b_col[j]  = eq(r_col, bits(j))  for j ∈ [0, n_cols_pad).
 *   4. Verifier checks the IPA against D (size n_cols_pad, log₂ rounds).
 *
 * Verifier cost: O(n_rows) EC ops for D  +  O(n_cols_pad) IPA EC ops
 *              = O(√n) total EC operations.
 *
 * For n=205,764: n_cols=512, n_rows=402  → 402 + 1,025 ≈ 1,427 EC ops
 * vs current flat IPA: ~1,048,577 EC ops  →  ~735× speedup.
 */

#include "zk/zkvm/ipa.h"
#include "zk/zkvm/transcript.h"
#include <vector>
#include <cstddef>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// Hyrax parameters
// ---------------------------------------------------------------------------

struct HyraxParams {
    size_t n_rows;      // Number of matrix rows (= ceil(n / n_cols))
    size_t n_cols;      // Padded column count (power of 2)
    size_t n_total;     // n_rows * n_cols  (>= witness length)

    // Derive from witness length n.
    // n_cols = next_pow2(ceil(sqrt(n))), n_rows = ceil(n / n_cols).
    static HyraxParams from_n(size_t n);
};

// ---------------------------------------------------------------------------
// Commitment
// ---------------------------------------------------------------------------

/**
 * Hyrax witness commitment: one EC point per row.
 * C[i] = <W_row_i, G[:n_cols]>   (no blinding — the IPA binding handles ZK)
 */
struct HyraxCommitment {
    std::vector<Point> C;   // n_rows row commitments
    HyraxParams params;

    // Serialization
    std::vector<uint8_t> serialize(secp256k1_context* ctx) const;
    static bool deserialize(const std::vector<uint8_t>& data, size_t& offset,
                            HyraxCommitment& out, secp256k1_context* ctx);
};

HyraxCommitment hyrax_commit(
    const std::vector<Scalar>& W,   // witness vector (length n)
    const HyraxParams& params,
    const GeneratorSet& gens,       // must have size >= n_cols
    secp256k1_context* ctx
);

// ---------------------------------------------------------------------------
// Evaluation proof
// ---------------------------------------------------------------------------

/**
 * Proof that W̃(r) = v, where W̃ is the MLE of W.
 * r is split as (r_row, r_col) matching the matrix layout.
 */
struct HyraxEvalProof {
    IPAProof col_ipa;    // IPA proof of the column inner product (length n_cols)
    Scalar   claimed;   // The claimed evaluation value v

    std::vector<uint8_t> serialize(secp256k1_context* ctx) const;
    static bool deserialize(const std::vector<uint8_t>& data, size_t& offset,
                            HyraxEvalProof& out, secp256k1_context* ctx);
};

/**
 * Prove W̃(r) = v using the Hyrax matrix structure.
 *
 * @param W       Witness vector (length n ≤ params.n_total)
 * @param r       Evaluation point in F^{log(n_rows) + log(n_cols)}
 * @param params  Matrix layout
 * @param gens    Generator set (size >= n_cols)
 * @param transcript  Fiat-Shamir transcript (already bound to commitment)
 * @param ctx     secp256k1 context
 * @return        Evaluation proof
 */
HyraxEvalProof hyrax_eval_prove(
    const std::vector<Scalar>& W,
    const std::vector<Scalar>& r,
    const HyraxParams& params,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
);

/**
 * Verify a Hyrax evaluation proof.
 *
 * @param comm    Row commitments from hyrax_commit
 * @param r       Same evaluation point used in prove
 * @param proof   Evaluation proof from hyrax_eval_prove
 * @param params  Matrix layout (must match commitment)
 * @param gens    Same generators used in commit
 * @param transcript  Must match prover's transcript
 * @param ctx     secp256k1 context
 * @return        true if proof is valid
 */
bool hyrax_eval_verify(
    const HyraxCommitment& comm,
    const std::vector<Scalar>& r,
    const HyraxEvalProof& proof,
    const HyraxParams& params,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
);

// ---------------------------------------------------------------------------
// MLE utilities (used by Hyrax and sum-check)
// ---------------------------------------------------------------------------

/**
 * Evaluate the equality polynomial eq(r, x) at binary x = bits(i).
 *   eq(r, x) = Π_j (r_j * x_j + (1-r_j) * (1-x_j))
 *
 * For a vector of length m = 2^s and point r ∈ F^s:
 * Returns eq_vec[i] = eq(r, bits(i)) for i ∈ [0, m).
 * Cost: O(m) field multiplications.
 */
std::vector<Scalar> mle_eq_vec(const std::vector<Scalar>& r);

/**
 * Evaluate the MLE of a padded vector v at point r.
 *   v must have length 2^s; r must have length s.
 * Cost: O(|v|) field multiplications.
 */
Scalar mle_eval(const std::vector<Scalar>& v, const std::vector<Scalar>& r);

} // namespace zkvm
} // namespace zk
} // namespace dinero
