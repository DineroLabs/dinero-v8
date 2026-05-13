// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * BIP-340 Schnorr Signature Verification Gadget for R1CS
 *
 * Proves in zero knowledge that a BIP-340 Schnorr signature (s, R_x)
 * is valid for public key P_x and message m, without revealing any
 * of these values.
 *
 * BIP-340 verification:
 *   1. Lift R_x to point R = (R_x, even_y)
 *   2. Lift P_x to point P = (P_x, even_y)
 *   3. e = SHA256(SHA256("BIP0340/challenge") || SHA256("BIP0340/challenge")
 *                 || R_x || P_x || m)
 *   4. Check: s*G == R + e*P
 *
 * X-only point lifting enforces even-Y parity per BIP-340:
 *   y^2 = x^3 + 7 (mod p), select y with y mod 2 == 0.
 *
 * Cost breakdown:
 *   Challenge hash (3 SHA256 blocks):  ~108K constraints
 *   Y-recovery (2 points):            ~8K constraints
 *   s*G (fixed-base):                 ~834K constraints
 *   e*P (variable-base):              ~3.36M constraints
 *   R + e*P (addition):               ~6K constraints
 *   Equality check:                   ~40 constraints
 *   Total:                            ~4.3M constraints
 */

#include "zk/zkvm/ec_gadget.h"
#include "zk/zkvm/sha256_gadget.h"

namespace dinero {
namespace zk {
namespace zkvm {

/**
 * BIP-340 tagged hash: SHA256(tag || tag || msg)
 * where tag = SHA256("BIP0340/challenge").
 *
 * @param cs       Constraint system
 * @param R_x_bytes  32 Word32 values for R's x-coordinate (big-endian bytes)
 * @param P_x_bytes  32 Word32 values for P's x-coordinate (big-endian bytes)
 * @param msg_bytes  32 Word32 values for the message (big-endian bytes)
 * @return           8 Word32 values: the challenge hash e
 */
std::array<Word32, 8> bip340_challenge_hash(
    R1CS& cs,
    const std::vector<Word32>& R_x_bytes,
    const std::vector<Word32>& P_x_bytes,
    const std::vector<Word32>& msg_bytes,
    const std::string& label = "bip340ch");

/**
 * X-only point lift with even-Y enforcement (BIP-340).
 *
 * Given x-coordinate as a FieldElement, compute y such that:
 *   y^2 = x^3 + 7 (mod p)
 *   y is even (y mod 2 == 0)
 *
 * The prover computes y outside the circuit. The circuit constrains:
 *   1. y^2 == x^3 + 7 (on-curve)
 *   2. y's LSB == 0 (even parity)
 *
 * @return ECPoint with the lifted coordinates
 * Cost: ~3,200 constraints (3 fe_mul + parity check)
 */
ECPoint bip340_lift_x(R1CS& cs, const FieldElement& x,
                       const std::string& label = "lift");

/**
 * Convert 8 Word32 hash output to 256 boolean bit Variables (LSB first).
 * The Word32 array is big-endian (h[0] = most significant 32 bits).
 * Cost: 0 constraints (just reindexes existing boolean bits).
 */
std::vector<Variable> hash_to_scalar_bits(
    const std::array<Word32, 8>& hash);

/**
 * Convert a FieldElement (4 × 64-bit limbs) to 32 Word32 byte values
 * for SHA256 input. Output is big-endian (byte 0 = most significant).
 * Cost: ~260 constraints (bit decomposition of each limb).
 */
std::vector<Word32> fe_to_bytes(R1CS& cs, const FieldElement& fe,
                                 const std::string& label = "fe2b");

/**
 * Full BIP-340 Schnorr signature verification in R1CS.
 *
 * Verifies: s*G == R + e*P where e = tagged_hash(R_x || P_x || m).
 *
 * All inputs are allocated as circuit variables. The prover provides
 * witness values; the circuit constrains correctness.
 *
 * @param cs           Constraint system
 * @param s_bits       256 boolean Variables for scalar s (LSB first)
 * @param R_x          x-coordinate of R as FieldElement
 * @param P_x          x-coordinate of P as FieldElement
 * @param msg_bytes    32 Word32 values for the 32-byte message
 * @return             Boolean Variable: 1 if signature is valid
 *
 * Cost: ~4.3M constraints
 */
Variable bip340_verify(R1CS& cs,
                        const std::vector<Variable>& s_bits,
                        const FieldElement& R_x,
                        const FieldElement& P_x,
                        const std::vector<Word32>& msg_bytes,
                        const std::string& label = "bip340");

} // namespace zkvm
} // namespace zk
} // namespace dinero
