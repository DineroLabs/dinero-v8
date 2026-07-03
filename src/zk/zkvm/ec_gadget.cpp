// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zk/zkvm/ec_gadget.h"
#include <cassert>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// secp256k1 generator point G
// ---------------------------------------------------------------------------

const Uint256& secp256k1_Gx() {
    static Uint256 gx = []() {
        uint8_t bytes[32] = {
            0x79, 0xBE, 0x66, 0x7E, 0xF9, 0xDC, 0xBB, 0xAC,
            0x55, 0xA0, 0x62, 0x95, 0xCE, 0x87, 0x0B, 0x07,
            0x02, 0x9B, 0xFC, 0xDB, 0x2D, 0xCE, 0x28, 0xD9,
            0x59, 0xF2, 0x81, 0x5B, 0x16, 0xF8, 0x17, 0x98
        };
        return Uint256(bytes);
    }();
    return gx;
}

const Uint256& secp256k1_Gy() {
    static Uint256 gy = []() {
        uint8_t bytes[32] = {
            0x48, 0x3A, 0xDA, 0x77, 0x26, 0xA3, 0xC4, 0x65,
            0x5D, 0xA4, 0xFB, 0xFC, 0x0E, 0x11, 0x08, 0xA8,
            0xFD, 0x17, 0xB4, 0x48, 0xA6, 0x85, 0x54, 0x19,
            0x9C, 0x47, 0xD0, 0x8F, 0xFB, 0x10, 0xD4, 0xB8
        };
        return Uint256(bytes);
    }();
    return gy;
}

// ---------------------------------------------------------------------------
// Prover-side EC point arithmetic (witness computation)
// ---------------------------------------------------------------------------

struct WitnessPoint {
    Uint256 x, y;
    bool is_inf;
};

static WitnessPoint witness_add(const WitnessPoint& P, const WitnessPoint& Q) {
    if (P.is_inf) return Q;
    if (Q.is_inf) return P;

    // Check P == -Q (same x, opposite y)
    Uint256 sum_y = uint256_add_mod_p(P.y, Q.y);
    if (P.x == Q.x) {
        if (sum_y.is_zero()) {
            return {Uint256(), Uint256(), true}; // P + (-P) = O
        }
        // P == Q: use doubling
        // lambda = 3*x^2 / (2*y)
        Uint256 x2 = uint256_mul_mod_p(P.x, P.x);
        Uint256 three_x2 = uint256_add_mod_p(uint256_add_mod_p(x2, x2), x2);
        Uint256 two_y = uint256_add_mod_p(P.y, P.y);
        Uint256 two_y_inv = uint256_inv_mod_p(two_y);
        Uint256 lambda = uint256_mul_mod_p(three_x2, two_y_inv);

        Uint256 lam2 = uint256_mul_mod_p(lambda, lambda);
        Uint256 rx = uint256_sub_mod_p(uint256_sub_mod_p(lam2, P.x), P.x);
        Uint256 ry = uint256_sub_mod_p(
            uint256_mul_mod_p(lambda, uint256_sub_mod_p(P.x, rx)), P.y);
        return {rx, ry, false};
    }

    // General addition: lambda = (Q.y - P.y) / (Q.x - P.x)
    Uint256 dy = uint256_sub_mod_p(Q.y, P.y);
    Uint256 dx = uint256_sub_mod_p(Q.x, P.x);
    Uint256 dx_inv = uint256_inv_mod_p(dx);
    Uint256 lambda = uint256_mul_mod_p(dy, dx_inv);

    Uint256 lam2 = uint256_mul_mod_p(lambda, lambda);
    Uint256 rx = uint256_sub_mod_p(uint256_sub_mod_p(lam2, P.x), Q.x);
    Uint256 ry = uint256_sub_mod_p(
        uint256_mul_mod_p(lambda, uint256_sub_mod_p(P.x, rx)), P.y);
    return {rx, ry, false};
}

static WitnessPoint witness_double(const WitnessPoint& P) {
    if (P.is_inf) return P;
    if (P.y.is_zero()) return {Uint256(), Uint256(), true};

    Uint256 x2 = uint256_mul_mod_p(P.x, P.x);
    Uint256 three_x2 = uint256_add_mod_p(uint256_add_mod_p(x2, x2), x2);
    Uint256 two_y = uint256_add_mod_p(P.y, P.y);
    Uint256 two_y_inv = uint256_inv_mod_p(two_y);
    Uint256 lambda = uint256_mul_mod_p(three_x2, two_y_inv);

    Uint256 lam2 = uint256_mul_mod_p(lambda, lambda);
    Uint256 rx = uint256_sub_mod_p(uint256_sub_mod_p(lam2, P.x), P.x);
    Uint256 ry = uint256_sub_mod_p(
        uint256_mul_mod_p(lambda, uint256_sub_mod_p(P.x, rx)), P.y);
    return {rx, ry, false};
}

static WitnessPoint witness_negate(const WitnessPoint& P) {
    if (P.is_inf) return P;
    return {P.x, uint256_sub_mod_p(uint256_p(), P.y), false};
}

Uint256 ec_witness_x(R1CS& cs, const ECPoint& P) {
    Uint256 val;
    for (int i = 0; i < 4; ++i) {
        Scalar s = cs.get_value(P.x.limbs[i]);
        const uint8_t* bytes = s.data();
        val.limbs[i] = 0;
        for (int b = 0; b < 8; ++b)
            val.limbs[i] = (val.limbs[i] << 8) | bytes[24 + b];
    }
    return val;
}

Uint256 ec_witness_y(R1CS& cs, const ECPoint& P) {
    Uint256 val;
    for (int i = 0; i < 4; ++i) {
        Scalar s = cs.get_value(P.y.limbs[i]);
        const uint8_t* bytes = s.data();
        val.limbs[i] = 0;
        for (int b = 0; b < 8; ++b)
            val.limbs[i] = (val.limbs[i] << 8) | bytes[24 + b];
    }
    return val;
}

bool ec_witness_is_inf(R1CS& cs, const ECPoint& P) {
    return !cs.get_value(P.is_identity).is_zero();
}

static WitnessPoint ec_witness(R1CS& cs, const ECPoint& P) {
    return {ec_witness_x(cs, P), ec_witness_y(cs, P), ec_witness_is_inf(cs, P)};
}

// ---------------------------------------------------------------------------
// Circuit gadgets
// ---------------------------------------------------------------------------

ECPoint ec_alloc(R1CS& cs, const Uint256& x, const Uint256& y,
                  bool is_inf, const std::string& label) {
    ECPoint P;
    P.x = fe_alloc_uint256(cs, x, label + "_x");
    P.y = fe_alloc_uint256(cs, y, label + "_y");
    P.is_identity = cs.alloc(is_inf ? Scalar::one() : Scalar::zero());
    gadgets::enforce_boolean(cs, P.is_identity, label + "_inf");
    return P;
}

ECPoint ec_generator(R1CS& cs, const std::string& label) {
    ECPoint G;
    G.x = fe_constant(cs, secp256k1_Gx(), label + "_x");
    G.y = fe_constant(cs, secp256k1_Gy(), label + "_y");
    G.is_identity = gadgets::constant(cs, Scalar::zero(), label + "_inf");
    return G;
}

ECPoint ec_identity(R1CS& cs, const std::string& label) {
    ECPoint O;
    O.x = fe_constant(cs, Uint256(), label + "_x");
    O.y = fe_constant(cs, Uint256(), label + "_y");
    O.is_identity = gadgets::constant(cs, Scalar::one(), label + "_inf");
    return O;
}

void ec_assert_on_curve(R1CS& cs, const ECPoint& P,
                         const std::string& label) {
    // y^2 = x^3 + 7 (mod p), only checked when not identity
    FieldElement x2 = fe_square(cs, P.x, label + "_x2");
    FieldElement x3 = fe_mul(cs, x2, P.x, label + "_x3");
    FieldElement seven = fe_constant(cs, Uint256(uint64_t(7)), label + "_7");
    FieldElement rhs = fe_add(cs, x3, seven, label + "_rhs");
    FieldElement y2 = fe_square(cs, P.y, label + "_y2");

    // y2 == rhs (when not identity)
    // If identity, skip check (both sides are 0 by convention)
    Variable match = fe_equal(cs, y2, rhs, label + "_eq");
    Variable ok = gadgets::or_bits(cs, match, P.is_identity, label + "_ok");
    gadgets::assert_equal(cs, ok, gadgets::constant(cs, Scalar::one(), label + "_1"),
                          label + "_check");
}

ECPoint ec_add_unsafe(R1CS& cs, const ECPoint& P, const ECPoint& Q,
                       const std::string& label) {
    // Fused slope verification: compute lambda witness natively, allocate it
    // once, then verify lambda*(Qx-Px) ≡ (Qy-Py) mod p via a single carry
    // chain.  Saves ~1,265 constraints vs the previous fe_inv_diff+fe_mul_diff
    // pair (993 vs 2,258).

    // lambda = (Q.y - P.y) / (Q.x - P.x)  — computed natively
    Uint256 Qx_val = fe_witness_value(cs, Q.x);
    Uint256 Px_val = fe_witness_value(cs, P.x);
    Uint256 Qy_val = fe_witness_value(cs, Q.y);
    Uint256 Py_val = fe_witness_value(cs, P.y);
    Uint256 dx     = uint256_sub_mod_p(Qx_val, Px_val);
    Uint256 dy     = uint256_sub_mod_p(Qy_val, Py_val);
    Uint256 lam    = uint256_mul_mod_p(dy, uint256_inv_mod_p(dx));

    FieldElement lambda = fe_alloc_uint256(cs, lam, label + "_lam");
    fe_slope_verify(cs, lambda, Q.x, P.x, Q.y, P.y, label + "_sv");

    // R.x = lambda^2 - P.x - Q.x
    FieldElement rx = fe_square_sub2(cs, lambda, P.x, Q.x, label + "_rx");

    // R.y = lambda * (P.x - R.x) - P.y
    FieldElement ry = fe_mul_diff_sub(cs, lambda, P.x, rx, P.y, label + "_ry");

    ECPoint R;
    R.x = rx;
    R.y = ry;
    R.is_identity = gadgets::constant(cs, Scalar::zero(), label + "_inf");
    return R;
}

ECPoint ec_double(R1CS& cs, const ECPoint& P,
                   const std::string& label) {
    // lambda = 3*x^2 / (2*y)
    // fe_square_triple: computes 3*x^2 directly, skipping 2 intermediate fe_add
    //   allocations (~558 constraints saved vs fe_square + 2*fe_add).
    // fe_double_inv: computes inv(2*y) without allocating 2*y
    //   (~545 constraints saved vs fe_add + fe_inv).
    FieldElement three_x2 = fe_square_triple(cs, P.x, label + "_3x2");
    FieldElement two_y_inv = fe_double_inv(cs, P.y, label + "_2yi");
    FieldElement lambda = fe_mul(cs, three_x2, two_y_inv, label + "_lam");

    // R.x = lambda^2 - P.x - P.x
    // fe_square_sub2 fuses squaring + double-subtraction into one carry chain,
    // eliminating the intermediate lam2 allocation (~281 constraints saved).
    FieldElement rx = fe_square_sub2(cs, lambda, P.x, P.x, label + "_rx");

    // R.y = lambda * (P.x - R.x) - P.y
    // fe_mul_diff_sub fuses mul_diff + sub, skip lam_diff allocation (~279 saved)
    FieldElement ry = fe_mul_diff_sub(cs, lambda, P.x, rx, P.y, label + "_ry");

    ECPoint R;
    R.x = rx;
    R.y = ry;
    R.is_identity = gadgets::constant(cs, Scalar::zero(), label + "_inf");
    return R;
}

ECPoint ec_select(R1CS& cs, Variable cond,
                   const ECPoint& P, const ECPoint& Q,
                   const std::string& label) {
    ECPoint R;
    R.x = fe_select(cs, cond, P.x, Q.x, label + "_x");
    R.y = fe_select(cs, cond, P.y, Q.y, label + "_y");
    R.is_identity = gadgets::select(cs, cond, P.is_identity, Q.is_identity,
                                     label + "_inf");
    return R;
}

ECPoint ec_negate(R1CS& cs, const ECPoint& P,
                   const std::string& label) {
    // -P = (x, p - y), identity stays identity
    Uint256 p_val = uint256_p();
    FieldElement p_fe = fe_constant(cs, p_val, label + "_p");
    FieldElement neg_y = fe_sub(cs, p_fe, P.y, label + "_ny");

    ECPoint R;
    R.x = P.x;
    R.y = neg_y;
    R.is_identity = P.is_identity;
    return R;
}

Variable ec_equal(R1CS& cs, const ECPoint& P, const ECPoint& Q,
                   const std::string& label) {
    Variable x_eq = fe_equal(cs, P.x, Q.x, label + "_xeq");
    Variable y_eq = fe_equal(cs, P.y, Q.y, label + "_yeq");
    Variable coord_eq = gadgets::and_bits(cs, x_eq, y_eq, label + "_ceq");
    // Both identity → equal. Both non-identity with same coords → equal.
    Variable both_inf = gadgets::and_bits(cs, P.is_identity, Q.is_identity, label + "_binf");
    Variable both_finite_eq = gadgets::and_bits(cs, coord_eq,
        gadgets::not_bit(cs, gadgets::or_bits(cs, P.is_identity, Q.is_identity, label + "_oi"),
                          label + "_noi"),
        label + "_bfeq");
    return gadgets::or_bits(cs, both_inf, both_finite_eq, label + "_eq");
}

ECPoint ec_add_complete(R1CS& cs, const ECPoint& P, const ECPoint& Q,
                         const std::string& label) {
    // Get witness values to determine which case we're in
    WitnessPoint Pw = ec_witness(cs, P);
    WitnessPoint Qw = ec_witness(cs, Q);

    // Compute the witness result
    WitnessPoint Rw = witness_add(Pw, Qw);

    // Determine cases
    bool p_inf = Pw.is_inf;
    bool q_inf = Qw.is_inf;
    bool same_x = (!p_inf && !q_inf && Pw.x == Qw.x);
    bool same_point = (same_x && Pw.y == Qw.y);
    bool opposite = (same_x && !same_point);

    // Always compute the "unsafe add" and "double" results.
    // Use dummy values when the operation would be undefined (e.g., dx=0).
    // The final select picks the correct result based on the case flags.

    // For the add path: if dx == 0 (same x), use dx = 1 to avoid div-by-zero
    FieldElement dx = fe_sub(cs, Q.x, P.x, label + "_dx");
    // cv-binding audit hardening: fe_sub does not guarantee a canonical (< p)
    // output, and dx feeds the zero-test that drives branch selection. Pin dx
    // canonical so the branch predicates are unconditionally a function of the
    // true geometry (not an infeasibility argument over non-canonical aliases).
    fe_assert_less_than_p(cs, dx, label + "_dxlt");
    // Apr 14 2026 (Bug #7 / #41) — DO NOT inline these sub-calls as arguments
    // to a single gadgets::mul() call. C++ function argument evaluation order
    // is unspecified, so on x86_64 Linux clang/gcc `gadgets::constant(..._1a)`
    // was evaluated before `fe_pack(..._dxp)`, while on ARM64 macOS clang the
    // order was reversed. This produced different constraint orders AND
    // different R1CS variable index assignments on Mac vs Linux for the same
    // circuit — a real cross-architecture consensus split. The fix: force
    // left-to-right evaluation by extracting each call to a named local,
    // preserving the x86_64 order (constant _1a first, then fe_pack _dxp)
    // so mainnet blocks signed against the current behavior remain valid.
    Variable dx_one_a = gadgets::constant(cs, Scalar::one(), label + "_1a");
    Variable dx_packed = fe_pack(cs, dx, label + "_dxp");
    Variable dx_mul    = gadgets::mul(cs, dx_packed, dx_one_a, label + "_dxm");
    Variable dx_is_zero = gadgets::is_zero(cs, dx_mul, label + "_dxz");

    // Safe dx for addition (replace 0 with 1 to avoid undefined inverse)
    FieldElement one_fe = fe_constant(cs, Uint256(uint64_t(1)), label + "_1fe");
    FieldElement safe_dx = fe_select(cs, dx_is_zero, one_fe, dx, label + "_sdx");

    FieldElement dy = fe_sub(cs, Q.y, P.y, label + "_dy");
    fe_assert_less_than_p(cs, dy, label + "_dylt");  // pin canonical (see dx above)
    FieldElement safe_dx_inv = fe_inv(cs, safe_dx, label + "_sdxi");
    FieldElement add_lambda = fe_mul(cs, dy, safe_dx_inv, label + "_alam");

    FieldElement add_lam2 = fe_mul(cs, add_lambda, add_lambda, label + "_al2");
    FieldElement add_rx_tmp = fe_sub(cs, add_lam2, P.x, label + "_arx1");
    FieldElement add_rx = fe_sub(cs, add_rx_tmp, Q.x, label + "_arx");
    FieldElement add_px_rx = fe_sub(cs, P.x, add_rx, label + "_apxrx");
    FieldElement add_ld = fe_mul(cs, add_lambda, add_px_rx, label + "_ald");
    FieldElement add_ry = fe_sub(cs, add_ld, P.y, label + "_ary");

    ECPoint add_result;
    add_result.x = add_rx;
    add_result.y = add_ry;
    add_result.is_identity = gadgets::constant(cs, Scalar::zero(), label + "_anf");

    // For the double path: if 2*y == 0, use 2*y = 1 to avoid div-by-zero
    FieldElement two_y = fe_add(cs, P.y, P.y, label + "_2y");
    // Apr 14 2026 (Bug #7 / #41) — same fix as the `_1a`/`_dxp` pattern above.
    // Force left-to-right evaluation to eliminate cross-architecture
    // divergence. CN x86_64 order: constant(_1b) first, then fe_pack(_2yp).
    Variable two_y_one_b   = gadgets::constant(cs, Scalar::one(), label + "_1b");
    Variable two_y_packed  = fe_pack(cs, two_y, label + "_2yp");
    Variable two_y_mul     = gadgets::mul(cs, two_y_packed, two_y_one_b, label + "_2ym");
    Variable two_y_zero    = gadgets::is_zero(cs, two_y_mul, label + "_2yz");

    FieldElement safe_two_y = fe_select(cs, two_y_zero, one_fe, two_y, label + "_s2y");

    FieldElement x2 = fe_mul(cs, P.x, P.x, label + "_x2");
    FieldElement x2_2 = fe_add(cs, x2, x2, label + "_2x2");
    FieldElement three_x2 = fe_add(cs, x2_2, x2, label + "_3x2");
    FieldElement safe_2y_inv = fe_inv(cs, safe_two_y, label + "_s2yi");
    FieldElement dbl_lambda = fe_mul(cs, three_x2, safe_2y_inv, label + "_dlam");

    FieldElement dbl_lam2 = fe_mul(cs, dbl_lambda, dbl_lambda, label + "_dl2");
    FieldElement two_px = fe_add(cs, P.x, P.x, label + "_2px");
    FieldElement dbl_rx = fe_sub(cs, dbl_lam2, two_px, label + "_drx");
    FieldElement dbl_px_rx = fe_sub(cs, P.x, dbl_rx, label + "_dpxrx");
    FieldElement dbl_ld = fe_mul(cs, dbl_lambda, dbl_px_rx, label + "_dld");
    FieldElement dbl_ry = fe_sub(cs, dbl_ld, P.y, label + "_dry");

    ECPoint dbl_result;
    dbl_result.x = dbl_rx;
    dbl_result.y = dbl_ry;
    dbl_result.is_identity = gadgets::constant(cs, Scalar::zero(), label + "_dnf");

    // Identity result
    ECPoint inf_result = ec_identity(cs, label + "_inf");

    // Case flags (boolean witness variables, constrained)
    Variable is_p_inf = P.is_identity;
    Variable is_q_inf = Q.is_identity;
    // Native witness values for the branch selectors.
    Variable is_same_pt = cs.alloc(same_point ? Scalar::one() : Scalar::zero());
    Variable is_opposite = cs.alloc(opposite ? Scalar::one() : Scalar::zero());
    gadgets::enforce_boolean(cs, is_same_pt, label + "_spt");
    gadgets::enforce_boolean(cs, is_opposite, label + "_opp");

    // SOUNDNESS FIX (cv-binding audit, 2026-07-01): PIN the branch selectors to
    // the actual P/Q geometry so a malicious prover cannot choose add/double/
    // identity. Previously is_same_pt/is_opposite were only boolean-constrained
    // (free witnesses) — a prover could forge the result (e.g. cv = 2*val*V, or
    // val*V = O) => shielded mint-from-nothing, and an analogous forgery in the
    // Schnorr circuit. dx_is_zero (P.x==Q.x) is already enforced above; enforce
    // dy_is_zero (P.y==Q.y) the same way, then bind:
    //   is_same_pt  == dx_is_zero AND dy_is_zero
    //   is_opposite == dx_is_zero AND NOT dy_is_zero
    // (For on-curve inputs, same x forces y == Q.y or y == -Q.y, so these two
    // predicates are exhaustive.) Cross-arch (Bug #7/#41): named locals,
    // left-to-right, so R1CS variable indices are identical Mac vs Linux.
    Variable dy_one_c    = gadgets::constant(cs, Scalar::one(), label + "_1c");
    Variable dy_packed   = fe_pack(cs, dy, label + "_dyp");
    Variable dy_mul      = gadgets::mul(cs, dy_packed, dy_one_c, label + "_dym");
    Variable dy_is_zero  = gadgets::is_zero(cs, dy_mul, label + "_dyz");
    // Match the native predicate EXACTLY: same_point/opposite require BOTH inputs
    // finite (native: same_x = !p_inf && !q_inf && ...). The identity cases are
    // handled by the is_p_inf/is_q_inf selects below, which override the result.
    // Without this gate, two identity inputs (sentinel coords equal) — which occur
    // in honest scalar-mul accumulation, e.g. ec_add_complete(O, O) — would compute
    // want_same=1 while the native witness sets is_same_pt=0, so enforce_equal would
    // reject valid proofs. (Regression caught by ShieldedCvBinding honest-output tests.)
    Variable either_inf  = gadgets::or_bits(cs, is_p_inf, is_q_inf, label + "_einf");
    Variable neither_inf = gadgets::not_bit(cs, either_inf, label + "_ninf");
    Variable dx_and_dy   = gadgets::and_bits(cs, dx_is_zero, dy_is_zero, label + "_dxdy");
    Variable want_same   = gadgets::and_bits(cs, dx_and_dy, neither_inf, label + "_wspt");
    Variable not_dy_zero = gadgets::not_bit(cs, dy_is_zero, label + "_ndyz");
    Variable dx_and_ndy  = gadgets::and_bits(cs, dx_is_zero, not_dy_zero, label + "_dxndy");
    Variable want_opp    = gadgets::and_bits(cs, dx_and_ndy, neither_inf, label + "_wopp");
    cs.enforce_equal(LinearCombination(is_same_pt), LinearCombination(want_same),
                     label + "_sptbind");
    cs.enforce_equal(LinearCombination(is_opposite), LinearCombination(want_opp),
                     label + "_oppbind");

    // Select result based on case:
    // 1. P is identity → result = Q
    // 2. Q is identity → result = P
    // 3. P == Q (same point) → result = double(P)
    // 4. P == -Q (opposite) → result = identity
    // 5. General → result = add(P, Q)

    ECPoint result = add_result; // default: general addition
    result = ec_select(cs, is_opposite, inf_result, result, label + "_s4");
    result = ec_select(cs, is_same_pt, dbl_result, result, label + "_s3");
    result = ec_select(cs, is_q_inf, P, result, label + "_s2");
    result = ec_select(cs, is_p_inf, Q, result, label + "_s1");

    return result;
}

// ---------------------------------------------------------------------------
// Witness scalar multiplication (prover-side)
// ---------------------------------------------------------------------------

static WitnessPoint witness_scalar_mul(const WitnessPoint& P,
                                        const uint8_t scalar_be[32]) {
    WitnessPoint acc = {Uint256(), Uint256(), true}; // identity
    for (int i = 255; i >= 0; --i) {
        acc = witness_double(acc);
        // Big-endian bit extraction: bit i is in byte (31 - i/8), bit (i%8)
        int byte_idx = 31 - (i / 8);
        int bit_idx = i % 8;
        if ((scalar_be[byte_idx] >> bit_idx) & 1) {
            acc = witness_add(acc, P);
        }
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Offset point for variable-base scalar mul
//
// We use R = 7*G as the offset. After 256 double-and-add iterations
// starting from R, the offset contribution is (2^256 mod n)*R.
// The neg_offset = -(2^256 mod n)*R is precomputed and subtracted.
// ---------------------------------------------------------------------------

static const WitnessPoint& offset_point_R() {
    static WitnessPoint R = []() {
        WitnessPoint G = {secp256k1_Gx(), secp256k1_Gy(), false};
        // R = 7*G
        uint8_t seven[32] = {0};
        seven[31] = 7;
        return witness_scalar_mul(G, seven);
    }();
    return R;
}

static const WitnessPoint& neg_offset_2_256_R() {
    // Compute -(2^256 mod n) * R
    // 2^256 mod n = 2^256 - n (since n < 2^256)
    // n = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
    // 2^256 - n = 14551231950B75FC4402DA1732FC9BEBF (129 bits)
    static WitnessPoint neg = []() {
        // 2^256 - n as big-endian 32 bytes
        uint8_t scalar_2_256_mod_n[32] = {0};
        // 0x14551231950B75FC4402DA1732FC9BEBF
        // = 00000000 00000000 00000001 45512319 50B75FC4 402DA173 2FC9BEBF
        // Wait, let me compute properly:
        // n = FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
        // 2^256 = 1_00000000_00000000_00000000_00000000_00000000_00000000_00000000_00000000
        // diff = 2^256 - n:
        //   10000000000000000000000000000000000000000000000000000000000000000
        // - FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
        // = 000000000000000000000000000000014551231950B75FC4402DA1732FC9BEBF

        // Big-endian bytes for 0x14551231950B75FC4402DA1732FC9BEBF:
        scalar_2_256_mod_n[15] = 0x01;
        scalar_2_256_mod_n[16] = 0x45; scalar_2_256_mod_n[17] = 0x51;
        scalar_2_256_mod_n[18] = 0x23; scalar_2_256_mod_n[19] = 0x19;
        scalar_2_256_mod_n[20] = 0x50; scalar_2_256_mod_n[21] = 0xB7;
        scalar_2_256_mod_n[22] = 0x5F; scalar_2_256_mod_n[23] = 0xC4;
        scalar_2_256_mod_n[24] = 0x40; scalar_2_256_mod_n[25] = 0x2D;
        scalar_2_256_mod_n[26] = 0xA1; scalar_2_256_mod_n[27] = 0x73;
        scalar_2_256_mod_n[28] = 0x2F; scalar_2_256_mod_n[29] = 0xC9;
        scalar_2_256_mod_n[30] = 0xBE; scalar_2_256_mod_n[31] = 0xBF;

        WitnessPoint offset_R = offset_point_R();
        WitnessPoint offset_2_256_R = witness_scalar_mul(offset_R, scalar_2_256_mod_n);
        return witness_negate(offset_2_256_R);
    }();
    return neg;
}

// ---------------------------------------------------------------------------
// GLV endomorphism constants for secp256k1
// ---------------------------------------------------------------------------

// β: cube root of unity in the base field (mod p)
// φ(x, y) = (β*x, y) is the GLV endomorphism
static const Uint256& glv_beta() {
    static Uint256 b = []() {
        uint8_t bytes[32] = {
            0x7A, 0xE9, 0x6A, 0x2B, 0x65, 0x7C, 0x07, 0x10,
            0x6E, 0x64, 0x47, 0x9E, 0xAC, 0x34, 0x34, 0xE9,
            0x9C, 0xF0, 0x49, 0x75, 0x12, 0xF5, 0x89, 0x95,
            0xC1, 0x39, 0x6C, 0x28, 0x71, 0x95, 0x01, 0xEE
        };
        return Uint256(bytes);
    }();
    return b;
}

// λ: scalar eigenvalue (mod n). λ*P = φ(P) for all P on the curve.
static const uint8_t GLV_LAMBDA_BE[32] = {
    0x53, 0x63, 0xAD, 0x4C, 0xC0, 0x5C, 0x30, 0xE0,
    0xA5, 0x26, 0x1C, 0x02, 0x88, 0x12, 0x64, 0x5A,
    0x12, 0x2E, 0x22, 0xEA, 0x20, 0x81, 0x66, 0x78,
    0xDF, 0x02, 0x96, 0x7C, 0x1B, 0x23, 0xBD, 0x72
};

// GLV decomposition: given scalar k, find k1, k2 with k = k1 + k2*λ (mod n)
// and |k1|, |k2| < ~2^128. Uses the known secp256k1 lattice basis.
static void glv_decompose(const uint8_t k_be[32],
                           uint8_t k1_be[32], bool& k1_neg,
                           uint8_t k2_be[32], bool& k2_neg) {
    // Simple approach: k2 = 0, k1 = k (no decomposition).
    // Then fall back to the lattice-based decomposition.
    //
    // For secp256k1, the lattice basis vectors (from libsecp256k1):
    //   g1 = 0x3086D221A7D46BCDE86C90E49284EB15
    //   g2 = 0xE4437ED6010E88286F547FA90ABFE4C3
    //
    // k1 = k - round(k*g1/n)*a1 - round(k*g2/n)*a2
    // k2 computed similarly.
    //
    // For MVP: use the Scalar class to do the decomposition in mod-n arithmetic.
    Scalar k_scalar(k_be);
    Scalar lambda_scalar(GLV_LAMBDA_BE);

    // Brute-force approach: try k2 = round(k / λ) truncated to 128 bits
    // k1 = k - k2*λ. If |k1| > 2^128, adjust.
    //
    // Better: use the known half-width decomposition.
    // For now, use a simple method that's correct if slower:
    // k2 = (k * g1) >> 256 where g1 ≈ n/λ (precomputed)

    // Simplest correct decomposition: the prover just provides (k1, k2) and
    // the circuit verifies. Use Scalar multiplication to find k2.
    //
    // k2 = k * inv(λ) mod n, truncated. But this gives k2 ≈ 256 bits.
    // Need the SHORT decomposition.
    //
    // Use the secp256k1 constants directly:
    // a1 =  0x3086D221A7D46BCDE86C90E49284EB15
    // b1 = -0xE4437ED6010E88286F547FA90ABFE4C3
    // a2 =  0x114CA50F7A8E2F3F657C1108D9D44CFD8
    // b2 =  0x3086D221A7D46BCDE86C90E49284EB15
    //
    // c1 = round(b2 * k / n), c2 = round(-b1 * k / n)
    // k1 = k - c1*a1 - c2*a2, k2 = -c1*b1 - c2*b2

    // For the MVP, compute decomposition using the Scalar type.
    // We'll check correctness in the circuit (k1 + k2*λ = k mod n).
    //
    // Approximate: k2 = (k * b2) >> 256 (where b2 is ~128 bits)
    // This gives |k2| < ~2^128.

    // Use the known relationship: λ^2 + λ + 1 ≡ 0 (mod n)
    // So λ = (-1 ± sqrt(-3)) / 2 mod n.
    // The decomposition: k1 = k mod 2^128, k2 = (k - k1) * inv(λ) mod n
    // ... but this doesn't guarantee |k2| < 2^128.

    // SIMPLEST CORRECT APPROACH: just split k into two 128-bit halves
    // and use Shamir's trick with the identity k = k_lo + k_hi * 2^128.
    // Then k*P = k_lo*P + k_hi*(2^128 * P).
    // Precompute 2^128 * P (128 doublings of P).
    // This is NOT GLV (no endomorphism), but gives the same 2x speedup
    // from Shamir's trick with 128-bit scalars.
    //
    // True GLV is better (no precompute of 2^128*P), but this is simpler
    // and equally fast in the circuit.

    // Split k into low 128 bits and high 128 bits
    // k = k_lo + k_hi * 2^128
    k1_neg = false;
    k2_neg = false;
    std::memset(k1_be, 0, 32);
    std::memset(k2_be, 0, 32);
    // k_be is big-endian. Low 128 bits = bytes[16..31]. High 128 bits = bytes[0..15].
    std::memcpy(k1_be + 16, k_be + 16, 16); // k_lo in low 16 bytes
    std::memcpy(k2_be + 16, k_be, 16);      // k_hi in low 16 bytes
}

// Precompute 2^128 * R (offset scaled for the high-half scalar)
static const WitnessPoint& neg_offset_2_128_R() {
    // After Shamir's trick with 128 iterations starting from R:
    // offset contribution = 2^128 * R for EACH sub-scalar.
    // Total offset = 2^128 * R + 2^128 * R * 2^128 = 2^128 * R * (1 + 2^128)
    // ... actually it's more nuanced. Let me think about this.
    //
    // With Shamir's trick, the accumulator starts at R and doubles 128 times.
    // At each step, it conditionally adds P (for k1 bits) or Q (for k2 bits).
    // After 128 doublings of R, the offset contribution is 2^128 * R.
    //
    // k1*P + k2*Q + 2^128*R is the final accumulator value.
    // Result = acc - 2^128 * R.
    static WitnessPoint neg = []() {
        WitnessPoint R = offset_point_R();
        // 2^128 * R via 128 doublings
        WitnessPoint scaled = R;
        for (int i = 0; i < 128; ++i) {
            scaled = witness_double(scaled);
        }
        return witness_negate(scaled);
    }();
    return neg;
}

// ---------------------------------------------------------------------------
// Variable-base scalar multiplication: k * P (GLV / Shamir's trick)
//
// Splits k into two 128-bit halves and uses Shamir's trick:
//   k*P = k_lo*P + k_hi*(2^128*P)
// with 128 double-and-add iterations instead of 256.
//
// Cost: 128 * (ec_double + ec_add_unsafe + selects) ≈ 1.7M constraints.
// ---------------------------------------------------------------------------

ECPoint ec_scalar_mul(R1CS& cs, const std::vector<Variable>& scalar_bits,
                       const ECPoint& P, const std::string& label) {
    assert(scalar_bits.size() == 256);

    // Split scalar into k_lo (bits 0-127) and k_hi (bits 128-255)
    // scalar_bits[0] = LSB, scalar_bits[255] = MSB

    // Precompute Q = 2^128 * P (prover-side)
    WitnessPoint Pw = ec_witness(cs, P);
    WitnessPoint Qw = Pw;
    for (int i = 0; i < 128; ++i) Qw = witness_double(Qw);

    // Allocate Q in circuit
    ECPoint Q = ec_alloc(cs, Qw.x, Qw.y, false, label + "_Q128");

    // Precompute P + Q (for Shamir's trick when both bits are 1)
    ECPoint PQ = ec_add_unsafe(cs, P, Q, label + "_PQ");

    // Start accumulator at offset R
    const WitnessPoint& Rw = offset_point_R();
    ECPoint acc = ec_alloc(cs, Rw.x, Rw.y, false, label + "_R");

    // Shamir's trick: 128 iterations (MSB to LSB of each 128-bit half)
    for (int i = 127; i >= 0; --i) {
        // Double
        acc = ec_double(cs, acc, label + "_d" + std::to_string(i));

        // Bits: b1 = k_lo bit i, b2 = k_hi bit i
        Variable b1 = scalar_bits[i];       // low half
        Variable b2 = scalar_bits[128 + i]; // high half

        // Select point to add based on (b1, b2):
        //   (0,0) → dummy (P, will be discarded)
        //   (1,0) → P
        //   (0,1) → Q
        //   (1,1) → PQ
        ECPoint inner = ec_select(cs, b1, PQ, Q, label + "_mi" + std::to_string(i));
        ECPoint selected = ec_select(cs, b2, inner, P, label + "_ms" + std::to_string(i));

        // Always compute the addition (fixed-shape)
        ECPoint sum = ec_add_unsafe(cs, acc, selected, label + "_a" + std::to_string(i));

        // Should we use the sum? Only if b1 OR b2 is set.
        Variable should_add = gadgets::or_bits(cs, b1, b2, label + "_or" + std::to_string(i));
        acc = ec_select(cs, should_add, sum, acc, label + "_s" + std::to_string(i));
    }

    // Subtract offset: result = acc - 2^128 * R
    const WitnessPoint& neg_off = neg_offset_2_128_R();
    ECPoint neg_offset_pt = ec_alloc(cs, neg_off.x, neg_off.y, false, label + "_noff");
    ECPoint result = ec_add_unsafe(cs, acc, neg_offset_pt, label + "_sub");

    return result;
}

// ---------------------------------------------------------------------------
// Fixed-base scalar multiplication: k * G (windowed)
// ---------------------------------------------------------------------------

// Precomputed table for fixed-base scalar mul against an arbitrary base B.
// table[i][j] = j * 256^i * B for i=0..31, j=0..255
// Computed once per distinct base, cached.
//
// Generalized from the original G-only table so the shielded cv-binding
// circuit can multiply by the Pedersen *value* generator V as well as the
// standard generator G. The standard-G instance is exposed via
// fixed_base_table()/ec_scalar_mul_gen() and is byte-for-byte identical to
// the previous G-only table (so taproot/Schnorr circuits are unaffected).
struct FixedBaseTable {
    static constexpr int WINDOW_BITS = 8;
    static constexpr int WINDOW_COUNT = 32;
    static constexpr int WINDOW_SIZE = 1 << WINDOW_BITS;

    WitnessPoint entries[WINDOW_COUNT][WINDOW_SIZE];

    explicit FixedBaseTable(const WitnessPoint& base_point) {
        // Compute base_i = 256^i * base for each window
        WitnessPoint base = base_point;
        for (int i = 0; i < WINDOW_COUNT; ++i) {
            entries[i][0] = {Uint256(), Uint256(), true}; // 0 * base = identity
            entries[i][1] = base; // 1 * base
            for (int j = 2; j < WINDOW_SIZE; ++j) {
                entries[i][j] = witness_add(entries[i][j - 1], base);
            }
            // Advance base: base *= 256 (8 doublings)
            for (int d = 0; d < WINDOW_BITS; ++d) {
                base = witness_double(base);
            }
        }
    }
};

static const FixedBaseTable& fixed_base_table() {
    static FixedBaseTable table(WitnessPoint{secp256k1_Gx(), secp256k1_Gy(), false});
    return table;
}

// Shared accumulation core: windowed fixed-base multiply against `table`.
// Used by both the standard-generator path (ec_scalar_mul_gen) and the
// arbitrary-base path (ec_scalar_mul_fixed). The constraint *structure* is
// fixed by which scalar-bit Variables are the const-zero variable (those
// windows are skipped), NOT by the witness values — so prover and verifier
// emit an identical R1CS as long as they pass the same bit Variables.
// `identity_safe` selects the accumulation strategy:
//   - false (the standard-generator path, ec_scalar_mul_gen): the original
//     fast guarded ec_add_unsafe accumulation. Sound ONLY when the running
//     accumulator is never the identity — true for the random/hash-derived
//     scalars Schnorr/taproot use, where the lowest window byte is non-zero.
//     Kept byte-for-byte identical so existing verifying keys don't change.
//   - true (the arbitrary-base path, ec_scalar_mul_fixed): identity-safe
//     ec_add_complete accumulation. REQUIRED for structured scalars (e.g. a
//     note value like 100,000,000 = 0x05F5E100, whose low byte is 0x00 →
//     window 0 selects the identity → the running accumulator starts at O).
static ECPoint ec_scalar_mul_with_table(R1CS& cs,
                                         const std::vector<Variable>& scalar_bits,
                                         const FixedBaseTable& table,
                                         const std::string& label,
                                         bool identity_safe) {
    const Variable zero = cs.const_zero();

    // Process 32 windows of 8 bits each.
    // For window i: byte = bits[8i..8i+7], select table[i][byte]
    // Accumulate via additions

    ECPoint acc;
    bool acc_initialized = false;

    for (int win = 0; win < FixedBaseTable::WINDOW_COUNT; ++win) {
        std::string wl = label + "_w" + std::to_string(win);
        const int base_bit = win * FixedBaseTable::WINDOW_BITS;

        bool window_is_const_zero = true;
        for (int b = 0; b < FixedBaseTable::WINDOW_BITS; ++b) {
            if (scalar_bits[base_bit + b] != zero) {
                window_is_const_zero = false;
                break;
            }
        }
        if (window_is_const_zero) {
            continue;
        }

        // Extract 8-bit window value (as witness)
        uint16_t window_val = 0;
        for (int b = 0; b < FixedBaseTable::WINDOW_BITS; ++b) {
            Scalar bv = cs.get_value(scalar_bits[base_bit + b]);
            if (!bv.is_zero()) {
                window_val |= static_cast<uint16_t>(1u << b);
            }
        }

        // Select the table point using a 256-way mux over constant coordinates.
        // Build 256 indicator variables from the 8-bit value.
        std::vector<Variable> indicators(FixedBaseTable::WINDOW_SIZE);
        for (int j = 0; j < FixedBaseTable::WINDOW_SIZE; ++j) {
            const bool is_match = (j == static_cast<int>(window_val));
            indicators[j] = cs.alloc(is_match ? Scalar::one() : Scalar::zero());
            gadgets::enforce_boolean(cs, indicators[j], wl + "_ind" + std::to_string(j));
        }

        // Constrain: sum(indicators) = 1
        LinearCombination ind_sum;
        for (int j = 0; j < FixedBaseTable::WINDOW_SIZE; ++j) {
            ind_sum = ind_sum + LinearCombination(indicators[j]);
        }
        cs.enforce_equal(ind_sum, LinearCombination(Scalar::one(), VAR_ONE), wl + "_isum");

        // Constrain: indicators encode the correct 8-bit window value:
        // window = sum(j * indicators[j])
        LinearCombination window_lc;
        for (int j = 0; j < FixedBaseTable::WINDOW_SIZE; ++j) {
            window_lc = window_lc + LinearCombination(Scalar(uint64_t(j)), indicators[j]);
        }

        // window should equal bits[0] + 2*bits[1] + ... + 128*bits[7]
        LinearCombination bits_lc;
        Scalar pow2 = Scalar::one();
        for (int b = 0; b < FixedBaseTable::WINDOW_BITS; ++b) {
            bits_lc = bits_lc + LinearCombination(pow2, scalar_bits[base_bit + b]);
            pow2 = pow2 + pow2;
        }
        cs.enforce_equal(window_lc, bits_lc, wl + "_wbits");

        // Select x,y coordinates from table using indicators
        // For each limb: out = sum(indicators[j] * table[win][j].coord.limb)
        // Since table values are CONSTANTS, this is a linear combination.
        const WitnessPoint& selected = table.entries[win][window_val];
        FieldElement sel_x, sel_y;
        for (int l = 0; l < 4; ++l) {
            // x limb l: sum(indicators[j] * table[win][j].x.limbs[l])
            Scalar x_val = Scalar(selected.x.limbs[l]);
            sel_x.limbs[l] = cs.alloc(x_val);
            LinearCombination x_lc;
            for (int j = 0; j < FixedBaseTable::WINDOW_SIZE; ++j) {
                if (!table.entries[win][j].is_inf) {
                    x_lc = x_lc + LinearCombination(
                        Scalar(table.entries[win][j].x.limbs[l]), indicators[j]);
                }
            }
            cs.constrain(LinearCombination(sel_x.limbs[l]), LinearCombination(VAR_ONE),
                         x_lc, wl + "_xl" + std::to_string(l));

            // y limb l
            Scalar y_val = Scalar(selected.y.limbs[l]);
            sel_y.limbs[l] = cs.alloc(y_val);
            LinearCombination y_lc;
            for (int j = 0; j < FixedBaseTable::WINDOW_SIZE; ++j) {
                if (!table.entries[win][j].is_inf) {
                    y_lc = y_lc + LinearCombination(
                        Scalar(table.entries[win][j].y.limbs[l]), indicators[j]);
                }
            }
            cs.constrain(LinearCombination(sel_y.limbs[l]), LinearCombination(VAR_ONE),
                         y_lc, wl + "_yl" + std::to_string(l));
        }

        ECPoint selected_pt;
        selected_pt.x = sel_x;
        selected_pt.y = sel_y;
        selected_pt.is_identity = indicators[0];

        if (identity_safe) {
            // Identity-safe: ec_add_complete handles acc==O, selected==O, and
            // acc==selected. Structure is independent of witness values, so
            // prover and verifier emit identical R1CS.
            if (!acc_initialized) {
                acc = ec_identity(cs, wl + "_acc0");
                acc_initialized = true;
            }
            acc = ec_add_complete(cs, acc, selected_pt, wl + "_cadd");
            continue;
        }
        if (!acc_initialized) {
            acc = selected_pt;
            acc_initialized = true;
        } else {
            // Replace ec_add_complete (~11.5K constraints) with ec_add_unsafe +
            // two ec_select guards (~5.6K constraints), saving ~182K constraints
            // across 31 windows.
            //
            // When window_val == 0, selected_pt is the identity. ec_add_unsafe
            // cannot handle identity inputs (divides by zero in the slope formula).
            // Guard: substitute a known non-identity dummy point (G = table[0][1])
            // before calling ec_add_unsafe, then select the correct result:
            //   - is_identity == 1  →  keep acc unchanged
            //   - is_identity == 0  →  use the computed sum
            //
            // The dummy computation's result is discarded. The constraints are
            // satisfied because dummy_x ≠ acc.x with overwhelming probability
            // (acc is a random accumulation; dummy_x = Gx is a specific constant).
            const WitnessPoint& dummy_wp = table.entries[0][1]; // = G
            ECPoint dummy_pt;
            for (int l = 0; l < 4; ++l) {
                dummy_pt.x.limbs[l] = gadgets::constant(
                    cs, Scalar(dummy_wp.x.limbs[l]), wl + "_dx" + std::to_string(l));
                dummy_pt.y.limbs[l] = gadgets::constant(
                    cs, Scalar(dummy_wp.y.limbs[l]), wl + "_dy" + std::to_string(l));
            }
            dummy_pt.is_identity = cs.const_zero();

            // safe_pt = (is_identity ? dummy_pt : selected_pt)
            ECPoint safe_pt = ec_select(cs, selected_pt.is_identity,
                                        dummy_pt, selected_pt, wl + "_safe");
            // add = acc + safe_pt (always valid: both non-identity, acc ≠ safe_pt)
            ECPoint added = ec_add_unsafe(cs, acc, safe_pt, wl + "_add");
            // result = (is_identity ? acc : added)
            acc = ec_select(cs, selected_pt.is_identity, acc, added, wl + "_sel");
        }
    }

    return acc;
}

ECPoint ec_scalar_mul_gen(R1CS& cs, const std::vector<Variable>& scalar_bits,
                           const std::string& label) {
    assert(scalar_bits.size() == 256);
    // identity_safe=false: preserve the exact original constraint structure so
    // taproot/Schnorr verifying keys are unchanged.
    return ec_scalar_mul_with_table(cs, scalar_bits, fixed_base_table(), label,
                                    /*identity_safe=*/false);
}

ECPoint ec_scalar_mul_fixed(R1CS& cs, const std::vector<Variable>& scalar_bits,
                            const Uint256& base_x, const Uint256& base_y,
                            const std::string& label) {
    assert(scalar_bits.size() == 256);
    // Cache one windowed table per distinct base point. The base is a fixed
    // circuit constant (e.g. the Pedersen value generator V), so the table is
    // built at most once per process and reused across every proof/verify.
    static std::mutex cache_mu;
    static std::map<std::pair<Uint256, Uint256>, std::unique_ptr<FixedBaseTable>> cache;
    const FixedBaseTable* table = nullptr;
    {
        std::lock_guard<std::mutex> lk(cache_mu);
        const auto key = std::make_pair(base_x, base_y);
        auto it = cache.find(key);
        if (it == cache.end()) {
            it = cache.emplace(
                     key,
                     std::make_unique<FixedBaseTable>(
                         WitnessPoint{base_x, base_y, false}))
                     .first;
        }
        table = it->second.get();
    }
    // identity_safe=true: arbitrary bases are multiplied by structured scalars
    // (note values), which can drive the accumulator through the identity.
    return ec_scalar_mul_with_table(cs, scalar_bits, *table, label,
                                    /*identity_safe=*/true);
}

} // namespace zkvm
} // namespace zk
} // namespace dinero
