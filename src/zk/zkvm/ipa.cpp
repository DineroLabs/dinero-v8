// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/ipa.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <openssl/sha.h>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// Generator set creation (NUMS — Nothing Up My Sleeve)
// ---------------------------------------------------------------------------

namespace {

// Deterministic hash-to-curve for generator creation.
// Uses try-and-increment: SHA256(tag || index || counter) -> candidate x.
Point hash_to_generator(const char* tag, size_t index, secp256k1_context* ctx) {
    for (uint32_t ctr = 0; ctr < 256; ++ctr) {
        uint8_t hash[32];
        SHA256_CTX h;
        SHA256_Init(&h);
        SHA256_Update(&h, tag, std::strlen(tag));
        uint8_t idx_bytes[8];
        for (int i = 0; i < 8; ++i) idx_bytes[i] = static_cast<uint8_t>(index >> (56 - 8*i));
        SHA256_Update(&h, idx_bytes, 8);
        uint8_t ctr_byte = static_cast<uint8_t>(ctr);
        SHA256_Update(&h, &ctr_byte, 1);
        SHA256_Final(hash, &h);

        // Try as compressed point with even y
        uint8_t candidate[33];
        candidate[0] = 0x02;
        std::memcpy(candidate + 1, hash, 32);

        Point p;
        if (Point::parse(candidate, 33, p, ctx)) {
            return p;
        }
    }
    // Should never happen
    assert(false && "hash_to_generator failed after 256 attempts");
    return Point();
}

} // anonymous namespace

GeneratorSet GeneratorSet::create(size_t n, secp256k1_context* ctx) {
    GeneratorSet gs;
    gs.Q_ = hash_to_generator("Dinero_IPA_Q", 0, ctx);

    // Parallel creation: each thread computes a disjoint range of generator indices.
    // hash_to_generator only reads from ctx (SHA256 uses stack-local state; pubkey_parse
    // uses const ctx) — safe for concurrent calls on the same context.
    // Thread-local vectors eliminate false sharing; final vectors are assembled
    // by concatenation rather than indexed write, preserving deterministic order.
    const size_t hw = std::thread::hardware_concurrency();
    const size_t nthreads = (n >= 256 && hw > 1) ? std::min(hw, n / 64) : 1;

    if (nthreads <= 1) {
        gs.G_.reserve(n);
        gs.H_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            gs.G_.push_back(hash_to_generator("Dinero_IPA_G", i, ctx));
            gs.H_.push_back(hash_to_generator("Dinero_IPA_H", i, ctx));
        }
        return gs;
    }

    // Each thread fills its own partial G and H vectors, then they are merged
    // in order. No shared mutable state between threads.
    const size_t chunk = (n + nthreads - 1) / nthreads;
    std::vector<std::vector<Point>> G_parts(nthreads);
    std::vector<std::vector<Point>> H_parts(nthreads);

    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    for (size_t t = 0; t < nthreads; ++t) {
        const size_t begin = t * chunk;
        const size_t end   = std::min(begin + chunk, n);
        if (begin >= n) break;
        G_parts[t].reserve(end - begin);
        H_parts[t].reserve(end - begin);
        threads.emplace_back([&G_parts, &H_parts, ctx, begin, end, t]() {
            for (size_t i = begin; i < end; ++i) {
                G_parts[t].push_back(hash_to_generator("Dinero_IPA_G", i, ctx));
                H_parts[t].push_back(hash_to_generator("Dinero_IPA_H", i, ctx));
            }
        });
    }
    for (auto& thr : threads) thr.join();

    // Assemble final vectors in order
    gs.G_.reserve(n);
    gs.H_.reserve(n);
    for (size_t t = 0; t < G_parts.size(); ++t) {
        for (auto& p : G_parts[t]) gs.G_.push_back(std::move(p));
        for (auto& p : H_parts[t]) gs.H_.push_back(std::move(p));
    }

    return gs;
}

// ---------------------------------------------------------------------------
// Generator cache — deterministic NUMS points never change, compute once
// ---------------------------------------------------------------------------

namespace {
    // Global cache: stores the largest generator set ever requested.
    // Smaller requests are served by taking a prefix.
    GeneratorSet g_cached_gens;
    size_t g_cached_size = 0;
} // anonymous namespace

const GeneratorSet& GeneratorSet::cached(size_t n, secp256k1_context* ctx) {
    if (n <= g_cached_size) {
        return g_cached_gens;
    }
    // Need to grow the cache. Create a new set of size n.
    g_cached_gens = GeneratorSet::create(n, ctx);
    g_cached_size = n;
    return g_cached_gens;
}

void GeneratorSet::clear_cache() {
    g_cached_gens = GeneratorSet();
    g_cached_size = 0;
}

// ---------------------------------------------------------------------------
// Inner product computation
// ---------------------------------------------------------------------------

Scalar inner_product(const std::vector<Scalar>& a, const std::vector<Scalar>& b) {
    assert(a.size() == b.size());
    Scalar result = Scalar::zero();
    for (size_t i = 0; i < a.size(); ++i) {
        result += a[i] * b[i];
    }
    return result;
}

// ---------------------------------------------------------------------------
// Pedersen vector commitment
// ---------------------------------------------------------------------------

Point pedersen_vector_commit(
    const std::vector<Scalar>& a,
    const std::vector<Scalar>& b,
    const GeneratorSet& gens,
    secp256k1_context* ctx
) {
    assert(a.size() == b.size());
    assert(a.size() <= gens.size());

    // C = <a, G> + <b, H> + <a,b> * Q
    // Build as multi-scalar multiplication for future optimization
    std::vector<Scalar> scalars;
    std::vector<Point> points;
    scalars.reserve(a.size() * 2 + 1);
    points.reserve(a.size() * 2 + 1);

    for (size_t i = 0; i < a.size(); ++i) {
        scalars.push_back(a[i]);
        points.push_back(gens.G()[i]);
    }
    for (size_t i = 0; i < b.size(); ++i) {
        scalars.push_back(b[i]);
        points.push_back(gens.H()[i]);
    }

    Scalar ip = inner_product(a, b);
    scalars.push_back(ip);
    points.push_back(gens.Q());

    return Point::multi_scalar_mul(scalars, points, ctx);
}

// ---------------------------------------------------------------------------
// IPA Prover — recursive halving protocol
// ---------------------------------------------------------------------------

IPAProof ipa_prove(
    const std::vector<Scalar>& a_in,
    const std::vector<Scalar>& b_in,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
) {
    assert(a_in.size() == b_in.size());
    assert((a_in.size() & (a_in.size() - 1)) == 0); // Must be power of 2

    size_t n = a_in.size();
    IPAProof proof;

    // Make mutable copies
    std::vector<Scalar> a = a_in;
    std::vector<Scalar> b = b_in;
    std::vector<Point> G_vec = gens.G();
    std::vector<Point> H_vec = gens.H();

    // Trim to size n
    G_vec.resize(n);
    H_vec.resize(n);

    // Append initial commitment to transcript
    Point P = pedersen_vector_commit(a, b, gens, ctx);
    transcript.append_point("P", P, ctx);

    // Recursive halving: log2(n) rounds.
    //
    // All operations use in-place indexing — no intermediate sub-vectors are
    // allocated.  For round 1 with N=4M this saves ~780MB of allocation+copy
    // overhead (4×2M scalars + 4×2M Points).
    //
    // In-place safety:
    //   • Scalar fold:    a[i] = a[i]*x + a[half+i]*x_inv  —  reads a[i] and
    //     a[half+i] before writing a[i].  a[half..n-1] is never written.
    //   • Generator fold: G_vec[i] = G_vec[i]*x_inv + G_vec[half+i]*x
    //     Same pattern; safe for parallel execution because each thread writes
    //     G_vec[begin..end-1] ⊂ [0,half) and reads G_vec[half+begin..half+end-1]
    //     ⊂ [half,n), which no thread writes.
    while (n > 1) {
        size_t half = n / 2;

        // Cross inner products (index directly, no copies)
        Scalar ip_lr = Scalar::zero();
        Scalar ip_rl = Scalar::zero();
        for (size_t i = 0; i < half; ++i) {
            ip_lr += a[i] * b[half + i];
            ip_rl += a[half + i] * b[i];
        }

        // L = <a[0..half), G_vec[half..n)> + <b[half..n), H_vec[0..half)> + ip_lr*Q
        // R = <a[half..n), G_vec[0..half)> + <b[0..half), H_vec[half..n)> + ip_rl*Q
        // (both read-only from the current vectors — computed before any fold)
        std::vector<Scalar> L_scalars, R_scalars;
        std::vector<Point>  L_points,  R_points;
        L_scalars.reserve(2 * half + 1);  L_points.reserve(2 * half + 1);
        R_scalars.reserve(2 * half + 1);  R_points.reserve(2 * half + 1);
        for (size_t i = 0; i < half; ++i) {
            L_scalars.push_back(a[i]);        L_points.push_back(G_vec[half + i]);
            L_scalars.push_back(b[half + i]); L_points.push_back(H_vec[i]);
            R_scalars.push_back(a[half + i]); R_points.push_back(G_vec[i]);
            R_scalars.push_back(b[i]);        R_points.push_back(H_vec[half + i]);
        }
        L_scalars.push_back(ip_lr); L_points.push_back(gens.Q());
        R_scalars.push_back(ip_rl); R_points.push_back(gens.Q());

        Point L = Point::multi_scalar_mul(L_scalars, L_points, ctx);
        Point R = Point::multi_scalar_mul(R_scalars, R_points, ctx);

        // Record L, R
        proof.L.push_back(L);
        proof.R.push_back(R);

        // Fiat-Shamir challenge
        transcript.append_point("L", L, ctx);
        transcript.append_point("R", R, ctx);
        Scalar x = transcript.challenge_scalar("x", ctx);
        Scalar x_inv = x.inverse(ctx);

        // In-place scalar fold: a[i] = a[i]*x + a[half+i]*x_inv
        //                       b[i] = b[i]*x_inv + b[half+i]*x
        for (size_t i = 0; i < half; ++i) {
            a[i] = a[i] * x + a[half + i] * x_inv;
            b[i] = b[i] * x_inv + b[half + i] * x;
        }
        a.resize(half);
        b.resize(half);

        // In-place generator fold: G_vec[i] = G_vec[i]*x_inv + G_vec[half+i]*x
        //                          H_vec[i] = H_vec[i]*x    + H_vec[half+i]*x_inv
        // Parallelised over hardware threads when half >= 256.
        {
            const size_t hw = std::thread::hardware_concurrency();
            const size_t nthreads = (half >= 256 && hw > 1)
                ? std::min(hw, half / 64)
                : 1;
            if (nthreads <= 1) {
                for (size_t i = 0; i < half; ++i) {
                    Point g_new = G_vec[i] * x_inv + G_vec[half + i] * x;
                    Point h_new = H_vec[i] * x     + H_vec[half + i] * x_inv;
                    G_vec[i] = g_new;
                    H_vec[i] = h_new;
                }
            } else {
                const size_t chunk = (half + nthreads - 1) / nthreads;
                std::vector<std::thread> threads;
                threads.reserve(nthreads);
                for (size_t t = 0; t < nthreads; ++t) {
                    const size_t begin = t * chunk;
                    const size_t end = std::min(begin + chunk, half);
                    if (begin >= half) break;
                    threads.emplace_back(
                        [&G_vec, &H_vec, begin, end, half, x, x_inv]() {
                            for (size_t i = begin; i < end; ++i) {
                                Point g_new = G_vec[i] * x_inv + G_vec[half + i] * x;
                                Point h_new = H_vec[i] * x     + H_vec[half + i] * x_inv;
                                G_vec[i] = g_new;
                                H_vec[i] = h_new;
                            }
                        });
                }
                for (auto& thr : threads) thr.join();
            }
        }
        G_vec.resize(half);
        H_vec.resize(half);

        n = half;
    }

    // Final scalars
    proof.a = a[0];
    proof.b = b[0];

    return proof;
}

// ---------------------------------------------------------------------------
// IPA Verifier
// ---------------------------------------------------------------------------

bool ipa_verify(
    const Point& commitment,
    const Scalar& ip_claim,
    const IPAProof& proof,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
) {
    size_t rounds = proof.num_rounds();
    size_t n = (size_t(1) << rounds);  // Derive from proof, not gens (cache may be oversized)

    // Check dimensions
    if (proof.L.size() != rounds || proof.R.size() != rounds) return false;
    if (gens.size() < n) return false;  // Generator set must have at least n elements

    // Replay transcript to get challenges
    transcript.append_point("P", commitment, ctx);

    std::vector<Scalar> challenges;
    for (size_t i = 0; i < rounds; ++i) {
        transcript.append_point("L", proof.L[i], ctx);
        transcript.append_point("R", proof.R[i], ctx);
        challenges.push_back(transcript.challenge_scalar("x", ctx));
    }

    // --- Precompute challenge inverses (eliminates ~n*rounds redundant inversions) ---
    // Before this fix: challenges[j].inverse(ctx) was called n*rounds times
    // inside the s[i] loop. Each inversion = 256 scalar muls (Fermat).
    // For n=512, rounds=9: 4,608 inversions × 256 muls = 1.18M wasted ops.
    // After: 9 inversions total.
    std::vector<Scalar> inv_challenges(rounds);
    std::vector<Scalar> challenges_sq(rounds);
    std::vector<Scalar> inv_challenges_sq(rounds);
    for (size_t j = 0; j < rounds; ++j) {
        inv_challenges[j] = challenges[j].inverse(ctx);
        challenges_sq[j] = challenges[j] * challenges[j];
        inv_challenges_sq[j] = inv_challenges[j] * inv_challenges[j];
    }

    // Compute s[i] and s_inv[i] in parallel — each element is independent.
    // For n=1M, 20 rounds: 2 × 20M scalar muls; previously serial.
    // s[i]     = product_j (challenges[j]     if bit set, inv_challenges[j] if not)
    // s_inv[i] = product_j (inv_challenges[j] if bit set, challenges[j]     if not)
    // Both computed in one combined pass to halve memory-access overhead.
    std::vector<Scalar> s(n);
    std::vector<Scalar> s_inv(n);
    {
        const size_t hw = std::thread::hardware_concurrency();
        const size_t nthreads = (n >= 512 && hw > 1) ? std::min(hw, (n + 63) / 64) : 1;
        const size_t chunk = (n + nthreads - 1) / nthreads;
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (size_t t = 0; t < nthreads; ++t) {
            const size_t begin = t * chunk;
            const size_t end = std::min(begin + chunk, n);
            if (begin >= n) break;
            threads.emplace_back([&challenges, &inv_challenges, &s, &s_inv,
                                   begin, end, rounds]() {
                for (size_t i = begin; i < end; ++i) {
                    Scalar si = Scalar::one();
                    Scalar si_inv = Scalar::one();
                    for (size_t j = 0; j < rounds; ++j) {
                        size_t bit = (i >> (rounds - 1 - j)) & 1;
                        si     = si     * (bit ? challenges[j]     : inv_challenges[j]);
                        si_inv = si_inv * (bit ? inv_challenges[j] : challenges[j]);
                    }
                    s[i]     = si;
                    s_inv[i] = si_inv;
                }
            });
        }
        for (auto& thr : threads) thr.join();
    }

    // LHS: P + sum(x_j^2 * L_j + x_j^{-2} * R_j)
    Point lhs = commitment;
    for (size_t j = 0; j < rounds; ++j) {
        lhs = lhs + (proof.L[j] * challenges_sq[j]) + (proof.R[j] * inv_challenges_sq[j]);
    }

    // RHS: a * (sum s_i * G_i) + b * (sum s_inv_i * H_i) + (a*b) * Q
    // Pre-sized vectors, filled in parallel (push_back is not thread-safe).
    std::vector<Scalar> rhs_scalars(2 * n + 1);
    std::vector<Point>  rhs_points(2 * n + 1);
    {
        const size_t hw = std::thread::hardware_concurrency();
        const size_t nthreads = (n >= 512 && hw > 1) ? std::min(hw, (n + 63) / 64) : 1;
        const size_t chunk = (n + nthreads - 1) / nthreads;
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (size_t t = 0; t < nthreads; ++t) {
            const size_t begin = t * chunk;
            const size_t end = std::min(begin + chunk, n);
            if (begin >= n) break;
            threads.emplace_back([&proof, &s, &s_inv, &gens, &rhs_scalars, &rhs_points,
                                   begin, end, n]() {
                const auto& G = gens.G();
                const auto& H = gens.H();
                for (size_t i = begin; i < end; ++i) {
                    rhs_scalars[i]     = proof.a * s[i];
                    rhs_points[i]      = G[i];
                    rhs_scalars[n + i] = proof.b * s_inv[i];
                    rhs_points[n + i]  = H[i];
                }
            });
        }
        for (auto& thr : threads) thr.join();
    }
    rhs_scalars[2 * n] = proof.a * proof.b;
    rhs_points[2 * n]  = gens.Q();

    Point rhs = Point::multi_scalar_mul(rhs_scalars, rhs_points, ctx);

    // Compare LHS == RHS by checking serialized forms
    Point::Compressed lhs_ser, rhs_ser;
    if (!lhs.serialize(lhs_ser, ctx)) return false;
    if (!rhs.serialize(rhs_ser, ctx)) return false;

    return lhs_ser == rhs_ser;
}

// ---------------------------------------------------------------------------
// Proof serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> IPAProof::serialize(secp256k1_context* ctx) const {
    std::vector<uint8_t> out;
    // Format: num_rounds(4) || L[0..k](33 each) || R[0..k](33 each) || a(32) || b(32)
    uint32_t k = static_cast<uint32_t>(L.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<uint8_t>((k >> (8*i)) & 0xff));

    for (const auto& p : L) {
        Point::Compressed c;
        p.serialize(c, ctx);
        out.insert(out.end(), c.begin(), c.end());
    }
    for (const auto& p : R) {
        Point::Compressed c;
        p.serialize(c, ctx);
        out.insert(out.end(), c.begin(), c.end());
    }
    out.insert(out.end(), a.data(), a.data() + 32);
    out.insert(out.end(), b.data(), b.data() + 32);

    return out;
}

bool IPAProof::deserialize(const std::vector<uint8_t>& data, IPAProof& out,
                           secp256k1_context* ctx) {
    if (data.size() < 4) return false;

    uint32_t k = (static_cast<uint32_t>(data[0]) << 24) |
                 (static_cast<uint32_t>(data[1]) << 16) |
                 (static_cast<uint32_t>(data[2]) << 8) |
                 static_cast<uint32_t>(data[3]);

    size_t expected = 4 + k * 33 * 2 + 64;
    if (data.size() < expected) return false;

    size_t offset = 4;

    out.L.resize(k);
    for (uint32_t i = 0; i < k; ++i) {
        if (!Point::parse(data.data() + offset, 33, out.L[i], ctx)) return false;
        offset += 33;
    }

    out.R.resize(k);
    for (uint32_t i = 0; i < k; ++i) {
        if (!Point::parse(data.data() + offset, 33, out.R[i], ctx)) return false;
        offset += 33;
    }

    out.a = Scalar(data.data() + offset); offset += 32;
    out.b = Scalar(data.data() + offset);

    return true;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
