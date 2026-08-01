#pragma once
/**
 * Phase 3 wave 1B (Path C) — native Pedersen value commitments.
 *
 * Pedersen commitment scheme:
 *   cv = blind · G + value · V
 *
 * where:
 *   - G is the standard secp256k1 generator (libsecp256k1's `secp256k1_GE_CONST_GEN`)
 *   - V is the DST-derived generator (`PedersenGeneratorV()`)
 *   - blind is a 32-byte secret scalar (witness-private)
 *   - value is the una-denominated amount (witness-private)
 *   - cv is published on the wire as 33-byte compressed form
 *
 * cv is COMPUTED natively and, after cv-binding activation, the Spartan
 * spend/output circuit also constrains the published point to the same
 * `(value, blind)` witness. A per-cv Borromean range proof proves
 * `value ∈ [0, 2^64)`, and the binding signature proves:
 *
 *   bvk = sum(cv_spend) - sum(cv_output) + value_balance·V
 *
 * where value_balance = sum(output_value) - sum(spend_value).
 *
 * Consensus does not learn `(blind, value)`. It verifies their relationship
 * to cv through the cv-bound Spartan circuit, verifies the value range through
 * the per-cv range proof, and verifies bundle conservation through the
 * Pedersen tally plus BIP340 binding signature.
 *
 * Storage: cv is stored as libsecp256k1-zkp's 33-byte Pedersen commitment
 * encoding (prefix 0x08/0x09 plus x-coordinate). The library-specific sign
 * prefix is consensus-critical and is never discarded.
 */

#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_tx.h"     // ValueCommitment

#include <secp256k1_rangeproof.h>  // secp256k1_pedersen_commitment

#include <array>
#include <cstdint>

namespace dinero::consensus::shielded {

/// Status of a Pedersen helper call.
enum class PedersenResult : uint8_t {
    Ok                 = 0,
    GeneratorNotReady  = 1,
    LibsecpFailure     = 2,
    InvalidEncoding    = 3,
};

/**
 * Compute `cv = blind · G + value · V`.
 *
 * @param blind   32-byte secret scalar (must be < curve order). Caller
 *                supplies fresh randomness; this function does NOT
 *                generate or store the blind.
 * @param value   uint64 una amount.
 * @param out_cv  Output: 33-byte Pedersen commitment encoding, including
 *                the 0x08/0x09 library sign prefix.
 *
 * Returns PedersenResult::Ok on success. The blind MUST be uniformly
 * random; reusing a blind across two cvs leaks the difference in
 * value via subtraction. Wallets are responsible for blind generation.
 */
PedersenResult PedersenCommit(const Hash& blind, uint64_t value,
                              ValueCommitment& out_cv);

/**
 * Serialize the libsecp `secp256k1_pedersen_commitment` to 33-byte
 * compressed form. Used by consensus paths that want the full point
 * for sum operations.
 */
PedersenResult PedersenCommitSerialize(const secp256k1_pedersen_commitment& commit,
                                       std::array<uint8_t, 33>& out33);

}  // namespace dinero::consensus::shielded
