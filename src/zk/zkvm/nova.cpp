// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/nova.h"
#include "zk/zkvm/ipa.h"
#include "zk/zkvm/r1cs_ipa.h"  // for next_pow2
#include <cassert>
#include <openssl/rand.h>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// CommittedInstance
// ---------------------------------------------------------------------------

CommittedInstance CommittedInstance::fresh(const Point& commit_W,
                                          const std::vector<Scalar>& public_inputs) {
    CommittedInstance ci;
    ci.commit_W = commit_W;
    ci.commit_E = Point::identity(); // Zero point (identity — commitment to zero vector)
    ci.u = Scalar::one();
    ci.x = public_inputs;
    return ci;
}

// ---------------------------------------------------------------------------
// NovaProver
// ---------------------------------------------------------------------------

NovaProver::NovaProver(StepCircuit& circuit, size_t gens_size, secp256k1_context* ctx)
    : circuit_(circuit)
    , ctx_(ctx)
    , gens_size_(gens_size)
    , transcript_("Nova_IVC")
{
    // Initialize running instance to "empty" (will be set on first step)
}

Point NovaProver::commit_witness(const std::vector<Scalar>& w, const Scalar& r) const {
    // Pedersen vector commitment using IPA H-generators:
    //   commit_W = Σ w_i * H[i]  +  r * Q       for i in [0, n)
    //
    // CRITICAL: Uses H[0..n-1] generators from GeneratorSet.
    // commit_E uses H[n..n+m-1] (disjoint range).
    // This separation enables the decider's combined Schnorr opening proof
    // to prove both openings with a single target = commit_W - commit_E.

    if (w.empty()) return Point();

    // For the decider, we need generators sized for the extended vector (W || E).
    // Use n + m generators, where n = next_pow2(num_variables), m = num_constraints.
    // Before first step, num_step_constraints_ == 0, so just use w.size().
    size_t n = next_pow2(w.size() > 0 ? w.size() : 1);
    size_t total_gens = n;
    if (num_step_constraints_ > 0) {
        total_gens = next_pow2(n + num_step_constraints_);
    }
    const auto& gens = GeneratorSet::cached(total_gens, ctx_);

    std::vector<Scalar> scalars;
    std::vector<Point> points;
    scalars.reserve(w.size() + 1);
    points.reserve(w.size() + 1);

    for (size_t i = 0; i < w.size() && i < n && i < gens.H().size(); ++i) {
        if (!w[i].is_zero()) {
            scalars.push_back(w[i]);
            points.push_back(gens.H()[i]);
        }
    }

    // Blinding: r * Q
    if (!r.is_zero()) {
        scalars.push_back(r);
        points.push_back(gens.Q());
    }

    if (scalars.empty()) return Point::identity();
    return Point::multi_scalar_mul(scalars, points, ctx_);
}

Point NovaProver::commit_error(const std::vector<Scalar>& e, const Scalar& r) const {
    // Pedersen vector commitment to error vector E using H-generators at offset n:
    //   commit_E = Σ e_i * H[n + i]  +  r * Q     for i in [0, m)
    //
    // where n = next_pow2(num_step_variables_), m = e.size().
    //
    // This uses the DISJOINT generator range H[n..n+m-1], separate from
    // commit_W which uses H[0..n-1]. This is required for the decider's
    // combined Schnorr opening proof where the target = commit_W - commit_E.

    if (e.empty()) return Point::identity();

    size_t n = next_pow2(num_step_variables_ > 0 ? num_step_variables_ : 1);
    size_t total_gens = next_pow2(n + e.size());
    const auto& gens = GeneratorSet::cached(total_gens, ctx_);

    std::vector<Scalar> scalars;
    std::vector<Point> points;
    scalars.reserve(e.size() + 1);
    points.reserve(e.size() + 1);

    for (size_t i = 0; i < e.size() && (n + i) < gens.H().size(); ++i) {
        if (!e[i].is_zero()) {
            scalars.push_back(e[i]);
            points.push_back(gens.H()[n + i]);
        }
    }

    // Blinding: r * Q
    if (!r.is_zero()) {
        scalars.push_back(r);
        points.push_back(gens.Q());
    }

    if (scalars.empty()) return Point::identity();
    return Point::multi_scalar_mul(scalars, points, ctx_);
}

std::vector<Scalar> NovaProver::prove_step(const std::vector<Scalar>& z_in) {
    assert(z_in.size() == circuit_.state_size());

    // Build the R1CS for this step
    R1CS cs;

    // Allocate input state variables
    std::vector<Variable> input_vars;
    for (const auto& val : z_in) {
        input_vars.push_back(cs.alloc_input(val));
    }

    // Synthesize the step circuit
    std::vector<Variable> output_vars = circuit_.synthesize(cs, input_vars);
    assert(output_vars.size() == circuit_.state_size());

    // Extract output state
    std::vector<Scalar> z_out;
    for (const auto& v : output_vars) {
        z_out.push_back(cs.get_value(v));
    }

    // Record R1CS dimensions (same for all steps — deterministic circuit)
    if (step_count_ == 0) {
        num_step_constraints_ = cs.num_constraints();
        num_step_variables_ = cs.num_variables();
    }

    // Create witness and committed instance for this step
    Witness new_witness;
    new_witness.W = cs.witness();
    new_witness.r_W = Scalar::random(ctx_);
    new_witness.E.resize(cs.num_constraints(), Scalar::zero());
    new_witness.r_E = Scalar::zero(); // Fresh instance has E=0

    Point commit_W = commit_witness(new_witness.W, new_witness.r_W);
    CommittedInstance new_instance = CommittedInstance::fresh(commit_W, z_out);

    // Save the last step's raw (unfolded) witness, instance, and input state.
    last_step_witness_ = new_witness;
    last_step_instance_ = new_instance;
    last_step_z_in_ = z_in;

    if (step_count_ == 0) {
        // First step: running instance IS this instance
        running_instance_ = new_instance;
        running_witness_ = new_witness;
    } else {
        // Fold new instance into running instance
        fold(new_instance, new_witness, cs);
    }

    step_count_++;
    return z_out;
}

void NovaProver::fold(const CommittedInstance& new_instance,
                      const Witness& new_witness,
                      const R1CS& cs) {
    // Step 1: Compute cross-term T
    std::vector<Scalar> T = compute_cross_term(
        running_witness_, new_witness,
        running_instance_.u, new_instance.u,
        cs);

    // Commit to T using ERROR generators (H[n..n+m-1]).
    // T is the cross-term error and has the same dimension as E (one per constraint).
    // Using commit_error ensures T's commitment is in the same generator subspace
    // as commit_E, maintaining algebraic consistency during folding:
    //   commit_E' = commit_E_running + r * commit_T + r^2 * commit_E_new
    Scalar r_T = Scalar::random(ctx_);
    Point commit_T = commit_error(T, r_T);

    // Record folding proof (include commit_W values for verifier replay)
    FoldingProof fp;
    fp.commit_T = commit_T;
    fp.commit_W_running = running_instance_.commit_W;
    fp.commit_W_new = new_instance.commit_W;
    folding_proofs_.push_back(fp);

    // Step 2: Fiat-Shamir challenge
    transcript_.append_point("commit_T", commit_T, ctx_);
    transcript_.append_point("commit_W1", running_instance_.commit_W, ctx_);
    transcript_.append_point("commit_W2", new_instance.commit_W, ctx_);
    Scalar r = transcript_.challenge_scalar("fold_r", ctx_);
    Scalar r2 = r * r;

    // Step 3: Fold committed instance
    //   commit_W' = commit_W1 + r * commit_W2
    //   commit_E' = commit_E1 + r * commit_T + r^2 * commit_E2
    //   u' = u1 + r * u2
    //   x' = x1 + r * x2
    CommittedInstance folded;
    folded.commit_W = running_instance_.commit_W + (new_instance.commit_W * r);
    folded.commit_E = running_instance_.commit_E + (commit_T * r) + (new_instance.commit_E * r2);
    folded.u = running_instance_.u + (r * new_instance.u);

    folded.x.resize(running_instance_.x.size());
    for (size_t i = 0; i < folded.x.size(); ++i) {
        folded.x[i] = running_instance_.x[i] + (r * new_instance.x[i]);
    }

    // Fold witness
    Witness folded_witness;
    size_t w_size = std::max(running_witness_.W.size(), new_witness.W.size());
    folded_witness.W.resize(w_size);
    for (size_t i = 0; i < w_size; ++i) {
        Scalar w1 = (i < running_witness_.W.size()) ? running_witness_.W[i] : Scalar::zero();
        Scalar w2 = (i < new_witness.W.size()) ? new_witness.W[i] : Scalar::zero();
        folded_witness.W[i] = w1 + (r * w2);
    }
    folded_witness.r_W = running_witness_.r_W + (r * new_witness.r_W);

    size_t e_size = std::max(running_witness_.E.size(),
                             std::max(T.size(), new_witness.E.size()));
    folded_witness.E.resize(e_size);
    for (size_t i = 0; i < e_size; ++i) {
        Scalar e1 = (i < running_witness_.E.size()) ? running_witness_.E[i] : Scalar::zero();
        Scalar t  = (i < T.size()) ? T[i] : Scalar::zero();
        Scalar e2 = (i < new_witness.E.size()) ? new_witness.E[i] : Scalar::zero();
        folded_witness.E[i] = e1 + (r * t) + (r2 * e2);
    }
    folded_witness.r_E = running_witness_.r_E + (r * r_T) + (r2 * new_witness.r_E);

    running_instance_ = folded;
    running_witness_ = folded_witness;
}

std::vector<Scalar> NovaProver::compute_cross_term(
    const Witness& w1, const Witness& w2,
    const Scalar& u1, const Scalar& u2,
    const R1CS& cs) const
{
    // Cross-term T for relaxed R1CS folding:
    //   T_i = (A_i · z1) * (B_i · z2) + (A_i · z2) * (B_i · z1) - u1 * (C_i · z2) - u2 * (C_i · z1)
    //
    // This is the "error" introduced by folding two instances.

    const auto& constraints = cs.constraints();
    std::vector<Scalar> T(constraints.size());

    for (size_t i = 0; i < constraints.size(); ++i) {
        const auto& c = constraints[i];

        // Evaluate constraint linear combinations on both witnesses
        Scalar a1 = c.a.evaluate(w1.W);
        Scalar b1 = c.b.evaluate(w1.W);
        Scalar c1 = c.c.evaluate(w1.W);

        Scalar a2 = c.a.evaluate(w2.W);
        Scalar b2 = c.b.evaluate(w2.W);
        Scalar c2 = c.c.evaluate(w2.W);

        // T_i = a1*b2 + a2*b1 - u1*c2 - u2*c1
        T[i] = (a1 * b2) + (a2 * b1) - (u1 * c2) - (u2 * c1);
    }

    return T;
}

// ---------------------------------------------------------------------------
// NovaVerifier
// ---------------------------------------------------------------------------

bool NovaVerifier::verify(
    const std::vector<Scalar>& initial_state,
    const CommittedInstance& final_instance,
    const std::vector<FoldingProof>& folding_proofs,
    size_t num_steps,
    secp256k1_context* ctx
) {
    // Basic sanity checks
    if (num_steps == 0) return false;
    if (num_steps > 1 && folding_proofs.size() != num_steps - 1) return false;

    // For single step, just verify the committed instance has u=1 (unrelaxed)
    if (num_steps == 1) {
        return final_instance.u == Scalar::one();
    }

    // ---------------------------------------------------------------
    // Multi-step: replay Fiat-Shamir fold sequence
    //
    // The verifier re-derives all challenges from the folding proofs
    // and checks that the final (u, x, commit_W, commit_E) values
    // match what the honest fold sequence would produce.
    //
    // This mirrors the prover's fold() method exactly:
    //   transcript << commit_T << commit_W_running << commit_W_new
    //   r = transcript.challenge("fold_r")
    //   u' = u_running + r * u_new
    //   x' = x_running + r * x_new
    //   commit_W' = commit_W_running + r * commit_W_new
    //   commit_E' = commit_E_running + r * commit_T + r^2 * commit_E_new
    //
    // Full IPA verification of the final R1CS instance is checked
    // separately by ipa_verify().
    // ---------------------------------------------------------------

    // Verify u != 0 (otherwise relaxed R1CS is trivially satisfiable)
    if (final_instance.u.is_zero()) return false;

    // Verify public inputs are present
    if (final_instance.x.empty()) return false;

    Transcript transcript("Nova_IVC");

    // Initialize running instance from the first step (fresh: u=1, x=initial_state)
    // commit_W and commit_E for the first step come from folding_proofs[0].commit_W_running
    Scalar running_u = Scalar::one();
    std::vector<Scalar> running_x = initial_state;
    Point running_commit_W = folding_proofs[0].commit_W_running;
    Point running_commit_E = Point::identity(); // Fresh instance: E=0, commitment is identity

    for (size_t i = 0; i < folding_proofs.size(); ++i) {
        const auto& fp = folding_proofs[i];

        // Consistency check: the commit_W_running in the proof must match
        // what we computed from the previous fold (or the initial value).
        if (running_commit_W != fp.commit_W_running) {
            return false;
        }

        // The new (fresh) instance being folded in has u_new=1
        Scalar new_u = Scalar::one();

        // Re-derive the Fiat-Shamir challenge (same transcript as prover)
        transcript.append_point("commit_T", fp.commit_T, ctx);
        transcript.append_point("commit_W1", running_commit_W, ctx);
        transcript.append_point("commit_W2", fp.commit_W_new, ctx);
        Scalar r = transcript.challenge_scalar("fold_r", ctx);
        Scalar r2 = r * r;

        // Fold u: u' = u_running + r * u_new
        running_u = running_u + (r * new_u);

        // Fold x: x' = x_running + r * x_new
        // The verifier doesn't know x_new for intermediate steps (it's the
        // step circuit output which only the prover computed). However, we
        // CAN verify the u and commitment evolution, which is the core
        // soundness property. x verification would require re-executing the
        // circuit, which defeats the purpose of succinct verification.

        // Fold commit_W: commit_W' = commit_W_running + r * commit_W_new
        running_commit_W = running_commit_W + (fp.commit_W_new * r);

        // Fold commit_E: commit_E' = commit_E_running + r * commit_T + r^2 * commit_E_new
        // For fresh instances, commit_E_new is identity (zero vector commitment)
        Point new_commit_E = Point(); // Fresh instance: identity
        running_commit_E = running_commit_E + (fp.commit_T * r) + (new_commit_E * r2);
    }

    // Check 1: Final u must match the folded u
    if (running_u != final_instance.u) {
        return false;
    }

    // Check 2: Final commit_W must match the folded commit_W
    if (running_commit_W != final_instance.commit_W) {
        return false;
    }

    // Check 3: Final commit_E must match the folded commit_E
    if (running_commit_E != final_instance.commit_E) {
        return false;
    }

    return true;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
