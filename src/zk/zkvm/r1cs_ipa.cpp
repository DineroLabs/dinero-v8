// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/r1cs_ipa.h"
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <thread>

namespace dinero {
namespace zk {
namespace zkvm {

namespace {

bool HiddenMemberBindingProfileEnabled() {
    static const bool enabled = []() {
        const char* env = std::getenv("DINERO_HMB_PROFILE");
        return env != nullptr && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0');
    }();
    return enabled;
}

void HiddenMemberBindingProfileLog(const char* fmt, ...) {
    if (!HiddenMemberBindingProfileEnabled()) {
        return;
    }
    std::fprintf(stderr, "[HMB_PROFILE] ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

} // namespace

// ---------------------------------------------------------------------------
// Circuit identity hash (P1 Audit Fix #3)
//
// Deterministically hashes the R1CS structure (A, B, C matrices) so the
// verifier can confirm the IPA proof was generated against the intended
// constraint system. Without this, a malicious prover can craft IPA
// vectors with <a,b>=0 that don't correspond to any real R1CS.
// ---------------------------------------------------------------------------

std::vector<uint8_t> hash_r1cs_structure(const R1CS& cs) {
    uint8_t hash[32];
    SHA256_CTX h;
    SHA256_Init(&h);

    uint64_t nc = cs.num_constraints();
    uint64_t nv = cs.num_variables();
    SHA256_Update(&h, &nc, sizeof(nc));
    SHA256_Update(&h, &nv, sizeof(nv));

    for (const auto& c : cs.constraints()) {
        // Hash A terms
        uint64_t na = c.a.terms().size();
        SHA256_Update(&h, &na, sizeof(na));
        for (const auto& t : c.a.terms()) {
            SHA256_Update(&h, &t.var.index, sizeof(t.var.index));
            SHA256_Update(&h, t.coeff.data(), 32);
        }
        // Hash B terms
        uint64_t nb = c.b.terms().size();
        SHA256_Update(&h, &nb, sizeof(nb));
        for (const auto& t : c.b.terms()) {
            SHA256_Update(&h, &t.var.index, sizeof(t.var.index));
            SHA256_Update(&h, t.coeff.data(), 32);
        }
        // Hash C terms
        uint64_t nct = c.c.terms().size();
        SHA256_Update(&h, &nct, sizeof(nct));
        for (const auto& t : c.c.terms()) {
            SHA256_Update(&h, &t.var.index, sizeof(t.var.index));
            SHA256_Update(&h, t.coeff.data(), 32);
        }
    }

    SHA256_Final(hash, &h);
    return std::vector<uint8_t>(hash, hash + 32);
}

// ---------------------------------------------------------------------------
// Nova Decider Proof — Extended-Vector Relaxed R1CS IPA
//
// Proves that the FOLDED relaxed R1CS instance is satisfiable:
//   ∀i: (A_i·z)(B_i·z) - u*(C_i·z) - E_i = 0   where z = W (full witness)
//
// Using the extended-vector IPA encoding:
//   n = next_pow2(num_variables)  — witness dimension
//   m = num_constraints           — error vector dimension
//   N = next_pow2(n + m)          — IPA dimension
//
//   l[k] = Σ_i α^i · B[i][k] · (A_i·W) − u · Σ_i α^i · C[i][k]   for k in [0, n)
//   l[n+i] = α^i                                                      for i in [0, m)
//   l[k] = 0                                                          for k in [n+m, N)
//
//   r[k] = W[k]                    for k in [0, n)
//   r[n+i] = -E[i]                 for i in [0, m)
//   r[k] = 0                       for k in [n+m, N)
//
//   <l, r> = Σ_i α^i [(A_i·W)(B_i·W) − u*(C_i·W)] + Σ_i α^i · (−E_i)
//          = Σ_i α^i [(A_i·W)(B_i·W) − u*(C_i·W) − E_i] = 0
//
// Combined Schnorr opening proof for target = commit_W - commit_E:
//   <r, H[0..N-1]> + (r_W - r_E) * Q = commit_W - commit_E
// ---------------------------------------------------------------------------

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
) {
    using Clock = std::chrono::steady_clock;
    const auto prove_start = Clock::now();
    R1CSIPAProof result;
    size_t m = cs.num_constraints();
    assert(m > 0);

    // Witness dimension (padded to power of 2)
    size_t n = next_pow2(num_variables > 0 ? num_variables : 1);
    // Extended IPA dimension: witness + error + padding
    size_t N = next_pow2(n + m);
    assert(gens.size() >= N);
    HiddenMemberBindingProfileLog(
        "r1cs_ipa dimensions m=%zu n=%zu N=%zu num_variables=%zu",
        m, n, N, num_variables);

    const auto& constraints = cs.constraints();

    // --- Step 1: Hash-commit to extended witness ρ = (W || E) ---
    //
    // Binds the prover to specific W and E BEFORE seeing α.
    uint8_t nonce[32];
    RAND_bytes(nonce, 32);

    // Generator identity hash (verifier can reproduce from public generators)
    uint8_t gen_hash[32];
    {
        SHA256_CTX gh;
        SHA256_Init(&gh);
        size_t gen_count = std::min(N, gens.size());
        for (size_t k = 0; k < gen_count; ++k) {
            Point::Compressed gc, hc;
            gens.G()[k].serialize(gc, ctx);
            gens.H()[k].serialize(hc, ctx);
            SHA256_Update(&gh, gc.data(), 33);
            SHA256_Update(&gh, hc.data(), 33);
        }
        SHA256_Final(gen_hash, &gh);
    }

    // Hash the extended witness: SHA256(W || E || gen_hash || nonce)
    uint8_t witness_hash[32];
    {
        SHA256_CTX h;
        SHA256_Init(&h);
        for (size_t k = 0; k < W.size(); ++k) {
            SHA256_Update(&h, W[k].data(), 32);
        }
        for (size_t k = 0; k < E.size(); ++k) {
            SHA256_Update(&h, E[k].data(), 32);
        }
        SHA256_Update(&h, gen_hash, 32);
        SHA256_Update(&h, nonce, 32);
        SHA256_Final(witness_hash, &h);
    }

    result.witness_nonce.assign(nonce, nonce + 32);
    result.witness_hash.assign(witness_hash, witness_hash + 32);
    result.gen_hash.assign(gen_hash, gen_hash + 32);

    // --- Step 2: Circuit identity hash ---
    result.circuit_hash = hash_r1cs_structure(cs);

    // --- Step 3: Append binding data to transcript ---
    transcript.append_scalar("witness_hash", Scalar(witness_hash));
    transcript.append_scalar("circuit_hash", Scalar(result.circuit_hash.data()));
    transcript.append_scalar("gen_hash", Scalar(gen_hash));
    transcript.append_u64("num_constraints", static_cast<uint64_t>(m));
    transcript.append_u64("num_variables", static_cast<uint64_t>(num_variables));

    // Bind IPA to the Nova committed instance
    transcript.append_scalar("nova_u", u);
    transcript.append_point("nova_commit_W", commit_W, ctx);
    transcript.append_point("nova_commit_E", commit_E, ctx);

    // --- Step 4: Challenge α ---
    Scalar alpha = transcript.challenge_scalar("alpha", ctx);

    // --- Step 5: Build extended IPA vectors l and r ---
    const auto vector_start = Clock::now();
    //
    // Evaluate A_i·W for each constraint
    std::vector<Scalar> a_dot_W(m);
    for (size_t i = 0; i < m; ++i) {
        a_dot_W[i] = constraints[i].a.evaluate(W);
    }

    // Build l[0..n-1]: the witness part
    //   l[k] = Σ_i α^i · (A_i·W) · B[i][k]  −  u · Σ_i α^i · C[i][k]
    std::vector<Scalar> l(N, Scalar::zero());
    std::vector<Scalar> c_vec(n, Scalar::zero());  // u-scaled C contribution

    Scalar alpha_power = Scalar::one();
    for (size_t i = 0; i < m; ++i) {
        Scalar alpha_i_times_a_dot_W = alpha_power * a_dot_W[i];

        // B[i][k] contributions to l[k]
        for (const auto& term : constraints[i].b.terms()) {
            if (term.var.index < n && !term.coeff.is_zero()) {
                l[term.var.index] = l[term.var.index] + (alpha_i_times_a_dot_W * term.coeff);
            }
        }

        // C[i][k] contributions (will be subtracted with u scaling)
        for (const auto& term : constraints[i].c.terms()) {
            if (term.var.index < n && !term.coeff.is_zero()) {
                c_vec[term.var.index] = c_vec[term.var.index] + (alpha_power * term.coeff);
            }
        }

        alpha_power = alpha_power * alpha;
    }

    // l[k] -= u * c_vec[k] for the witness part
    for (size_t k = 0; k < n; ++k) {
        l[k] = l[k] - (u * c_vec[k]);
    }

    // Build l[n..n+m-1]: the error part — coefficients for -E_i terms
    //   l[n+i] = α^i
    alpha_power = Scalar::one();
    for (size_t i = 0; i < m; ++i) {
        l[n + i] = alpha_power;
        alpha_power = alpha_power * alpha;
    }
    // l[n+m..N-1] = 0 (already initialized)

    // Build r[0..N-1]: the extended right vector (W || -E || 0)
    std::vector<Scalar> r(N, Scalar::zero());
    for (size_t k = 0; k < W.size() && k < n; ++k) {
        r[k] = W[k];
    }
    for (size_t i = 0; i < E.size() && i < m; ++i) {
        r[n + i] = -E[i];  // Negated error
    }
    // r[n+m..N-1] = 0 (already initialized)
    const auto vector_end = Clock::now();

    size_t nonzero_l = 0;
    size_t nonzero_r = 0;
    for (const auto& v : l) {
        if (!v.is_zero()) {
            ++nonzero_l;
        }
    }
    for (const auto& v : r) {
        if (!v.is_zero()) {
            ++nonzero_r;
        }
    }
    HiddenMemberBindingProfileLog(
        "r1cs_ipa vectors build_ms=%.2f nonzero_l=%zu/%zu nonzero_r=%zu/%zu",
        std::chrono::duration<double, std::milli>(vector_end - vector_start).count(),
        nonzero_l,
        l.size(),
        nonzero_r,
        r.size());

    // --- Step 6: Inner product (should be zero for satisfied relaxed R1CS) ---
    result.inner_product = inner_product(l, r);

    // --- Step 7: Wire commitment and IPA proof ---
    const auto wire_commit_start = Clock::now();
    result.wire_commitment = pedersen_vector_commit(l, r, gens, ctx);
    const auto wire_commit_end = Clock::now();
    transcript.append_point("V_ipa", result.wire_commitment, ctx);

    result.commitment = result.wire_commitment;
    const auto ipa_start = Clock::now();
    result.ipa = ipa_prove(l, r, gens, transcript, ctx);
    const auto ipa_end = Clock::now();

    // --- Step 8: Store commitment binding scalar ---
    //
    // r_combined = r_W - r_E. The verifier uses this to derive P:
    //   P = <l, G> + (commit_W - commit_E) - r_combined * Q
    // The IPA verify against this derived P proves commitment binding.
    result.r_combined = r_W - r_E;

    // Generate logarithmic Pedersen opening proof
    // Proves: <r, H[0..N-1]> + r_combined * Q = commit_W - commit_E
    Point target = commit_W + (commit_E * (-Scalar::one()));
    const auto opening_start = Clock::now();
    result.opening_proof = pedersen_opening_prove(r, result.r_combined, target, N, gens, ctx);
    const auto opening_end = Clock::now();

    HiddenMemberBindingProfileLog(
        "r1cs_ipa commit_ms=%.2f ipa_ms=%.2f opening_ms=%.2f total_ms=%.2f",
        std::chrono::duration<double, std::milli>(wire_commit_end - wire_commit_start).count(),
        std::chrono::duration<double, std::milli>(ipa_end - ipa_start).count(),
        std::chrono::duration<double, std::milli>(opening_end - opening_start).count(),
        std::chrono::duration<double, std::milli>(opening_end - prove_start).count());

    return result;
}

// ---------------------------------------------------------------------------
// Pedersen vector commitment opening proof (logarithmic)
// ---------------------------------------------------------------------------

R1CSIPAProof::PedersenOpeningProof pedersen_opening_prove(
    const std::vector<Scalar>& r,
    const Scalar& blind,
    const Point& target,
    size_t N,
    const GeneratorSet& gens,
    secp256k1_context* ctx
) {
    R1CSIPAProof::PedersenOpeningProof proof;

    // Prove: <r, H[0..N-1]> + blind * Q = target
    //
    // Folding protocol (log N rounds):
    //   Split r = (r_L || r_R) and H = (H_L || H_R) at midpoint
    //   L = <r_L, H_R> + l_blind * Q
    //   R = <r_R, H_L> + r_blind_part * Q
    //   Challenge x = Hash(target, L, R)
    //   Fold: r' = x^{-1} * r_L + x * r_R
    //         H' = x * H_L + x^{-1} * H_R
    //         target' = x^{-2} * L + target + x^2 * R
    //         blind' = x^{-2} * l_blind + blind + x^2 * r_blind_part
    //
    // This matches the verifier's folded-generator weights:
    //   H_final = sum_i s_inv[i] * H[i]
    // where each left branch contributes x and each right branch x^{-1}.

    size_t n = N;
    std::vector<Scalar> r_cur = r;
    r_cur.resize(N, Scalar::zero());
    Scalar blind_cur = blind;
    std::vector<Point> H_cur;
    H_cur.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        H_cur.push_back(gens.H()[i]);
    }

    Transcript ts("pedersen_opening");
    ts.append_point("target", target, ctx);

    while (n > 1) {
        size_t half = n / 2;

        // L = <r_L, H_R> + l_blind * Q
        Scalar l_blind = Scalar::random(ctx);
        {
            std::vector<Scalar> scalars;
            std::vector<Point> points;
            for (size_t i = 0; i < half; ++i) {
                if (!r_cur[i].is_zero()) {
                    scalars.push_back(r_cur[i]);
                    points.push_back(H_cur[half + i]); // r_L against current H_R
                }
            }
            if (!l_blind.is_zero()) {
                scalars.push_back(l_blind);
                points.push_back(gens.Q());
            }
            Point L = scalars.empty() ? Point::identity()
                      : Point::multi_scalar_mul(scalars, points, ctx);
            proof.L.push_back(L);
        }

        // R = <r_R, H_L> + r_blind * Q
        Scalar r_blind_part = Scalar::random(ctx);
        {
            std::vector<Scalar> scalars;
            std::vector<Point> points;
            for (size_t i = 0; i < half; ++i) {
                if (!r_cur[half + i].is_zero()) {
                    scalars.push_back(r_cur[half + i]);
                    points.push_back(H_cur[i]); // r_R against current H_L
                }
            }
            if (!r_blind_part.is_zero()) {
                scalars.push_back(r_blind_part);
                points.push_back(gens.Q());
            }
            Point R = scalars.empty() ? Point::identity()
                      : Point::multi_scalar_mul(scalars, points, ctx);
            proof.R.push_back(R);
        }

        // Challenge
        ts.append_point("L", proof.L.back(), ctx);
        ts.append_point("R", proof.R.back(), ctx);
        Scalar x = ts.challenge_scalar("x", ctx);
        Scalar x_inv = x.inverse(ctx);

        // Fold r: r' = x^{-1} * r_L + x * r_R
        std::vector<Scalar> r_new(half);
        for (size_t i = 0; i < half; ++i) {
            r_new[i] = (x_inv * r_cur[i]) + (x * r_cur[half + i]);
        }
        r_cur = r_new;

        // Fold H explicitly (parallel): H' = x * H_L + x^{-1} * H_R
        // Write to H_new[i] (left half) while reading H_cur[i] and H_cur[half+i].
        // Safe: each thread writes distinct indices in H_new (no data races).
        {
            static const size_t hw = std::thread::hardware_concurrency();
            const size_t nthreads = (half >= 256 && hw > 1)
                ? std::min(hw, half / 64) : 1;
            std::vector<Point> H_new(half, Point::identity());
            if (nthreads <= 1) {
                for (size_t i = 0; i < half; ++i) {
                    H_new[i] = (H_cur[i] * x) + (H_cur[half + i] * x_inv);
                }
            } else {
                const size_t chunk = (half + nthreads - 1) / nthreads;
                std::vector<std::thread> threads;
                threads.reserve(nthreads);
                for (size_t t = 0; t < nthreads; ++t) {
                    const size_t begin = t * chunk;
                    const size_t end = std::min(begin + chunk, half);
                    if (begin >= half) break;
                    threads.emplace_back([&H_cur, &H_new, begin, end, half, x, x_inv]() {
                        for (size_t i = begin; i < end; ++i) {
                            H_new[i] = (H_cur[i] * x) + (H_cur[half + i] * x_inv);
                        }
                    });
                }
                for (auto& thr : threads) thr.join();
            }
            H_cur = std::move(H_new);
        }

        // Fold blind: blind' = x^{-2} * l_blind + blind_cur + x^2 * r_blind_part
        Scalar x2 = x * x;
        Scalar x_inv2 = x_inv * x_inv;
        blind_cur = (x_inv2 * l_blind) + blind_cur + (x2 * r_blind_part);

        n = half;
    }

    // Final: r_cur[0] and blind_cur
    proof.a_final = r_cur[0];
    proof.blind_final = blind_cur;

    return proof;
}

bool pedersen_opening_verify(
    const R1CSIPAProof::PedersenOpeningProof& proof,
    const Point& target,
    const Scalar& r_combined,
    size_t N,
    const GeneratorSet& gens,
    secp256k1_context* ctx
) {
    size_t rounds = proof.L.size();
    if (proof.R.size() != rounds) return false;
    if ((1u << rounds) != N) return false;

    // Replay transcript to get challenges
    Transcript ts("pedersen_opening");
    ts.append_point("target", target, ctx);

    std::vector<Scalar> challenges(rounds);
    for (size_t i = 0; i < rounds; ++i) {
        ts.append_point("L", proof.L[i], ctx);
        ts.append_point("R", proof.R[i], ctx);
        challenges[i] = ts.challenge_scalar("x", ctx);
    }

    // Precompute challenge inverses and squares once.
    // The previous implementation recomputed x^{-1} inside the N*rounds
    // fold-weight loop below, which is catastrophically expensive at the
    // current proof sizes.
    std::vector<Scalar> inv_challenges(rounds);
    std::vector<Scalar> challenges_sq(rounds);
    std::vector<Scalar> inv_challenges_sq(rounds);
    for (size_t i = 0; i < rounds; ++i) {
        inv_challenges[i] = challenges[i].inverse(ctx);
        challenges_sq[i] = challenges[i] * challenges[i];
        inv_challenges_sq[i] = inv_challenges[i] * inv_challenges[i];
    }

    // Fold target: target_folded = target + sum(x_i^{-2} * L_i + x_i^2 * R_i)
    Point target_folded = target;
    for (size_t i = 0; i < rounds; ++i) {
        target_folded =
            target_folded +
            (proof.L[i] * inv_challenges_sq[i]) +
            (proof.R[i] * challenges_sq[i]);
    }

    // Compute folded generator H_final = sum(s_inv[i] * H[i]) for i=0..N-1
    // where s_inv[i] = product_j (x_j^{-1} if bit j set, x_j if not)
    // This is the same computation as in IPA verify.
    //
    // Parallelised: each element is independent (no inter-element dependency).
    // Combined s_inv computation and scalar-point build in one parallel pass
    // to halve memory traffic.
    const size_t n_pts = std::min(N, gens.H().size());
    std::vector<Scalar> scalars(n_pts + 1);
    std::vector<Point>  points(n_pts + 1, Point::identity());
    {
        const size_t hw = std::thread::hardware_concurrency();
        const size_t nthreads = (n_pts >= 512 && hw > 1)
            ? std::min(hw, (n_pts + 63) / 64) : 1;
        const size_t chunk = (n_pts + nthreads - 1) / nthreads;
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (size_t t = 0; t < nthreads; ++t) {
            const size_t begin = t * chunk;
            const size_t end = std::min(begin + chunk, n_pts);
            if (begin >= n_pts) break;
            threads.emplace_back([&challenges, &inv_challenges, &proof, &gens,
                                   &scalars, &points, begin, end, rounds]() {
                const auto& H = gens.H();
                for (size_t i = begin; i < end; ++i) {
                    Scalar si = Scalar::one();
                    for (size_t j = 0; j < rounds; ++j) {
                        size_t bit = (i >> (rounds - 1 - j)) & 1;
                        si = si * (bit ? inv_challenges[j] : challenges[j]);
                    }
                    scalars[i] = proof.a_final * si;
                    points[i]  = H[i];
                }
            });
        }
        for (auto& thr : threads) thr.join();
    }
    scalars[n_pts] = proof.blind_final;
    points[n_pts]  = gens.Q();

    // H_final = sum(s_inv[i] * H[i])
    // Verify: a_final * H_final + blind_final * Q == target_folded
    Point lhs = Point::multi_scalar_mul(scalars, points, ctx);

    Point::Compressed lhs_ser, rhs_ser;
    if (!lhs.serialize(lhs_ser, ctx)) return false;
    if (!target_folded.serialize(rhs_ser, ctx)) return false;
    return lhs_ser == rhs_ser;
}

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
) {
    using Clock = std::chrono::steady_clock;
    const auto verify_start = Clock::now();
    // --- Replay transcript ---
    if (proof.witness_hash.size() != 32) return false;
    // witness_nonce no longer serialized (P2 malleability fix) — skip size check

    size_t n = next_pow2(num_variables > 0 ? num_variables : 1);
    size_t N = next_pow2(n + num_constraints);

    // Verify generator identity hash
    if (proof.gen_hash.size() != 32) return false;
    {
        size_t gen_count = std::min(N, gens.size());
        uint8_t expected_gen_hash[32];
        SHA256_CTX gh;
        SHA256_Init(&gh);
        for (size_t k = 0; k < gen_count; ++k) {
            Point::Compressed gc, hc;
            gens.G()[k].serialize(gc, ctx);
            gens.H()[k].serialize(hc, ctx);
            SHA256_Update(&gh, gc.data(), 33);
            SHA256_Update(&gh, hc.data(), 33);
        }
        SHA256_Final(expected_gen_hash, &gh);
        if (std::memcmp(proof.gen_hash.data(), expected_gen_hash, 32) != 0) {
            return false;
        }
    }
    const auto preproof_end = Clock::now();

    // Verify circuit identity hash
    if (proof.circuit_hash.size() != 32) return false;
    if (proof.circuit_hash != expected_circuit_hash) return false;

    // Replay transcript entries (must match prover exactly)
    transcript.append_scalar("witness_hash", Scalar(proof.witness_hash.data()));
    transcript.append_scalar("circuit_hash", Scalar(proof.circuit_hash.data()));
    transcript.append_scalar("gen_hash", Scalar(proof.gen_hash.data()));
    transcript.append_u64("num_constraints", static_cast<uint64_t>(num_constraints));
    transcript.append_u64("num_variables", static_cast<uint64_t>(num_variables));

    // Bind to Nova committed instance
    transcript.append_scalar("nova_u", u);
    transcript.append_point("nova_commit_W", commit_W, ctx);
    transcript.append_point("nova_commit_E", commit_E, ctx);

    // Consume alpha challenge (keep transcript in sync)
    transcript.challenge_scalar("alpha", ctx);

    // Append wire/IPA commitment
    transcript.append_point("V_ipa", proof.wire_commitment, ctx);

    // --- Check inner product is zero ---
    if (!proof.inner_product.is_zero()) {
        return false;
    }

    // --- Verify Pedersen vector opening proof ---
    //
    // Proves that the IPA right vector r = (W || -E || 0) opens the
    // target commitment: <r, H> + r_combined*Q = commit_W - commit_E.
    //
    // Uses a logarithmic folding argument (IPA-style) instead of the
    // previous O(N) Schnorr response. At each round, the prover commits
    // to left/right halves, gets a challenge, and folds. After log(N)
    // rounds, a single scalar + blinding verifies the opening.
    //
    // Proof size: O(log N) group elements + 2 scalars ≈ 1KB.
    {
        Point target = commit_W + (commit_E * (-Scalar::one()));

        const auto opening_start = Clock::now();
        if (!pedersen_opening_verify(
                proof.opening_proof, target, proof.r_combined,
                N, gens, ctx)) {
            return false;
        }
        const auto opening_end = Clock::now();
        HiddenMemberBindingProfileLog(
            "r1cs_ipa verify preproof_ms=%.2f opening_ms=%.2f",
            std::chrono::duration<double, std::milli>(preproof_end - verify_start).count(),
            std::chrono::duration<double, std::milli>(opening_end - opening_start).count());
    }

    // --- Verify IPA proof ---
    const auto ipa_start = Clock::now();
    const bool ok = ipa_verify(
        proof.commitment,
        proof.inner_product,
        proof.ipa,
        gens,
        transcript,
        ctx
    );
    const auto ipa_end = Clock::now();
    HiddenMemberBindingProfileLog(
        "r1cs_ipa verify ipa_ms=%.2f total_ms=%.2f",
        std::chrono::duration<double, std::milli>(ipa_end - ipa_start).count(),
        std::chrono::duration<double, std::milli>(ipa_end - verify_start).count());
    return ok;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
