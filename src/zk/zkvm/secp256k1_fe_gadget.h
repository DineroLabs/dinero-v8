// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Non-Native Base Field Arithmetic for secp256k1 in R1CS
 *
 * The R1CS constraint system operates in the secp256k1 SCALAR field (mod n),
 * but EC point arithmetic uses the BASE field (mod p). Since p != n, base
 * field operations must be emulated using multi-limb arithmetic.
 *
 * Representation: 4 x 64-bit limbs (little-endian).
 *   value = limb[0] + limb[1]*2^64 + limb[2]*2^128 + limb[3]*2^192
 *
 * Each limb is a single R1CS Variable constrained to [0, 2^64).
 *
 * secp256k1 base field prime:
 *   p = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
 *     = 2^256 - 4294968273
 *
 * Cost estimates:
 *   fe_alloc (with range check):  260 constraints
 *   fe_add:                       ~270 constraints
 *   fe_sub:                       ~270 constraints
 *   fe_mul:                       ~1200 constraints
 *   fe_inv (witness + verify):    ~1500 constraints
 *
 * This is the foundation for EC point arithmetic gadgets used by the
 * BIP-340 Schnorr verification circuit (OP_CHECKSIG in ZK).
 */

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/gadgets.h"
#include <array>
#include <cstdint>

namespace dinero {
namespace zk {
namespace zkvm {

// secp256k1 base field prime p = 2^256 - 4294968273
// In 4 x 64-bit limbs (little-endian):
constexpr uint64_t SECP256K1_P_LIMB0 = 0xFFFFFFFEFFFFFC2Full;
constexpr uint64_t SECP256K1_P_LIMB1 = 0xFFFFFFFFFFFFFFFFull;
constexpr uint64_t SECP256K1_P_LIMB2 = 0xFFFFFFFFFFFFFFFFull;
constexpr uint64_t SECP256K1_P_LIMB3 = 0xFFFFFFFFFFFFFFFFull;

// p as a constant: 2^256 - C where C = 4294968273 = 0x1000003D1
constexpr uint64_t SECP256K1_P_LOW_DEFICIT = 0x1000003D1ull;

// 2^64 as a Scalar (precomputed for carry multiplier)
Scalar scalar_pow2_64();

// p limbs as Scalars (precomputed)
const std::array<Scalar, 4>& p_limb_scalars();

/**
 * Non-native base field element: 4 x 64-bit limbs in R1CS.
 *
 * Each limb is a Variable constrained to [0, 2^64) via boolean
 * decomposition (64 boolean constraints + 1 pack equality = 65/limb).
 */
struct FieldElement {
    std::array<Variable, 4> limbs;  // limbs[0] = low 64 bits
};

// ---------------------------------------------------------------------------
// 256-bit witness arithmetic (prover-side, outside the circuit)
// ---------------------------------------------------------------------------

struct Uint256 {
    uint64_t limbs[4] = {0, 0, 0, 0};  // little-endian

    Uint256() = default;
    explicit Uint256(uint64_t v) { limbs[0] = v; }
    explicit Uint256(const uint8_t bytes[32]); // big-endian bytes
    void to_bytes(uint8_t out[32]) const;      // big-endian bytes

    bool operator<(const Uint256& other) const;
    bool operator>=(const Uint256& other) const { return !(*this < other); }
    bool operator==(const Uint256& other) const;
    bool is_zero() const;
};

// 512-bit result of multiplication
struct Uint512 {
    uint64_t limbs[8] = {};
};

Uint256 uint256_add(const Uint256& a, const Uint256& b);
Uint256 uint256_sub(const Uint256& a, const Uint256& b); // assumes a >= b
Uint512 uint256_mul(const Uint256& a, const Uint256& b);
Uint256 uint256_mod_p(const Uint256& a);      // reduce mod p
Uint256 uint256_add_mod_p(const Uint256& a, const Uint256& b);
Uint256 uint256_sub_mod_p(const Uint256& a, const Uint256& b);
Uint256 uint256_mul_mod_p(const Uint256& a, const Uint256& b);
Uint256 uint256_inv_mod_p(const Uint256& a);   // a^{p-2} mod p

// Quotient and remainder: a*b = q*p + r
struct MulQR {
    Uint256 q;
    Uint256 r;
};
MulQR uint256_mul_divmod_p(const Uint256& a, const Uint256& b);

// The prime p as Uint256
const Uint256& uint256_p();

// ---------------------------------------------------------------------------
// Circuit gadgets: FieldElement operations (constrained mod p)
// ---------------------------------------------------------------------------

/**
 * Extract the prover-side Uint256 value of an allocated FieldElement.
 * Reads limb values from the R1CS witness — zero cost, no constraints.
 */
Uint256 fe_witness_value(R1CS& cs, const FieldElement& fe);

/**
 * Allocate a FieldElement from witness bytes (big-endian 32 bytes).
 * Range-checks all 4 limbs to [0, 2^64).
 * Does NOT check < p (caller must do fe_assert_less_than_p if needed).
 * Cost: 260 constraints.
 */
FieldElement fe_alloc(R1CS& cs, const uint8_t bytes[32],
                       const std::string& label = "fe");

/**
 * Allocate a FieldElement from a Uint256 witness value.
 * Cost: 260 constraints.
 */
FieldElement fe_alloc_uint256(R1CS& cs, const Uint256& val,
                               const std::string& label = "fe");

/**
 * Create a FieldElement from a known constant (no range check needed,
 * the limbs are allocated as constants with fixed values).
 * Cost: 4 constraints (constant allocation).
 */
FieldElement fe_constant(R1CS& cs, const Uint256& val,
                          const std::string& label = "fec");

/**
 * Addition mod p: result = (a + b) mod p.
 * Cost: ~270 constraints.
 */
FieldElement fe_add(R1CS& cs, const FieldElement& a, const FieldElement& b,
                     const std::string& label = "fea");

/**
 * Subtraction mod p: result = (a - b) mod p.
 * Cost: ~270 constraints.
 */
FieldElement fe_sub(R1CS& cs, const FieldElement& a, const FieldElement& b,
                     const std::string& label = "fes");

/**
 * Double subtraction mod p: result = (a - b - c) mod p.
 * Fuses two sequential fe_sub calls into one, eliminating the intermediate
 * allocation (saving ~260 constraints vs two separate fe_sub calls).
 * Cost: ~285 constraints.
 */
FieldElement fe_sub2(R1CS& cs, const FieldElement& a, const FieldElement& b,
                      const FieldElement& c, const std::string& label = "fes2");

/**
 * Multiply by difference mod p: result = a * (b - c) mod p.
 * Eliminates the intermediate (b-c) field element allocation by using limb-wise
 * differences in the partial products, with a borrow correction on the carry chain.
 * Cost: ~995 constraints (vs ~1270 for fe_sub + fe_mul).
 */
FieldElement fe_mul_diff(R1CS& cs, const FieldElement& a,
                          const FieldElement& b, const FieldElement& c,
                          const std::string& label = "femd");

/**
 * Inverse of difference mod p: result = (a - b)^{-1} mod p.
 * Eliminates the intermediate (a-b) field element allocation.
 * Cost: ~1260 constraints (vs ~1530 for fe_sub + fe_inv).
 */
FieldElement fe_inv_diff(R1CS& cs, const FieldElement& a, const FieldElement& b,
                          const std::string& label = "feid");

/**
 * Fused slope verification: assert lambda * (Qx - Px) ≡ (Qy - Py) mod p.
 *
 * Replaces the fe_inv_diff + fe_mul_diff pair in ec_add_unsafe with a single
 * carry chain that proves the slope equation directly against the existing
 * Qy/Py variables (no inverse allocation, no intermediate result allocation).
 *
 * The caller allocates lambda (via fe_alloc_uint256) from a natively computed
 * witness; this gadget only verifies the algebraic relation.
 *
 * Cost: ~733 constraints (vs ~2,251 for fe_inv_diff + fe_mul_diff).
 */
void fe_slope_verify(R1CS& cs,
                     const FieldElement& lambda,
                     const FieldElement& Qx, const FieldElement& Px,
                     const FieldElement& Qy, const FieldElement& Py,
                     const std::string& label = "fesv");

/**
 * Multiply-difference-subtract mod p: result = a*(b-c) - d mod p.
 * Fuses fe_mul_diff(a,b,c) + fe_sub(result, d) into one carry chain,
 * eliminating the intermediate lam_diff allocation (~279 constraints saved).
 * Used for both ec_double and ec_add: R.y = lambda*(Px - Rx) - Py.
 * Cost: ~716 constraints (vs ~1273 for fe_mul_diff + fe_sub).
 */
FieldElement fe_mul_diff_sub(R1CS& cs,
                              const FieldElement& a,
                              const FieldElement& b, const FieldElement& c,
                              const FieldElement& d,
                              const std::string& label = "femds");

/**
 * Multiplication mod p: result = (a * b) mod p.
 *
 * Uses quotient-remainder verification: prover computes q, r such that
 * a * b = q * p + r (over the integers). The circuit verifies this
 * equation limb-by-limb with carry propagation.
 *
 * Cost: ~1200 constraints.
 */
FieldElement fe_mul(R1CS& cs, const FieldElement& a, const FieldElement& b,
                     const std::string& label = "fem");

/**
 * Multiplicative inverse mod p: result = a^{-1} mod p.
 * Prover computes the inverse; circuit verifies a * result = 1 (mod p).
 * Cost: ~1500 constraints.
 */
FieldElement fe_inv(R1CS& cs, const FieldElement& a,
                     const std::string& label = "fei");

/**
 * Squaring mod p: result = a^2 mod p.
 * Slightly cheaper than general multiplication because a_i * a_j = a_j * a_i.
 * Cost: ~1000 constraints.
 */
FieldElement fe_square(R1CS& cs, const FieldElement& a,
                        const std::string& label = "feq");

/**
 * Fused 3*a^2 mod p without intermediate allocations.
 * Equivalent to fe_mul(a,a) followed by two fe_add, but skips the two
 * intermediate FieldElement allocations (~558 fewer constraints).
 * Cost: ~1,001 constraints (vs 1,559 for fe_square + 2*fe_add).
 */
FieldElement fe_square_triple(R1CS& cs, const FieldElement& a,
                               const std::string& label = "feqt");

/**
 * Fused a^2 - b - c mod p without allocating the intermediate a^2.
 * Fuses the squaring carry chain with two limb-level subtractions via two
 * boolean borrow variables. Used in ec_double and ec_add_unsafe to compute
 * lambda^2 - x1 - x2 in one pass.
 * Cost: ~993 constraints (vs fe_square + fe_sub2 ≈ 1,274; saves ~281).
 */
FieldElement fe_square_sub2(R1CS& cs, const FieldElement& a,
                             const FieldElement& b, const FieldElement& c,
                             const std::string& label = "feqs2");

/**
 * Fused inv(2*a) mod p without allocating 2*a.
 * Equivalent to fe_add(a,a) + fe_inv, but skips the (2*a) FieldElement
 * allocation (~545 fewer constraints).
 * Cost: ~991 constraints (vs 1,536 for fe_add + fe_inv).
 */
FieldElement fe_double_inv(R1CS& cs, const FieldElement& a,
                            const std::string& label = "fedi");

/**
 * Conditional select: result = cond ? a : b.
 * Cost: 4 constraints (one select per limb).
 */
FieldElement fe_select(R1CS& cs, Variable cond,
                        const FieldElement& a, const FieldElement& b,
                        const std::string& label = "fsl");

/**
 * Equality check: returns boolean Variable (1 if a == b, 0 otherwise).
 * Cost: ~20 constraints.
 */
Variable fe_equal(R1CS& cs, const FieldElement& a, const FieldElement& b,
                   const std::string& label = "feq");

/**
 * Assert a < p (range check for valid field element).
 * Cost: ~66 constraints.
 */
void fe_assert_less_than_p(R1CS& cs, const FieldElement& a,
                            const std::string& label = "felp");

/**
 * Pack a FieldElement into a single Scalar (for transcript/hashing).
 * Returns a Variable whose value = limb[0] + limb[1]*2^64 + limb[2]*2^128 + limb[3]*2^192.
 * WARNING: This is mod n (the scalar field), not mod p.
 * Cost: 1 constraint.
 */
Variable fe_pack(R1CS& cs, const FieldElement& a,
                  const std::string& label = "fep");

// ---------------------------------------------------------------------------
// Internal: limb range check (64-bit boolean decomposition)
// ---------------------------------------------------------------------------

/**
 * Range-check a variable to [0, 2^num_bits).
 * Allocates num_bits boolean variables and constrains pack == var.
 * Cost: num_bits + 1 constraints.
 */
void range_check_limb(R1CS& cs, Variable var, size_t num_bits,
                       const std::string& label = "rc");

/**
 * Range-check a signed carry variable.
 * The carry is shifted to unsigned: (carry + offset) in [0, 2^num_bits).
 * Returns the shifted variable.
 * Cost: num_bits + 2 constraints.
 */
Variable range_check_signed(R1CS& cs, Variable carry, const Scalar& offset,
                             size_t num_bits, const std::string& label = "rcs");

} // namespace zkvm
} // namespace zk
} // namespace dinero
