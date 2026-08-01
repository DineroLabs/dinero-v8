// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/r1cs_spartan.h"
#include "crypto/sha256.h"
#include <cassert>
#include <cstring>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

static size_t next_pow2(size_t n) {
    if (n <= 1) return 1;
    --n;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16; n |= n >> 32;
    return n + 1;
}

// Exact log2 for power-of-2 n.
static size_t log2_exact(size_t n) {
    assert(n > 0 && (n & (n - 1)) == 0);
    size_t k = 0; while ((size_t(1) << k) < n) ++k; return k;
}

// Append a scalar to transcript with a fixed label.
static void ts_scalar(Transcript& t, const Scalar& s) {
    t.append_scalar("sc", s);
}

// ---------------------------------------------------------------------------
// Generic bilinear sum-check prover (degree-2, used for inner sum-check)
//
// Proves: ∑_{x ∈ {0,1}^s} f_table[x] * g_table[x] = initial_claim
//
// Both tables must have size 2^s (power of 2).
// Tables are folded in-place; at exit, f_table[0]*g_table[0] = final_oracle.
//
// Returns: the per-round (g(0), g(1), g(2)) messages, and stores the
//          per-round Fiat-Shamir challenges in `challenges_out`.
// ---------------------------------------------------------------------------
std::vector<std::array<Scalar, 3>> sc_prove(
    std::vector<Scalar>& f_table,
    std::vector<Scalar>& g_table,
    Transcript& transcript,
    secp256k1_context* ctx,
    std::vector<Scalar>& challenges_out
) {
    const size_t total = f_table.size();
    assert(total == g_table.size());
    assert(total > 0 && (total & (total - 1)) == 0);
    const size_t rounds = log2_exact(total);

    std::vector<std::array<Scalar, 3>> msgs;
    msgs.reserve(rounds);
    challenges_out.reserve(rounds);

    size_t S = total;
    for (size_t k = 0; k < rounds; ++k) {
        const size_t half = S / 2;
        Scalar g0 = Scalar::zero(), g1 = Scalar::zero(), g2 = Scalar::zero();
        for (size_t j = 0; j < half; ++j) {
            const Scalar& A = f_table[j];
            const Scalar& B = f_table[half + j];
            const Scalar& C = g_table[j];
            const Scalar& D = g_table[half + j];
            g0 += A * C;
            g1 += B * D;
            // g_k(2) by extrapolation: (2B-A)*(2D-C)
            const Scalar two_B_mA = B + B - A;
            const Scalar two_D_mC = D + D - C;
            g2 += two_B_mA * two_D_mC;
        }

        ts_scalar(transcript, g0);
        ts_scalar(transcript, g1);
        ts_scalar(transcript, g2);
        const Scalar r = transcript.challenge_scalar("scr", ctx);
        challenges_out.push_back(r);

        // Fold: f[j] = f[j] + r*(f[half+j] - f[j]),  g[j] = g[j] + r*(g[half+j]-g[j])
        for (size_t j = 0; j < half; ++j) {
            f_table[j] = f_table[j] + r * (f_table[half + j] - f_table[j]);
            g_table[j] = g_table[j] + r * (g_table[half + j] - g_table[j]);
        }
        f_table.resize(half);
        g_table.resize(half);
        S = half;

        msgs.push_back({g0, g1, g2});
    }
    return msgs;
}

// ---------------------------------------------------------------------------
// Generic bilinear sum-check verifier (degree-2, used for inner sum-check)
//
// Verifies that the round messages are consistent with initial_claim.
// At exit, writes the final sum-check oracle claim to `final_claim_out`
// and the per-round challenges to `challenges_out`.
// Returns false on check failure.
// ---------------------------------------------------------------------------
bool sc_verify(
    const std::vector<std::array<Scalar, 3>>& msgs,
    const Scalar& initial_claim,
    Transcript& transcript,
    secp256k1_context* ctx,
    std::vector<Scalar>& challenges_out,
    Scalar& final_claim_out
) {
    Scalar claim = initial_claim;
    challenges_out.reserve(msgs.size());

    for (const auto& [g0, g1, g2] : msgs) {
        // Check g(0) + g(1) == claim
        if (!(g0 + g1 == claim)) return false;

        ts_scalar(transcript, g0);
        ts_scalar(transcript, g1);
        ts_scalar(transcript, g2);
        const Scalar r = transcript.challenge_scalar("scr", ctx);
        challenges_out.push_back(r);

        // Next claim = g(r): interpolate degree-2 poly through (0,g0),(1,g1),(2,g2)
        // g(r) = g0*(r-1)*(r-2)/2 - g1*r*(r-2) + g2*r*(r-1)/2
        // Using Lagrange basis at 0,1,2:
        const Scalar r_m1 = r - Scalar::one();
        const Scalar r_m2 = r - Scalar(uint64_t(2));
        const Scalar inv2 = Scalar(uint64_t(2)).inverse(ctx);
        claim = g0 * r_m1 * r_m2 * inv2
              - g1 * r * r_m2
              + g2 * r * r_m1 * inv2;
    }
    final_claim_out = claim;
    return true;
}

// ---------------------------------------------------------------------------
// Trilinear (degree-3) outer sum-check prover
//
// Proves: ∑_{x ∈ {0,1}^s} eq_table[x] * A_table[x] * B_table[x] = initial_claim
// (where the full sumcheck polynomial is eq(τ,x) * (A(rx_m,x)·z * B(rx_m,x)·z - u·C(rx_m,x)·z - E(x)))
//
// All four tables must have size 2^s (power of 2). They are folded in-place.
//
// At each round we need g(0), g(1), g(2), g(3):
//   g(t) = ∑_{j < half} eq_lo(t,j) * A_lo(t,j) * B_lo(t,j) - u*C_lo(t,j) - E_lo(t,j)
// where each "lo(t,j)" = table[j] + t*(table[half+j] - table[j])
//
// Returns the per-round (g0,g1,g2,g3) messages and stores per-round challenges.
// ---------------------------------------------------------------------------
std::vector<std::array<Scalar, 4>> sc_prove_trilinear(
    std::vector<Scalar>& eq_table,
    std::vector<Scalar>& A_table,
    std::vector<Scalar>& B_table,
    std::vector<Scalar>& C_table,
    std::vector<Scalar>& E_table,
    const Scalar& u,
    Transcript& transcript,
    secp256k1_context* ctx,
    std::vector<Scalar>& challenges_out
) {
    const size_t total = eq_table.size();
    assert(total == A_table.size() && total == B_table.size() &&
           total == C_table.size() && total == E_table.size());
    assert(total > 0 && (total & (total - 1)) == 0);
    const size_t rounds = log2_exact(total);

    const Scalar one = Scalar::one();
    const Scalar two = Scalar(uint64_t(2));
    const Scalar three = Scalar(uint64_t(3));

    std::vector<std::array<Scalar, 4>> msgs;
    msgs.reserve(rounds);
    challenges_out.reserve(rounds);

    size_t S = total;
    for (size_t k = 0; k < rounds; ++k) {
        const size_t half = S / 2;

        // For each t in {0,1,2,3}, compute g(t) = ∑_{j<half} f(t,j)
        // where f(t,j) = eq(t,j) * A(t,j) * B(t,j) - u * C(t,j) - E(t,j)
        // and X(t,j) = X_lo[j] + t * (X_hi[j] - X_lo[j]) for each table X.
        Scalar g0 = Scalar::zero(), g1 = Scalar::zero(),
               g2 = Scalar::zero(), g3 = Scalar::zero();

        for (size_t j = 0; j < half; ++j) {
            const Scalar& eq0 = eq_table[j];
            const Scalar  deq = eq_table[half + j] - eq_table[j];
            const Scalar& a0  = A_table[j];
            const Scalar  da  = A_table[half + j] - A_table[j];
            const Scalar& b0  = B_table[j];
            const Scalar  db  = B_table[half + j] - B_table[j];
            const Scalar& c0  = C_table[j];
            const Scalar  dc  = C_table[half + j] - C_table[j];
            const Scalar& e0  = E_table[j];
            const Scalar  de  = E_table[half + j] - E_table[j];

            // t=0: g(0) += eq(0,j) * (A(0,j)*B(0,j) - u*C(0,j) - E(0,j))
            g0 += eq0 * (a0 * b0 - u * c0 - e0);

            // t=1
            const Scalar eq1 = eq0 + deq;
            const Scalar a1  = a0  + da;
            const Scalar b1  = b0  + db;
            const Scalar c1  = c0  + dc;
            const Scalar e1  = e0  + de;
            g1 += eq1 * (a1 * b1 - u * c1 - e1);

            // t=2
            const Scalar eq2 = eq0 + two * deq;
            const Scalar a2  = a0  + two * da;
            const Scalar b2  = b0  + two * db;
            const Scalar c2  = c0  + two * dc;
            const Scalar e2  = e0  + two * de;
            g2 += eq2 * (a2 * b2 - u * c2 - e2);

            // t=3
            const Scalar eq3 = eq0 + three * deq;
            const Scalar a3  = a0  + three * da;
            const Scalar b3  = b0  + three * db;
            const Scalar c3  = c0  + three * dc;
            const Scalar e3  = e0  + three * de;
            g3 += eq3 * (a3 * b3 - u * c3 - e3);
        }

        ts_scalar(transcript, g0);
        ts_scalar(transcript, g1);
        ts_scalar(transcript, g2);
        ts_scalar(transcript, g3);
        const Scalar r = transcript.challenge_scalar("scr", ctx);
        challenges_out.push_back(r);

        // Fold all tables: X[j] = X[j] + r*(X[half+j] - X[j])
        for (size_t j = 0; j < half; ++j) {
            eq_table[j] = eq_table[j] + r * (eq_table[half + j] - eq_table[j]);
            A_table[j]  = A_table[j]  + r * (A_table[half + j]  - A_table[j]);
            B_table[j]  = B_table[j]  + r * (B_table[half + j]  - B_table[j]);
            C_table[j]  = C_table[j]  + r * (C_table[half + j]  - C_table[j]);
            E_table[j]  = E_table[j]  + r * (E_table[half + j]  - E_table[j]);
        }
        eq_table.resize(half);
        A_table.resize(half);
        B_table.resize(half);
        C_table.resize(half);
        E_table.resize(half);
        S = half;

        msgs.push_back({g0, g1, g2, g3});
    }
    return msgs;
}

// ---------------------------------------------------------------------------
// Trilinear (degree-3) outer sum-check verifier
//
// Verifies that the round messages are consistent with initial_claim.
// Returns false on check failure; writes final oracle value and challenges.
// ---------------------------------------------------------------------------
bool sc_verify_trilinear(
    const std::vector<std::array<Scalar, 4>>& msgs,
    const Scalar& initial_claim,
    Transcript& transcript,
    secp256k1_context* ctx,
    std::vector<Scalar>& challenges_out,
    Scalar& final_claim_out
) {
    Scalar claim = initial_claim;
    challenges_out.reserve(msgs.size());

    // Precompute inverses needed for degree-3 Lagrange interpolation at 0,1,2,3.
    // g(r) = ∑_{i=0}^{3} g_i * ∏_{j≠i} (r-j)/(i-j)
    // Denominators: (0-1)(0-2)(0-3)=-6, (1-0)(1-2)(1-3)=2, (2-0)(2-1)(2-3)=(-2), (3-0)(3-1)(3-2)=6
    // So Lagrange basis: L0 = (r-1)(r-2)(r-3)/(-6)
    //                    L1 = r(r-2)(r-3)/2
    //                    L2 = r(r-1)(r-3)/(-2)
    //                    L3 = r(r-1)(r-2)/6
    const Scalar inv6 = Scalar(uint64_t(6)).inverse(ctx);
    const Scalar inv2 = Scalar(uint64_t(2)).inverse(ctx);

    for (const auto& msg : msgs) {
        const Scalar& g0 = msg[0];
        const Scalar& g1 = msg[1];
        const Scalar& g2 = msg[2];
        const Scalar& g3 = msg[3];

        // Check g(0) + g(1) == claim
        if (!(g0 + g1 == claim)) return false;

        ts_scalar(transcript, g0);
        ts_scalar(transcript, g1);
        ts_scalar(transcript, g2);
        ts_scalar(transcript, g3);
        const Scalar r = transcript.challenge_scalar("scr", ctx);
        challenges_out.push_back(r);

        // Degree-3 Lagrange interpolation at r:
        const Scalar one  = Scalar::one();
        const Scalar two  = Scalar(uint64_t(2));
        const Scalar three = Scalar(uint64_t(3));
        const Scalar r_m1 = r - one;
        const Scalar r_m2 = r - two;
        const Scalar r_m3 = r - three;

        // L0(r) = (r-1)(r-2)(r-3) / (-6)
        // L1(r) = r(r-2)(r-3) / 2
        // L2(r) = r(r-1)(r-3) / (-2)
        // L3(r) = r(r-1)(r-2) / 6
        const Scalar neg_inv6 = Scalar::zero() - inv6;
        const Scalar neg_inv2 = Scalar::zero() - inv2;
        claim = g0 * r_m1 * r_m2 * r_m3 * neg_inv6
              + g1 * r   * r_m2 * r_m3 * inv2
              + g2 * r   * r_m1 * r_m3 * neg_inv2
              + g3 * r   * r_m1 * r_m2 * inv6;
    }
    final_claim_out = claim;
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Circuit structure hash
// ---------------------------------------------------------------------------

std::vector<uint8_t> spartan_hash_r1cs_structure(const R1CS& cs) {
    uint8_t hash[32];
    dinero::crypto::CSHA256 h;
    uint64_t nc = cs.num_constraints();
    uint64_t nv = cs.num_variables();
    h.Write(reinterpret_cast<const uint8_t*>(&nc), sizeof(nc));
    h.Write(reinterpret_cast<const uint8_t*>(&nv), sizeof(nv));
    for (const auto& c : cs.constraints()) {
        auto hash_lc = [&](const LinearCombination& lc) {
            uint64_t nt = lc.terms().size();
            h.Write(reinterpret_cast<const uint8_t*>(&nt), sizeof(nt));
            for (const auto& t : lc.terms()) {
                h.Write(reinterpret_cast<const uint8_t*>(&t.var.index), sizeof(t.var.index));
                h.Write(t.coeff.data(), 32);
            }
        };
        hash_lc(c.a); hash_lc(c.b); hash_lc(c.c);
    }
    h.Finalize(hash);
    return std::vector<uint8_t>(hash, hash + 32);
}

// ---------------------------------------------------------------------------
// Spartan prover
// ---------------------------------------------------------------------------

SpartanProof r1cs_spartan_prove(
    const R1CS& cs,
    const std::vector<Scalar>& E,
    const Scalar& u,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx,
    bool bind_public_inputs
) {
    SpartanProof proof;

    const std::vector<Scalar>& z = cs.witness();
    const size_t n_z  = z.size();                        // actual witness size
    const size_t n_zp = next_pow2(n_z);                  // padded for sum-check
    const size_t m    = cs.num_constraints();
    const size_t m_p  = next_pow2(m);                    // padded for sum-check

    // SECURITY (CONFIRMED-CRIT-05 fix, 2026-05-30) — Spartan z=(1, io, W) split.
    // When bind_public_inputs (the default), commit/open ONLY the private witness W:
    // zero the public slots (index 0 = the constant ONE, 1..num_inputs = public inputs).
    // The verifier recomputes the public/io contribution to z̃(ry) itself from the
    // on-chain public inputs and adds it back. Without this split the public inputs are
    // unbound and forgeable (see ShieldedSpendCircuitTest.SUSPECTED01_AnchorBindingPoC).
    // The sum-check below always runs over the FULL z; only the commitment + eval use
    // the (possibly public-zeroed) vector. bind_public_inputs=false reproduces the
    // pre-fix unbound rule — retained ONLY for pre-activation-height history validation.
    const size_t num_pub = 1 + cs.num_inputs();          // constant ONE + public inputs
    std::vector<Scalar> z_priv = z;
    if (bind_public_inputs) {
        for (size_t j = 0; j < num_pub && j < z_priv.size(); ++j) {
            z_priv[j] = Scalar::zero();
        }
    }

    // Hyrax parameters
    const HyraxParams H_z = HyraxParams::from_n(n_z);
    const HyraxParams H_E = HyraxParams::from_n(m);

    // Generator requirement: n_cols max of H_z and H_E (both = 512 typically)
    const size_t gens_need = std::max(H_z.n_cols, H_E.n_cols);
    assert(gens.size() >= gens_need);

    // --- Step 1: Commit private witness and error vector ---
    proof.comm_W = hyrax_commit(z_priv,                      H_z, gens, ctx);
    proof.comm_E = hyrax_commit(std::vector<Scalar>(E.begin(), E.end()), H_E, gens, ctx);
    proof.circuit_hash = spartan_hash_r1cs_structure(cs);

    // Bind commitments, relaxation scalar, and circuit hash to transcript.
    // The caller (ring_covenant.cpp) should append any Nova committed instance
    // data (commit_W, commit_E from Nova) to the transcript BEFORE calling this,
    // so those external commitments are also included in the Fiat-Shamir hash.
    transcript.append_scalar("spartan_u", u);
    for (const Point& p : proof.comm_W.C)
        transcript.append_point("hW", p, ctx);
    for (const Point& p : proof.comm_E.C)
        transcript.append_point("hE", p, ctx);
    transcript.append_scalar("chash", Scalar(proof.circuit_hash.data()));

    // --- Step 2: Derive outer sum-check challenge τ ∈ F^{log_m} ---
    const size_t log_m = log2_exact(m_p);
    std::vector<Scalar> tau;
    tau.reserve(log_m);
    for (size_t k = 0; k < log_m; ++k)
        tau.push_back(transcript.challenge_scalar("tau", ctx));

    // --- Step 3: Build outer sum-check tables ---
    // Trilinear (degree-3) outer sum-check: polynomial is
    //   ∑_{x ∈ {0,1}^{log_m}} eq(τ,x) * (A(x)·z * B(x)·z - u·C(x)·z - E(x))
    // Keep eq, A·z, B·z, C·z, E separate so the prover can evaluate at t=0,1,2,3.
    std::vector<Scalar> eq_table = mle_eq_vec(tau);          // size m_p
    std::vector<Scalar> A_table(m_p, Scalar::zero());
    std::vector<Scalar> B_table(m_p, Scalar::zero());
    std::vector<Scalar> C_table(m_p, Scalar::zero());
    std::vector<Scalar> E_table(m_p, Scalar::zero());

    const auto& constraints = cs.constraints();
    for (size_t i = 0; i < m; ++i) {
        A_table[i] = constraints[i].a.evaluate(z);
        B_table[i] = constraints[i].b.evaluate(z);
        C_table[i] = constraints[i].c.evaluate(z);
        E_table[i] = (i < E.size()) ? E[i] : Scalar::zero();
    }

    // --- Step 4: Run outer sum-check (degree-3, initial claim = 0) ---
    // Tables folded in-place; after log_m rounds, size = 1.
    // sc_prove_trilinear folds MSB-first; reverse challenges for LSB-first MLE convention.
    std::vector<Scalar> outer_challenges;
    proof.outer_sc = sc_prove_trilinear(eq_table, A_table, B_table, C_table, E_table,
                                         u, transcript, ctx, outer_challenges);
    // rx_m: reversed so that rx_m[k] corresponds to bit k (LSB-first, for mle_eq_vec)
    const std::vector<Scalar> rx_m(outer_challenges.rbegin(), outer_challenges.rend());

    // --- Step 5: Compute Az, Bz, Cz, Ez at rx_m ---
    // These are the oracle queries the verifier needs.
    // A_i·z for all i, then weighted by eq(rx_m, i).
    const std::vector<Scalar> eq_rx_m = mle_eq_vec(rx_m);  // size m_p

    Scalar Az = Scalar::zero();
    Scalar Bz_s = Scalar::zero();
    Scalar Cz_s = Scalar::zero();
    Scalar Ez_s = Scalar::zero();
    for (size_t i = 0; i < m; ++i) {
        const Scalar eq_i = eq_rx_m[i];
        Az  += eq_i * constraints[i].a.evaluate(z);
        Bz_s += eq_i * constraints[i].b.evaluate(z);
        Cz_s += eq_i * constraints[i].c.evaluate(z);
        Ez_s += eq_i * ((i < E.size()) ? E[i] : Scalar::zero());
    }
    proof.Az_claim = Az;
    proof.Bz_claim = Bz_s;
    proof.Cz_claim = Cz_s;
    proof.Ez_claim = Ez_s;

    // Append claims to transcript
    transcript.append_scalar("Az", Az);
    transcript.append_scalar("Bz", Bz_s);
    transcript.append_scalar("Cz", Cz_s);
    transcript.append_scalar("Ez", Ez_s);

    // --- Step 6: Batch challenge ρ ---
    const Scalar rho  = transcript.challenge_scalar("rho", ctx);
    const Scalar rho2 = rho * rho;

    // Combined inner claim: Az + ρ·Bz + ρ²·Cz
    const Scalar vz_combined = Az + rho * Bz_s + rho2 * Cz_s;

    // --- Step 7: Build inner sum-check tables ---
    // M_table[j] = Ã(rx_m, j) + ρ·B̃(rx_m, j) + ρ²·C̃(rx_m, j)  for j ∈ [0, n_zp)
    // z_table[j] = z[j]  for j ∈ [0, n_z), else 0
    std::vector<Scalar> M_table(n_zp, Scalar::zero());
    for (size_t i = 0; i < m; ++i) {
        const Scalar eq_i = eq_rx_m[i];
        for (const auto& t : constraints[i].a.terms()) {
            if (t.var.index < n_zp)
                M_table[t.var.index] += t.coeff * eq_i;
        }
        for (const auto& t : constraints[i].b.terms()) {
            if (t.var.index < n_zp)
                M_table[t.var.index] += rho * t.coeff * eq_i;
        }
        for (const auto& t : constraints[i].c.terms()) {
            if (t.var.index < n_zp)
                M_table[t.var.index] += rho2 * t.coeff * eq_i;
        }
    }

    // z_table: copy z, zero-pad to n_zp
    std::vector<Scalar> z_table(z.begin(), z.end());
    z_table.resize(n_zp, Scalar::zero());

    // --- Step 8: Run inner sum-check (initial claim = vz_combined) ---
    // sc_prove folds MSB-first; reverse challenges for LSB-first MLE convention.
    std::vector<Scalar> inner_challenges;
    proof.inner_sc = sc_prove(M_table, z_table, transcript, ctx, inner_challenges);
    const std::vector<Scalar> ry(inner_challenges.rbegin(), inner_challenges.rend());

    // --- Step 9: Hyrax eval proof for the PRIVATE witness z_priv at ry ---
    // Must open the same vector that was committed in Step 1 (z_priv, public slots
    // zeroed). The verifier adds the public/io contribution to z̃(ry) separately.
    proof.eval_W = hyrax_eval_prove(z_priv, ry, H_z, gens, transcript, ctx);

    // --- Step 10: Hyrax eval proof for E at rx_m ---
    // Transcript is already bound to comm_E.
    std::vector<Scalar> E_padded(E.begin(), E.end());
    E_padded.resize(m, Scalar::zero());
    proof.eval_E = hyrax_eval_prove(E_padded, rx_m, H_E, gens, transcript, ctx);

    return proof;
}

// ---------------------------------------------------------------------------
// Spartan verifier
// ---------------------------------------------------------------------------

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
    bool bind_public_inputs
) {
    // --- Basic sanity checks ---
    if (proof.circuit_hash.size() != 32) return false;
    // The M̃ evaluation in the inner sum-check directly verifies the proof
    // against the verifier's R1CS matrices — that is the soundness binding.
    // The circuit hash is used for Fiat-Shamir (both sides use proof.circuit_hash),
    // so transcript consistency is guaranteed.  The expected_circuit_hash check
    // is an optional fast-rejection for version mismatches; skip it when the
    // caller passes an empty vector (saves the O(m) hash computation).
    if (!expected_circuit_hash.empty() && proof.circuit_hash != expected_circuit_hash) return false;

    const size_t n_zp = next_pow2(num_variables);
    const size_t m_p  = next_pow2(num_constraints);
    const size_t log_m = log2_exact(m_p);
    const size_t log_n = log2_exact(n_zp);

    if (proof.outer_sc.size() != log_m) return false;
    if (proof.inner_sc.size() != log_n) return false;

    const HyraxParams H_z = HyraxParams::from_n(num_variables);
    const HyraxParams H_E = HyraxParams::from_n(num_constraints);

    if (proof.comm_W.params.n_rows != H_z.n_rows ||
        proof.comm_W.params.n_cols != H_z.n_cols) return false;
    if (proof.comm_E.params.n_rows != H_E.n_rows ||
        proof.comm_E.params.n_cols != H_E.n_cols) return false;

    // --- Replay transcript: relaxation scalar, commitments, circuit hash ---
    transcript.append_scalar("spartan_u", u);
    for (const Point& p : proof.comm_W.C)
        transcript.append_point("hW", p, ctx);
    for (const Point& p : proof.comm_E.C)
        transcript.append_point("hE", p, ctx);
    transcript.append_scalar("chash", Scalar(proof.circuit_hash.data()));

    // --- Derive τ ---
    std::vector<Scalar> tau;
    tau.reserve(log_m);
    for (size_t k = 0; k < log_m; ++k)
        tau.push_back(transcript.challenge_scalar("tau", ctx));

    // --- Verify outer sum-check (degree-3 trilinear, initial claim = 0) ---
    std::vector<Scalar> outer_challenges;
    Scalar outer_final;
    if (!sc_verify_trilinear(proof.outer_sc, Scalar::zero(), transcript, ctx,
                              outer_challenges, outer_final)) return false;
    // sc_verify_trilinear returns challenges in MSB-first round order.
    // For mle_eq_vec (LSB-first) we reverse.
    const std::vector<Scalar> rx_m(outer_challenges.rbegin(), outer_challenges.rend());

    // Check outer sum-check final step:
    // outer_final == eq(τ, rx_m) * (Az_claim * Bz_claim - u*Cz_claim - Ez_claim)
    //
    // The prover's outer SC folds MSB-first: round k uses challenge outer_challenges[k]
    // to fold bit (log_m-1-k). eq_table was initialized as mle_eq_vec(tau) where tau[j]
    // is the coefficient for bit j (LSB ordering). So eq_table[0] after folding is:
    //   ∏_k eq(tau[k], rx_m[k])   where rx_m = reversed(outer_challenges) = LSB-first order
    //
    // Equivalently: ∏_k eq(tau[k], outer_challenges[log_m-1-k]).
    Scalar eq_tau_rx = Scalar::one();
    for (size_t k = 0; k < log_m; ++k) {
        eq_tau_rx *= tau[k] * rx_m[k]
                   + (Scalar::one() - tau[k]) * (Scalar::one() - rx_m[k]);
    }
    const Scalar abce_at_rx = proof.Az_claim * proof.Bz_claim
                            - u * proof.Cz_claim
                            - proof.Ez_claim;
    if (!(eq_tau_rx * abce_at_rx == outer_final)) return false;

    // --- Append claims, derive ρ ---
    transcript.append_scalar("Az", proof.Az_claim);
    transcript.append_scalar("Bz", proof.Bz_claim);
    transcript.append_scalar("Cz", proof.Cz_claim);
    transcript.append_scalar("Ez", proof.Ez_claim);
    const Scalar rho  = transcript.challenge_scalar("rho", ctx);
    const Scalar rho2 = rho * rho;

    // Initial inner claim
    const Scalar vz_combined = proof.Az_claim + rho * proof.Bz_claim + rho2 * proof.Cz_claim;

    // --- Verify inner sum-check ---
    std::vector<Scalar> inner_challenges_raw;
    Scalar inner_final;
    if (!sc_verify(proof.inner_sc, vz_combined, transcript, ctx,
                   inner_challenges_raw, inner_final)) return false;
    // Reverse to LSB-first for MLE evaluations (sc_verify returns MSB-first).
    const std::vector<Scalar> ry(inner_challenges_raw.rbegin(), inner_challenges_raw.rend());

    // Check inner sum-check final step:
    // inner_final should equal M̃_combined(rx_m, ry) * z̃(ry)
    // Verifier computes M̃_combined(rx_m, ry) from the sparse R1CS.
    const std::vector<Scalar> eq_rx_m = mle_eq_vec(rx_m);  // size m_p
    const std::vector<Scalar> eq_ry   = mle_eq_vec(ry);    // size n_zp

    const auto& constraints = verifier_cs.constraints();
    const size_t nc_check = std::min(num_constraints, constraints.size());

    // Two-phase M̃_combined(rx_m, ry) evaluation.
    //
    // Phase 1 — accumulate column sums scaled by eq_rx:
    //   col_sum[j] += (A_{ij} + ρ*B_{ij} + ρ²*C_{ij}) * eq_rx[i]
    // Phase 2 — dot product with eq_ry (fully sequential):
    //   M_eval = Σ_j col_sum[j] * eq_ry[j]
    //
    // Within Phase 1, precompute (ρ * eq_rx[i]) and (ρ² * eq_rx[i]) once per
    // constraint row (2 muls), then use the precomputed values for every B and C
    // nonzero in that row (1 mul each instead of 2). Saves ~1 mul per B entry
    // and ~1 mul per C entry → ~840K fewer field muls for the 211K-constraint
    // taproot HMB circuit.
    std::vector<Scalar> col_sum(n_zp, Scalar::zero());
    for (size_t i = 0; i < nc_check; ++i) {
        const Scalar eq_i      = eq_rx_m[i];
        const Scalar rho_eq_i  = rho  * eq_i;   // 1 mul, amortized over all B entries in row i
        const Scalar rho2_eq_i = rho2 * eq_i;   // 1 mul, amortized over all C entries in row i
        for (const auto& t : constraints[i].a.terms()) {
            if (t.var.index < n_zp)
                col_sum[t.var.index] += t.coeff * eq_i;
        }
        for (const auto& t : constraints[i].b.terms()) {
            if (t.var.index < n_zp)
                col_sum[t.var.index] += t.coeff * rho_eq_i;   // was: (rho * t.coeff) * eq_i
        }
        for (const auto& t : constraints[i].c.terms()) {
            if (t.var.index < n_zp)
                col_sum[t.var.index] += t.coeff * rho2_eq_i;  // was: (rho2 * t.coeff) * eq_i
        }
    }
    Scalar M_eval = Scalar::zero();
    for (size_t j = 0; j < n_zp; ++j) {
        M_eval += col_sum[j] * eq_ry[j];
    }

    // SECURITY (CONFIRMED-CRIT-05 fix, 2026-05-30) — bind the public inputs.
    // The prover committed/opened ONLY the private witness (public slots zeroed), so
    // eval_W.claimed = W̃(ry). The verifier reconstructs the public/io contribution to
    // z̃(ry) itself from its KNOWN public inputs — the constant ONE (zpub[0]==1) plus
    // verifier_cs's public-input slots (1..num_inputs), which BuildSpend/OutputCircuit
    // populated from the on-chain `pub`. z̃(ry) = io_eval(ry) + W̃(ry). This is what ties
    // the proof to the presented public inputs; a proof committing different io values
    // (a forgery) no longer satisfies inner_final == M_eval · z̃(ry).
    Scalar z_at_ry = proof.eval_W.claimed;
    if (bind_public_inputs) {
        const std::vector<Scalar>& zpub = verifier_cs.witness();
        const size_t num_pub = 1 + verifier_cs.num_inputs();   // constant ONE + public inputs
        Scalar io_eval = Scalar::zero();
        for (size_t j = 0; j < num_pub && j < zpub.size() && j < n_zp; ++j) {
            io_eval += eq_ry[j] * zpub[j];
        }
        z_at_ry = io_eval + proof.eval_W.claimed;
    }
    // bind_public_inputs=false → z_at_ry = eval_W.claimed (pre-fix unbound rule).

    // inner_final == M_eval * z̃(ry), with z̃(ry) = io_eval(ry) + W̃(ry) when bound
    if (!(M_eval * z_at_ry == inner_final)) return false;

    // --- Verify Hyrax eval proofs ---
    // eval_W: z̃(ry)
    if (!hyrax_eval_verify(proof.comm_W, ry, proof.eval_W, H_z, gens, transcript, ctx)) return false;

    // eval_E: Ẽ(rx_m), and the claimed value must match Ez_claim
    if (!hyrax_eval_verify(proof.comm_E, rx_m, proof.eval_E, H_E, gens, transcript, ctx)) return false;
    if (!(proof.eval_E.claimed == proof.Ez_claim)) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> SpartanProof::serialize(secp256k1_context* ctx) const {
    std::vector<uint8_t> out;

    // Helper: append a scalar
    auto push_scalar = [&](const Scalar& s) {
        out.insert(out.end(), s.data(), s.data() + 32);
    };
    // Helper: append u64 big-endian
    auto push64 = [&](uint64_t v) {
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>((v >> (8*i)) & 0xff));
    };

    // comm_W and comm_E
    auto wbytes = comm_W.serialize(ctx);
    out.insert(out.end(), wbytes.begin(), wbytes.end());
    auto ebytes = comm_E.serialize(ctx);
    out.insert(out.end(), ebytes.begin(), ebytes.end());

    // circuit_hash
    out.insert(out.end(), circuit_hash.begin(), circuit_hash.end());

    // outer_sc (degree-3: 4 scalars per round)
    push64(static_cast<uint64_t>(outer_sc.size()));
    for (const auto& msg : outer_sc) {
        push_scalar(msg[0]); push_scalar(msg[1]);
        push_scalar(msg[2]); push_scalar(msg[3]);
    }

    // Az, Bz, Cz, Ez
    push_scalar(Az_claim); push_scalar(Bz_claim);
    push_scalar(Cz_claim); push_scalar(Ez_claim);

    // inner_sc
    push64(static_cast<uint64_t>(inner_sc.size()));
    for (const auto& [h0, h1, h2] : inner_sc) {
        push_scalar(h0); push_scalar(h1); push_scalar(h2);
    }

    // eval_W, eval_E
    auto ew = eval_W.serialize(ctx);
    out.insert(out.end(), ew.begin(), ew.end());
    auto ee = eval_E.serialize(ctx);
    out.insert(out.end(), ee.begin(), ee.end());

    return out;
}

bool SpartanProof::deserialize(const std::vector<uint8_t>& data,
                                SpartanProof& out, secp256k1_context* ctx) {
    size_t offset = 0;

    // comm_W, comm_E
    if (!HyraxCommitment::deserialize(data, offset, out.comm_W, ctx)) return false;
    if (!HyraxCommitment::deserialize(data, offset, out.comm_E, ctx)) return false;

    // circuit_hash
    if (data.size() < offset + 32) return false;
    out.circuit_hash.assign(data.begin() + offset, data.begin() + offset + 32);
    offset += 32;

    // outer_sc (degree-3: 4 scalars per round = 128 bytes per round)
    if (data.size() < offset + 8) return false;
    uint64_t nOuter = 0;
    for (int i = 0; i < 8; ++i) nOuter = (nOuter << 8) | data[offset++];
    if (data.size() < offset + nOuter * 128) return false;
    out.outer_sc.resize(nOuter);
    for (uint64_t k = 0; k < nOuter; ++k) {
        out.outer_sc[k][0] = Scalar(data.data() + offset); offset += 32;
        out.outer_sc[k][1] = Scalar(data.data() + offset); offset += 32;
        out.outer_sc[k][2] = Scalar(data.data() + offset); offset += 32;
        out.outer_sc[k][3] = Scalar(data.data() + offset); offset += 32;
    }

    // Az, Bz, Cz, Ez
    if (data.size() < offset + 128) return false;
    out.Az_claim = Scalar(data.data() + offset); offset += 32;
    out.Bz_claim = Scalar(data.data() + offset); offset += 32;
    out.Cz_claim = Scalar(data.data() + offset); offset += 32;
    out.Ez_claim = Scalar(data.data() + offset); offset += 32;

    // inner_sc
    if (data.size() < offset + 8) return false;
    uint64_t nInner = 0;
    for (int i = 0; i < 8; ++i) nInner = (nInner << 8) | data[offset++];
    if (data.size() < offset + nInner * 96) return false;
    out.inner_sc.resize(nInner);
    for (uint64_t k = 0; k < nInner; ++k) {
        out.inner_sc[k][0] = Scalar(data.data() + offset); offset += 32;
        out.inner_sc[k][1] = Scalar(data.data() + offset); offset += 32;
        out.inner_sc[k][2] = Scalar(data.data() + offset); offset += 32;
    }

    // eval_W, eval_E
    if (!HyraxEvalProof::deserialize(data, offset, out.eval_W, ctx)) return false;
    if (!HyraxEvalProof::deserialize(data, offset, out.eval_E, ctx)) return false;

    return true;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
