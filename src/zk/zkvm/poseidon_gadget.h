// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Poseidon-2 hash gadget over the secp256k1 scalar field.
 *
 * Poseidon2(a, b) → c, where a, b, c ∈ Fp (secp256k1 group order).
 *
 * Parameters (t=3 state, 128-bit security):
 *   Field:          secp256k1 group order n (gcd(5, n-1) = 1 → x^5 S-Box valid)
 *   State width:    t = 3  (capacity[0] = 0, rate[1] = a, rate[2] = b)
 *   S-Box:          x^5  (3 R1CS multiplications per element)
 *   Full rounds:    Rf = 8   (4 before partial, 4 after)
 *   Partial rounds: Rp = 56  (only element [0])
 *   Total rounds:   64
 *   MDS matrix:     M = [[2,1,1],[1,2,1],[1,1,2]]  (MDS for all p > 3)
 *   Round constants: SHA256("PoseidonC_secp256k1" || BE32(round) ||
 *                    BE32(element)), rehashed until a valid non-zero scalar
 *
 * Constraint cost:
 *   Full rounds:    8 rounds × 3 elements × 3 muls = 72 R1CS mul constraints
 *   Partial rounds: 56 rounds × 1 element  × 3 muls = 168 R1CS mul constraints
 *   Total:          240 R1CS multiplication constraints
 *   (MDS matrix multiplications and round constant additions are free — linear)
 *
 * Usage: the collision-resistant hash inside the shielded circuit. It hashes
 *   Merkle authentication paths, the nullifier-key commitment, the ownership
 *   and address bindings, and the covenant policy terms. See
 *   consensus/shielded/shielded_circuit.cpp for the call sites. Using a hash
 *   here replaces an in-circuit variable-base EC scalar multiplication costing
 *   roughly 3.36M constraints with 240.
 *
 * Security: collision resistance, and only that. Two distinct preimages cannot
 *           share an output, so a commitment produced by this gadget identifies
 *           its inputs.
 *
 *           This gadget does NOT bind the circuit's witness to the public
 *           inputs a verifier is presented with. That property is enforced
 *           elsewhere, by the Spartan verifier reconstructing the public-input
 *           contribution from its own known values rather than trusting the
 *           prover's commitment (see r1cs_spartan.cpp, gated on
 *           shielded_input_binding_activation_height and
 *           shielded_cv_binding_activation_height). Do not rely on this gadget
 *           for witness binding.
 *
 *           Historical note: an earlier revision of this comment credited CLSAG
 *           with binding the witness key to a ring signer's key. That was
 *           accurate when written, but the legacy ring and confidential stack
 *           was removed in April 2026 and no CLSAG message exists any more.
 */

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/scalar.h"
#include <array>
#include <cstdint>

namespace dinero {
namespace zk {
namespace zkvm {

// ---------------------------------------------------------------------------
// Native evaluation (for witness generation and commitment computation)
// ---------------------------------------------------------------------------

/**
 * Evaluate Poseidon-2 hash natively over the secp256k1 scalar field.
 *
 * Computes: output = Poseidon(a, b)
 *
 * This is the native evaluator used by the prover to compute the commitment
 * value before building the ZK proof.
 */
Scalar poseidon2_native(const Scalar& a, const Scalar& b);

// ---------------------------------------------------------------------------
// Convenience wrapper: bytes → bytes (for integration with Scalar32)
// ---------------------------------------------------------------------------

/**
 * Compute Poseidon-2 commitment from 32-byte scalars.
 *
 * Returns 32-byte big-endian serialization of Poseidon(a, b).
 * Used in signing: commitment = poseidon2_bytes(privkey, nonce).
 */
std::array<uint8_t, 32> poseidon2_bytes(const std::array<uint8_t, 32>& a,
                                         const std::array<uint8_t, 32>& b);

// ---------------------------------------------------------------------------
// R1CS gadget (for circuit constraint generation)
// ---------------------------------------------------------------------------

/**
 * Add Poseidon-2 constraints to the R1CS.
 *
 * Allocates intermediate state variables and adds:
 *   - 72 constraints for 8 full rounds (x^5 applied to all 3 state elements)
 *   - 168 constraints for 56 partial rounds (x^5 applied to element [0] only)
 *   - Linear (zero-cost) MDS matrix applications and round constant additions
 *
 * @param cs      The R1CS constraint system to append to
 * @param a       Variable holding the first input (private witness or public input)
 * @param b       Variable holding the second input (private witness or public input)
 * @param label   Debug label prefix for all allocated variables
 * @return        Variable representing the Poseidon output (single field element)
 */
Variable poseidon2_gadget(R1CS& cs, Variable a, Variable b,
                           const std::string& label = "poseidon2");

} // namespace zkvm
} // namespace zk
} // namespace dinero
