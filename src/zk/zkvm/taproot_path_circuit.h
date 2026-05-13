// Copyright (c) 2026 Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

/**
 * Single-leaf Taproot path circuit helpers.
 *
 * This is the first cryptographic building block for the final hidden-member
 * Taproot binding. It proves the canonical BIP341 TapTweak hash for:
 *
 *   TapTweak(internal_xonly || tapleaf_hash)
 *
 * where `internal_xonly` is prover witness data and `tapleaf_hash` is a public
 * 32-byte TapLeaf hash already bound by the ring-covenant message.
 *
 * This module intentionally stops at the tagged-hash step. Point reconstruction
 * and same-member linkage are separate proof layers.
 */

#include "zk/zkvm/r1cs.h"
#include "zk/zkvm/secp256k1_fe_gadget.h"
#include "zk/zkvm/sha256_gadget.h"
#include <array>

namespace dinero {
namespace zk {
namespace zkvm {

struct SingleLeafTapTweakWitness {
    std::array<uint8_t, 32> internal_xonly{};
    std::array<uint8_t, 32> tapleaf_hash{};
};

/**
 * Build the single-leaf TapTweak tagged-hash circuit.
 *
 * Returns the resulting 32-byte tweak hash as 8 SHA256 output words.
 */
std::array<Word32, 8> single_leaf_taptweak_circuit(
    R1CS& cs,
    const SingleLeafTapTweakWitness& witness
);

/**
 * Build the same TapTweak circuit from an already-allocated internal x-only
 * field element.
 *
 * This is the shared-witness form used by the hidden-member binding work: the
 * TapTweak hash and the EC lift/tweak-add path can consume the same internal
 * key variables instead of allocating unrelated duplicate witnesses. The
 * tapleaf hash is allocated as witness bytes in this form so callers can safely
 * reuse it after witness variables already exist in the constraint system.
 */
std::array<Word32, 8> single_leaf_taptweak_circuit_from_field_element(
    R1CS& cs,
    const FieldElement& internal_xonly,
    const std::array<uint8_t, 32>& tapleaf_hash
);

/**
 * Build the same TapTweak circuit but bind the TapLeaf hash as statement
 * constants in the circuit shape rather than as witness bytes.
 *
 * This is used by the standalone hidden-member anonymous proof so the verifier
 * can reconstruct a statement-specific circuit hash without knowing any hidden
 * witness values.
 */
std::array<Word32, 8> single_leaf_taptweak_circuit_from_field_element_with_constant_tapleaf(
    R1CS& cs,
    const FieldElement& internal_xonly,
    const std::array<uint8_t, 32>& tapleaf_hash
);

/**
 * Constrain a TapTweak hash output against the expected 32-byte digest.
 */
void single_leaf_taptweak_constrain_expected_hash(
    R1CS& cs,
    const std::array<Word32, 8>& tweak_words,
    const std::array<uint8_t, 32>& expected_tweak_hash,
    const std::string& label = "taptweak"
);

/**
 * Full verification circuit for the single-leaf TapTweak hash.
 *
 * `tapleaf_hash` and `expected_tweak_hash` are treated as public inputs, while
 * `internal_xonly` remains prover witness data.
 */
bool single_leaf_taptweak_verify_circuit(
    R1CS& cs,
    const SingleLeafTapTweakWitness& witness,
    const std::array<uint8_t, 32>& expected_tweak_hash
);

/**
 * Native verifier-side helper for the same single-leaf TapTweak relation.
 */
bool single_leaf_taptweak_verify_native(
    const SingleLeafTapTweakWitness& witness,
    const std::array<uint8_t, 32>& expected_tweak_hash
);

// ============================================================
// Multi-leaf Taproot path circuit (bounded depth)
// ============================================================

constexpr size_t MAX_TAPROOT_DEPTH = 8;

/**
 * Witness for the multi-leaf Taproot path proof.
 * All fields are private (not revealed on-chain).
 * path_len is passed separately as a public parameter.
 */
struct TaprootPathTweakWitness {
    std::array<uint8_t, 32> internal_xonly{};   // spending key's x-only pubkey (hidden)
    std::array<uint8_t, 32> tapleaf_hash{};      // specific leaf hash (public — bound by covenant msg)
    // merkle_path[i] = sibling hash at level i (level 0 = adjacent to leaf)
    std::array<std::array<uint8_t, 32>, MAX_TAPROOT_DEPTH> merkle_path{};
    // sort_bits[i] = true if current_node <= sibling[i] at level i (i.e., node goes on left)
    std::array<bool, MAX_TAPROOT_DEPTH> sort_bits{};
};

/**
 * Build the multi-leaf TapTweak circuit.
 * Applies path_len TapBranch steps from tapleaf_hash, then TapTweak(internal_key || root).
 * tapleaf_hash is bound as a statement constant.
 * path_len is public (0..MAX_TAPROOT_DEPTH).
 * Returns 8 SHA256 output words (the TapTweak hash).
 */
std::array<Word32, 8> taproot_path_tweak_circuit_with_constant_tapleaf(
    R1CS& cs,
    const FieldElement& internal_xonly,
    const TaprootPathTweakWitness& witness,
    size_t path_len
);

/**
 * Full multi-leaf circuit (allocates internal_xonly as private witness internally).
 */
std::array<Word32, 8> taproot_path_tweak_circuit(
    R1CS& cs,
    const TaprootPathTweakWitness& witness,
    size_t path_len
);

/**
 * Native oracle for the multi-leaf TapTweak relation.
 */
bool taproot_path_tweak_verify_native(
    const TaprootPathTweakWitness& witness,
    size_t path_len,
    const std::array<uint8_t, 32>& expected_tweak_hash
);

/**
 * Full verify circuit for multi-leaf TapTweak.
 * tapleaf_hash and expected_tweak_hash are public inputs; everything else is private.
 */
bool taproot_path_tweak_verify_circuit(
    R1CS& cs,
    const TaprootPathTweakWitness& witness,
    size_t path_len,
    const std::array<uint8_t, 32>& expected_tweak_hash
);

} // namespace zkvm
} // namespace zk
} // namespace dinero
