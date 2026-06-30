// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * EC Point Arithmetic Gadgets for secp256k1 in R1CS
 *
 * Implements elliptic curve point operations as R1CS constraints using
 * non-native base field arithmetic (secp256k1_fe_gadget). These gadgets
 * are the building blocks for the BIP-340 Schnorr verification circuit.
 *
 * Curve: secp256k1 (y^2 = x^3 + 7 over GF(p))
 *
 * All points use affine coordinates (x, y) with an explicit is_identity
 * flag for the point at infinity. This avoids projective coordinate
 * overhead and keeps the constraint structure simple.
 *
 * Cost estimates:
 *   ec_assert_on_curve:  ~3,200 constraints
 *   ec_add (incomplete): ~4,500 constraints
 *   ec_double:           ~5,200 constraints
 *   ec_add_complete:     ~10,000 constraints (handles all edge cases)
 */

#include "zk/zkvm/secp256k1_fe_gadget.h"

namespace dinero {
namespace zk {
namespace zkvm {

/**
 * Affine EC point on secp256k1.
 *
 * The is_identity flag handles the point at infinity. When is_identity=1,
 * x and y values are ignored (set to 0 by convention).
 */
struct ECPoint {
    FieldElement x;
    FieldElement y;
    Variable is_identity;  // boolean: 1 = point at infinity
};

// secp256k1 generator point G coordinates
const Uint256& secp256k1_Gx();
const Uint256& secp256k1_Gy();

/**
 * Allocate an EC point from witness coordinates.
 * Does NOT check on-curve (caller must do ec_assert_on_curve if needed).
 * Cost: ~520 constraints (2 field element allocations).
 */
ECPoint ec_alloc(R1CS& cs, const Uint256& x, const Uint256& y,
                  bool is_inf, const std::string& label = "pt");

/**
 * Create the generator point G as circuit constants.
 * Cost: ~8 constraints (constant allocations).
 */
ECPoint ec_generator(R1CS& cs, const std::string& label = "G");

/**
 * Create the identity point (point at infinity).
 * Cost: ~9 constraints.
 */
ECPoint ec_identity(R1CS& cs, const std::string& label = "O");

/**
 * Assert that a point lies on secp256k1: y^2 = x^3 + 7 (mod p).
 * Skipped if is_identity = 1.
 * Cost: ~3,200 constraints (3 fe_mul + 1 fe_add + equality check).
 */
void ec_assert_on_curve(R1CS& cs, const ECPoint& P,
                         const std::string& label = "oc");

/**
 * Point addition: R = P + Q (incomplete formula).
 *
 * PRECONDITION: P != Q, P != -Q, neither is identity.
 * Use ec_add_complete for the general case.
 *
 * lambda = (Q.y - P.y) / (Q.x - P.x)
 * R.x = lambda^2 - P.x - Q.x
 * R.y = lambda(P.x - R.x) - P.y
 *
 * Cost: ~4,500 constraints.
 */
ECPoint ec_add_unsafe(R1CS& cs, const ECPoint& P, const ECPoint& Q,
                       const std::string& label = "eadd");

/**
 * Point doubling: R = 2*P.
 *
 * PRECONDITION: P is not identity, P.y != 0.
 *
 * lambda = 3*P.x^2 / (2*P.y)    (a=0 for secp256k1)
 * R.x = lambda^2 - 2*P.x
 * R.y = lambda(P.x - R.x) - P.y
 *
 * Cost: ~5,200 constraints.
 */
ECPoint ec_double(R1CS& cs, const ECPoint& P,
                   const std::string& label = "edbl");

/**
 * Complete point addition: R = P + Q, handles all cases.
 *
 * Handles: P=O, Q=O, P=Q (doubling), P=-Q (result=O).
 * Uses conditional select to branch based on these cases.
 *
 * Cost: ~10,000 constraints.
 */
ECPoint ec_add_complete(R1CS& cs, const ECPoint& P, const ECPoint& Q,
                         const std::string& label = "eac");

/**
 * Conditional point select: result = cond ? P : Q.
 * Cost: 9 constraints (4 per coordinate limb + 1 for identity flag).
 */
ECPoint ec_select(R1CS& cs, Variable cond,
                   const ECPoint& P, const ECPoint& Q,
                   const std::string& label = "esel");

/**
 * Point negation: R = -P = (P.x, p - P.y).
 * Cost: ~281 constraints (1 fe_sub).
 */
ECPoint ec_negate(R1CS& cs, const ECPoint& P,
                   const std::string& label = "eneg");

/**
 * Point equality check: returns boolean (1 if P == Q).
 * Cost: ~40 constraints.
 */
Variable ec_equal(R1CS& cs, const ECPoint& P, const ECPoint& Q,
                   const std::string& label = "eeq");

// ---------------------------------------------------------------------------
// Scalar multiplication
// ---------------------------------------------------------------------------

/**
 * Variable-base scalar multiplication: result = k * P.
 *
 * Uses double-and-add with an offset point to avoid identity/edge cases.
 * The scalar k is provided as 256 boolean bit Variables (bit 0 = LSB).
 *
 * Cost: ~256 * (ec_double + ec_add_unsafe + ec_select) + 1 ec_add_unsafe
 *     ≈ 256 * 13,092 + 5,907 ≈ 3.36M constraints.
 */
ECPoint ec_scalar_mul(R1CS& cs, const std::vector<Variable>& scalar_bits,
                       const ECPoint& P, const std::string& label = "esm");

/**
 * Fixed-base scalar multiplication: result = k * G (generator).
 *
 * Uses an 8-bit windowed lookup table with 32 windows of 256 precomputed
 * constant points. No doublings are needed in the accumulator — table entries
 * are pre-scaled by powers of 256. Windows whose bits are statically-zero
 * constants are skipped entirely, which matters for padded low-width scalars.
 *
 * Cost: dominated by 31 complete additions plus byte-selection constraints,
 * substantially lower than the older 64-window / 63-add form.
 */
ECPoint ec_scalar_mul_gen(R1CS& cs, const std::vector<Variable>& scalar_bits,
                           const std::string& label = "esmg");

/**
 * Fixed-base scalar multiplication against an ARBITRARY constant base B:
 * result = k * B, where B = (base_x, base_y) is an affine, non-identity
 * secp256k1 point supplied as a circuit constant.
 *
 * Uses the same 8-bit windowed table machinery as ec_scalar_mul_gen, but
 * builds (and process-caches) a precomputed table for the given base. Pass
 * the scalar as 256 boolean bit Variables (bit 0 = LSB); windows whose bits
 * are all the const-zero Variable are skipped, so a low-width scalar padded
 * with cs.const_zero() in its high bits costs only the active windows.
 *
 * Introduced for the shielded cv-binding circuit, which multiplies the
 * 64-bit note value by the Pedersen value generator V.
 */
ECPoint ec_scalar_mul_fixed(R1CS& cs, const std::vector<Variable>& scalar_bits,
                            const Uint256& base_x, const Uint256& base_y,
                            const std::string& label = "esmf");

/**
 * Extract witness x-coordinate from an ECPoint (for testing).
 */
Uint256 ec_witness_x(R1CS& cs, const ECPoint& P);
Uint256 ec_witness_y(R1CS& cs, const ECPoint& P);
bool ec_witness_is_inf(R1CS& cs, const ECPoint& P);

} // namespace zkvm
} // namespace zk
} // namespace dinero
