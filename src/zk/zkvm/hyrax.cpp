// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/hyrax.h"
#include <cassert>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// ceil(sqrt(n)) — pure integer arithmetic (Newton's method).
//
// Apr 14 2026 (Bug #7 / #41) — DO NOT use std::sqrt(double) here.
// Floating-point sqrt is NOT bit-identical across CPU architectures: ARM64
// Apple Silicon and x86_64 Linux can return slightly different values for
// the same input, leading to off-by-one differences in the cast-to-size_t
// step. The previous implementation only had a downward correction loop
// (`while (s*s > n) --s`), which compensated for ARM-too-high float sqrt
// estimates, but NOT for ARM-too-low estimates where the missing upward
// correction left s one short and `s + 1` (the non-perfect-square return)
// then disagreed with x86 by one.
//
// Symptom: HyraxParams::from_n() produced different `n_cols` between Mac
// and Linux for the same Spartan witness count, leading to different Hyrax
// generator dimensions. Ring-covenant proofs generated on x86 servers then
// failed to verify on Mac with "Taproot index-commitment Spartan
// verification failed" — a real consensus split between architectures.
// All ring-covenant verification was broken cross-architecture.
//
// Fix: Newton's method integer square root, no floating-point at all.
// Convergent in O(log log n) iterations. Deterministic on every platform.
static size_t isqrt_ceil(size_t n) {
    if (n == 0) return 0;
    if (n == 1) return 1;
    // Newton's method to find floor(sqrt(n)). The initial guess `n` is
    // always >= floor(sqrt(n)), and the iteration `y = (x + n/x) / 2`
    // monotonically decreases until it converges (after which it would
    // start oscillating, which is when `y < x` becomes false).
    size_t x = n;
    size_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    // x is now floor(sqrt(n))
    if (x * x == n) return x;
    return x + 1;   // not a perfect square → ceil
}

static size_t next_pow2(size_t n) {
    if (n <= 1) return 1;
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

// Number of bits needed to address n distinct values: ceil(log2(n)).
// bits_for(1) = 0  (2^0 = 1 suffices)
// bits_for(2) = 1, bits_for(3)=bits_for(4)=2, etc.
static size_t bits_for(size_t n) {
    if (n <= 1) return 0;
    size_t b = 0;
    size_t v = n - 1;
    while (v > 0) { ++b; v >>= 1; }
    return b;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HyraxParams
// ---------------------------------------------------------------------------

HyraxParams HyraxParams::from_n(size_t n) {
    HyraxParams p;
    p.n_cols  = next_pow2(isqrt_ceil(n));          // power-of-2 column count
    p.n_rows  = (n + p.n_cols - 1) / p.n_cols;    // ceil(n / n_cols)
    p.n_total = p.n_rows * p.n_cols;
    return p;
}

// ---------------------------------------------------------------------------
// MLE equality-polynomial utilities
// ---------------------------------------------------------------------------

// mle_eq_vec(r): Returns a vector of size 2^|r| where entry i equals
//   eq(r, bits(i)) = ∏_k (r_k * x_k + (1 - r_k) * (1 - x_k))
// with x_k = (i >> k) & 1 (LSB-first bit indexing).
//
// Recurrence (in-place, O(2^|r|) field ops):
//   Start with eq[0] = 1.
//   For each bit k from 0 to |r|-1:
//     For each existing index i (from high to low to avoid overwrite):
//       eq[i + 2^k] = eq[i] * r_k        (bit k of new index is 1)
//       eq[i]       = eq[i] * (1 - r_k)  (bit k of new index is 0)
std::vector<Scalar> mle_eq_vec(const std::vector<Scalar>& r) {
    const size_t s = r.size();
    const size_t m = size_t(1) << s;
    std::vector<Scalar> eq(m, Scalar::zero());
    eq[0] = Scalar::one();

    for (size_t k = 0; k < s; ++k) {
        const Scalar& rk    = r[k];
        Scalar one_minus_rk = Scalar::one() - rk;
        const size_t half   = size_t(1) << k;
        // Iterate from half-1 down to 0 so we read eq[i] before overwriting it.
        for (size_t i = half; i-- > 0; ) {
            eq[i + half] = eq[i] * rk;
            eq[i]        = eq[i] * one_minus_rk;
        }
    }
    return eq;
}

// mle_eval(v, r): Evaluate the MLE of v at point r.
//   v must have size 2^|r|; the result is ∑_i v[i] * eq(r, bits(i)).
Scalar mle_eval(const std::vector<Scalar>& v, const std::vector<Scalar>& r) {
    assert(v.size() == (size_t(1) << r.size()));
    const std::vector<Scalar> eq = mle_eq_vec(r);
    Scalar result = Scalar::zero();
    for (size_t i = 0; i < v.size(); ++i) {
        result += v[i] * eq[i];
    }
    return result;
}

// ---------------------------------------------------------------------------
// Hyrax commit: C_i = <W_row_i, G[:n_cols]>  (one EC point per row)
// ---------------------------------------------------------------------------

HyraxCommitment hyrax_commit(
    const std::vector<Scalar>& W,
    const HyraxParams& params,
    const GeneratorSet& gens,
    secp256k1_context* ctx
) {
    assert(W.size() <= params.n_total);
    assert(gens.size() >= params.n_cols);

    HyraxCommitment comm;
    comm.params = params;
    comm.C.reserve(params.n_rows);

    const std::vector<Point>& G = gens.G();

    // Build the generator slice once (shared across all row commits)
    std::vector<Point> G_slice(G.cbegin(), G.cbegin() + params.n_cols);

    for (size_t row = 0; row < params.n_rows; ++row) {
        const size_t base = row * params.n_cols;
        std::vector<Scalar> row_scalars(params.n_cols, Scalar::zero());
        for (size_t col = 0; col < params.n_cols; ++col) {
            const size_t idx = base + col;
            if (idx < W.size()) row_scalars[col] = W[idx];
        }
        comm.C.push_back(Point::multi_scalar_mul(row_scalars, G_slice, ctx));
    }

    return comm;
}

// ---------------------------------------------------------------------------
// Hyrax evaluation proof
// ---------------------------------------------------------------------------

//
// Computes a proof that W̃(r) = v using the matrix decomposition.
//
// Bit-layout convention (matches mle_eq_vec LSB-first ordering):
//   flat index k = row * n_cols + col
//   col occupies bits [0, col_bits)  → r_col = r[0..col_bits)
//   row occupies bits [col_bits, ..) → r_row = r[col_bits..]
//
// Protocol (prover side):
//   1. Split r = (r_col || r_row).
//   2. Compute eq_row[i] = eq(r_row, bits(i)) for i ∈ [0, n_rows).
//   3. Compute d_witness[j] = ∑_i eq_row[i] · W[i·n_cols + j].
//   4. Compute b_col[j]   = eq(r_col, bits(j)) for j ∈ [0, n_cols).
//   5. v = <d_witness, b_col>.
//   6. Run IPA prove on (d_witness, b_col) — this internally forms
//      P = <d_witness, G[:n_cols]> + <b_col, H[:n_cols]> + v·Q
//      and appends P to the transcript.
//
HyraxEvalProof hyrax_eval_prove(
    const std::vector<Scalar>& W,
    const std::vector<Scalar>& r,
    const HyraxParams& params,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
) {
    assert(W.size() <= params.n_total);
    assert(gens.size() >= params.n_cols);

    // Split evaluation point: low bits → column, high bits → row.
    const size_t col_bits = bits_for(params.n_cols);   // n_cols is power of 2 → exact
    const size_t row_bits = bits_for(params.n_rows);
    assert(r.size() == col_bits + row_bits);

    const std::vector<Scalar> r_col(r.cbegin(),            r.cbegin() + col_bits);
    const std::vector<Scalar> r_row(r.cbegin() + col_bits, r.cend());

    // eq_row: full 2^row_bits vector, but we use only [0..n_rows)
    const std::vector<Scalar> eq_row_full = mle_eq_vec(r_row);

    // b_col[j] = eq(r_col, bits(j)) for j ∈ [0, n_cols)
    const std::vector<Scalar> b_col = mle_eq_vec(r_col);
    assert(b_col.size() == params.n_cols);

    // d_witness[j] = ∑_{i=0}^{n_rows-1} eq_row[i] · W[i·n_cols + j]
    std::vector<Scalar> d_witness(params.n_cols, Scalar::zero());
    for (size_t row = 0; row < params.n_rows; ++row) {
        const Scalar& eq_i = eq_row_full[row];
        const size_t base  = row * params.n_cols;
        for (size_t col = 0; col < params.n_cols; ++col) {
            const size_t idx = base + col;
            const Scalar w   = (idx < W.size()) ? W[idx] : Scalar::zero();
            d_witness[col]  += eq_i * w;
        }
    }

    // Claimed evaluation value
    const Scalar v = inner_product(d_witness, b_col);

    // IPA proof over the column dimension (length n_cols = power of 2)
    // ipa_prove computes P = <d_witness, G[:n_cols]> + <b_col, H[:n_cols]> + v·Q
    // and appends it to the transcript before the recursive folding.
    HyraxEvalProof proof;
    proof.col_ipa = ipa_prove(d_witness, b_col, gens, transcript, ctx);
    proof.claimed = v;
    return proof;
}

// ---------------------------------------------------------------------------
// Hyrax evaluation verify
// ---------------------------------------------------------------------------

//
// Verifier cost: O(n_rows) EC ops for D  +  O(n_cols) EC ops for <b_col, H>
//                +  O(n_cols) IPA verify  =  O(√n) total.
//
// Protocol (verifier side):
//   1. Split r = (r_col || r_row) — low bits col, high bits row.
//   2. Compute eq_row[i] = eq(r_row, bits(i)) for i ∈ [0, n_rows).
//   3. D = ∑_i eq_row[i] · C_i                    (MSM of n_rows points)
//   4. b_col[j] = eq(r_col, bits(j)).
//   5. P = D + <b_col, H[:n_cols]> + claimed · Q   (MSM of n_cols + 1 points)
//   6. ipa_verify(P, claimed, proof.col_ipa, gens, transcript, ctx)
//
bool hyrax_eval_verify(
    const HyraxCommitment& comm,
    const std::vector<Scalar>& r,
    const HyraxEvalProof& proof,
    const HyraxParams& params,
    const GeneratorSet& gens,
    Transcript& transcript,
    secp256k1_context* ctx
) {
    if (comm.C.size() != params.n_rows)  return false;
    if (gens.size() < params.n_cols)     return false;

    const size_t col_bits = bits_for(params.n_cols);
    const size_t row_bits = bits_for(params.n_rows);
    if (r.size() != col_bits + row_bits) return false;

    const std::vector<Scalar> r_col(r.cbegin(),            r.cbegin() + col_bits);
    const std::vector<Scalar> r_row(r.cbegin() + col_bits, r.cend());

    // eq_row[i] for i ∈ [0, n_rows)
    const std::vector<Scalar> eq_row_full = mle_eq_vec(r_row);
    std::vector<Scalar> eq_row(eq_row_full.cbegin(),
                               eq_row_full.cbegin() + params.n_rows);

    // b_col
    const std::vector<Scalar> b_col = mle_eq_vec(r_col);
    if (b_col.size() != params.n_cols) return false;

    // D = ∑_i eq_row[i] · C_i  (MSM of n_rows EC points — the fast part)
    const Point D = Point::multi_scalar_mul(eq_row, comm.C, ctx);

    // <b_col, H[:n_cols]>  (MSM of n_cols EC points)
    const std::vector<Point>& H_full = gens.H();
    const std::vector<Point> H_slice(H_full.cbegin(),
                                     H_full.cbegin() + params.n_cols);
    const Point bH = Point::multi_scalar_mul(b_col, H_slice, ctx);

    // P = D + <b_col, H[:n_cols]> + claimed · Q
    // This reconstructs exactly the commitment P that ipa_prove appended to the
    // transcript, so ipa_verify will observe the same transcript state.
    const Point P = D + bH + gens.Q() * proof.claimed;

    return ipa_verify(P, proof.claimed, proof.col_ipa, gens, transcript, ctx);
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

std::vector<uint8_t> HyraxCommitment::serialize(secp256k1_context* ctx) const {
    std::vector<uint8_t> out;
    // Format: n_rows(8) || n_cols(8) || n_total(8) || C[0..n_rows-1](33 each)
    auto write64 = [&](size_t v) {
        for (int i = 7; i >= 0; --i)
            out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    };
    write64(params.n_rows);
    write64(params.n_cols);
    write64(params.n_total);
    for (const Point& p : C) {
        Point::Compressed c;
        p.serialize(c, ctx);
        out.insert(out.end(), c.begin(), c.end());
    }
    return out;
}

bool HyraxCommitment::deserialize(const std::vector<uint8_t>& data, size_t& offset,
                                   HyraxCommitment& out, secp256k1_context* ctx) {
    if (data.size() < offset + 24) return false;
    auto read64 = [&]() -> size_t {
        size_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | data[offset++];
        return v;
    };
    out.params.n_rows  = read64();
    out.params.n_cols  = read64();
    out.params.n_total = read64();
    const size_t need  = out.params.n_rows * 33;
    if (data.size() < offset + need) return false;
    out.C.resize(out.params.n_rows);
    for (size_t i = 0; i < out.params.n_rows; ++i) {
        if (!Point::parse(data.data() + offset, 33, out.C[i], ctx)) return false;
        offset += 33;
    }
    return true;
}

std::vector<uint8_t> HyraxEvalProof::serialize(secp256k1_context* ctx) const {
    // Format: claimed(32) || IPAProof bytes
    std::vector<uint8_t> out;
    out.insert(out.end(), claimed.data(), claimed.data() + 32);
    const std::vector<uint8_t> ipa_bytes = col_ipa.serialize(ctx);
    out.insert(out.end(), ipa_bytes.begin(), ipa_bytes.end());
    return out;
}

bool HyraxEvalProof::deserialize(const std::vector<uint8_t>& data, size_t& offset,
                                  HyraxEvalProof& out, secp256k1_context* ctx) {
    if (data.size() < offset + 32) return false;
    out.claimed = Scalar(data.data() + offset);
    offset += 32;

    // IPAProof::deserialize reads from index 0 of the slice it is given.
    const std::vector<uint8_t> ipa_slice(data.cbegin() + offset, data.cend());
    if (!IPAProof::deserialize(ipa_slice, out.col_ipa, ctx)) return false;

    const size_t rounds = out.col_ipa.num_rounds();
    offset += 4 + rounds * 33 * 2 + 64;
    return true;
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
