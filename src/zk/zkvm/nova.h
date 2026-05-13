// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Nova IVC (Incremental Verifiable Computation) on secp256k1
 *
 * Nova is a folding scheme that proves sequential computation:
 *   F(state_0) -> state_1 -> state_2 -> ... -> state_n
 *
 * Each step is one Tapscript opcode execution. Instead of proving
 * n separate R1CS instances, Nova folds them into a single instance
 * that can be verified with one final proof.
 *
 * The key insight: two R1CS instances can be "folded" into one
 * relaxed R1CS instance. The folded instance is satisfiable IFF
 * both original instances were satisfiable.
 *
 * Relaxed R1CS:
 *   (A · z) ∘ (B · z) = u · (C · z) + E
 * where u=1 and E=0 for standard (unrelaxed) R1CS.
 *
 * Folding two instances (z1, u1, E1) and (z2, u2, E2):
 *   1. Prover commits to cross-term T
 *   2. Verifier sends random challenge r
 *   3. Folded instance: z' = z1 + r*z2, u' = u1 + r*u2, E' = E1 + r*T + r²*E2
 *
 * After n folds, produce one Bulletproofs IPA proof for the final instance.
 *
 * Based on: Kothapalli, Setty, Tzialla, "Nova: Recursive Zero-Knowledge
 * Arguments from Folding Schemes" (CRYPTO 2022).
 *
 * This is the first implementation of Nova on secp256k1.
 * No curve cycles needed — we don't recurse proofs, just fold instances.
 */

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/scalar.h"
#include "zk/zkvm/transcript.h"
#include <vector>
#include <functional>

namespace dinero {
namespace zk {
namespace zkvm {

/**
 * A committed relaxed R1CS instance.
 *
 * The "committed" part means the witness is hidden behind a Pedersen
 * commitment, so the verifier never sees the actual witness values.
 */
struct CommittedInstance {
    Point commit_W;  // Pedersen commitment to witness W
    Point commit_E;  // Pedersen commitment to error vector E
    Scalar u;        // Relaxation scalar (u=1 for unrelaxed)
    std::vector<Scalar> x; // Public inputs

    // For unrelaxed (fresh) instances
    static CommittedInstance fresh(const Point& commit_W,
                                   const std::vector<Scalar>& public_inputs);
};

/**
 * Witness for a committed instance.
 * Kept by the prover, never revealed.
 */
struct Witness {
    std::vector<Scalar> W;       // Full witness vector (private + public)
    Scalar r_W;                  // Randomness for commit_W
    std::vector<Scalar> E;       // Error vector
    Scalar r_E;                  // Randomness for commit_E
};

/**
 * Nova folding proof.
 * Sent from prover to verifier at each fold step.
 *
 * Includes the intermediate commit_W values so the verifier can replay
 * the exact Fiat-Shamir transcript and re-derive challenges.
 */
struct FoldingProof {
    Point commit_T;        // Commitment to cross-term T
    Point commit_W_running; // commit_W of the running instance at this fold step
    Point commit_W_new;     // commit_W of the new (fresh) instance being folded in
};

/**
 * Step circuit interface.
 *
 * Implement this for your computation step. For ZK Tapscript, each step
 * is one opcode execution.
 *
 * The circuit takes:
 *   - z_in: input state (public inputs from previous step)
 *   - Returns z_out: output state (public inputs for next step)
 *
 * The circuit builds R1CS constraints that enforce the state transition.
 */
class StepCircuit {
public:
    virtual ~StepCircuit() = default;

    // Number of state variables (public input/output per step)
    virtual size_t state_size() const = 0;

    // Synthesize the step circuit:
    // Given input state z_in (as allocated variables), produce output state z_out.
    // The circuit adds constraints to cs that enforce the transition.
    virtual std::vector<Variable> synthesize(
        R1CS& cs,
        const std::vector<Variable>& z_in
    ) = 0;
};

/**
 * Nova IVC Prover
 *
 * Manages the running folded instance and produces folding proofs
 * at each step. After all steps, call finalize() to get the
 * final proof.
 */
class NovaProver {
public:
    /**
     * Initialize the IVC with a step circuit and generator set.
     *
     * @param circuit   The step circuit to prove repeated execution of
     * @param gens_size Number of generators (must be >= max R1CS variables)
     * @param ctx       secp256k1 context
     */
    NovaProver(StepCircuit& circuit, size_t gens_size, secp256k1_context* ctx);

    /**
     * Execute and prove one step.
     *
     * @param z_in   Input state for this step
     * @return       Output state after step execution
     */
    std::vector<Scalar> prove_step(const std::vector<Scalar>& z_in);

    /**
     * Get the current folded committed instance.
     * The verifier needs this to check the final proof.
     */
    const CommittedInstance& running_instance() const { return running_instance_; }

    /**
     * Get all folding proofs (one per step after the first).
     */
    const std::vector<FoldingProof>& folding_proofs() const { return folding_proofs_; }

    /**
     * Number of steps proved so far.
     */
    size_t num_steps() const { return step_count_; }

    /**
     * Get the final witness for the folded instance.
     * Needed by the IPA prover to generate the final proof.
     */
    const Witness& running_witness() const { return running_witness_; }

    /**
     * Get the number of R1CS constraints per step.
     * Needed by the decider to determine generator layout:
     * commit_W uses H[0..n-1], commit_E uses H[n..n+m-1].
     */
    size_t num_step_constraints() const { return num_step_constraints_; }

    /**
     * Get the number of R1CS variables per step.
     * Needed by the decider to size the IPA vectors.
     */
    size_t num_step_variables() const { return num_step_variables_; }

    /**
     * Get the LAST STEP's raw (unfolded) witness.
     * For multi-step proofs where the step circuit has witness-dependent
     * constraint structure, the decider proves THIS witness satisfies the
     * standard R1CS (u=1, E=0). The Schnorr opening proof ties it to
     * the last step's commit_W (verified by the fold chain).
     */
    const Witness& last_step_witness() const { return last_step_witness_; }

    /**
     * Get the last step's committed instance (before folding).
     */
    const CommittedInstance& last_step_instance() const { return last_step_instance_; }
    const std::vector<Scalar>& last_step_z_in() const { return last_step_z_in_; }

private:
    StepCircuit& circuit_;
    secp256k1_context* ctx_;
    size_t gens_size_;

    // Running IVC state
    CommittedInstance running_instance_;
    Witness running_witness_;
    size_t step_count_ = 0;

    // R1CS dimensions (set on first step)
    size_t num_step_constraints_ = 0;
    size_t num_step_variables_ = 0;

    // Last step's raw (unfolded) witness, instance, and input state
    Witness last_step_witness_;
    CommittedInstance last_step_instance_;
    std::vector<Scalar> last_step_z_in_;

    // Folding proofs for all steps
    std::vector<FoldingProof> folding_proofs_;
    Transcript transcript_;

    // Fold a new instance into the running instance
    void fold(const CommittedInstance& new_instance,
              const Witness& new_witness,
              const R1CS& cs);

    // Compute cross-term T for folding
    std::vector<Scalar> compute_cross_term(
        const Witness& w1, const Witness& w2,
        const Scalar& u1, const Scalar& u2,
        const R1CS& cs) const;

    // Commit to a witness vector using Pedersen commitment.
    // Uses H[0..w.size()-1] generators from GeneratorSet.
    Point commit_witness(const std::vector<Scalar>& w, const Scalar& r) const;

    // Commit to an error vector using Pedersen commitment.
    // Uses H[n..n+e.size()-1] generators where n = next_pow2(num_step_variables_).
    // This ensures commit_E and commit_W use disjoint generator ranges,
    // which is essential for the decider's combined Schnorr opening proof.
    Point commit_error(const std::vector<Scalar>& e, const Scalar& r) const;
};

/**
 * Nova IVC Verifier
 *
 * Verifies the folded instance by:
 * 1. Re-deriving all Fiat-Shamir challenges
 * 2. Re-folding the committed instances
 * 3. Checking the final IPA proof
 */
class NovaVerifier {
public:
    /**
     * Verify a Nova IVC proof.
     *
     * @param initial_state   The starting state z_0
     * @param final_instance  The final folded committed instance
     * @param folding_proofs  All folding proofs (one per step after first)
     * @param num_steps       Number of computation steps
     * @param ctx             secp256k1 context
     * @return                true if the IVC proof is valid
     */
    static bool verify(
        const std::vector<Scalar>& initial_state,
        const CommittedInstance& final_instance,
        const std::vector<FoldingProof>& folding_proofs,
        size_t num_steps,
        secp256k1_context* ctx
    );
};

} // namespace zkvm
} // namespace zk
} // namespace dinero
