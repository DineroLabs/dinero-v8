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
 * cv is COMPUTED natively (outside any ZK circuit). The associated
 * Bulletproof range proof on cv proves `value ∈ [0, 2^64)` without
 * revealing value, and Wave 2's binding signature proves the bvk
 * equation `bvk = sum(cv_spend) - sum(cv_output) - value_balance·V`
 * — together those close the inflation hole.
 *
 * IMPORTANT: there is no per-cv "verify Pedersen commitment" check
 * available to consensus directly, because consensus does not have
 * `(blind, value)`. The cv binding to the proven value lives entirely
 * in the Bulletproof; the cv binding to the bundle balance lives in
 * the Schnorr binding signature. This header exposes commit /
 * serialize / parse / sum primitives — the helpers consensus and
 * wallet need, not a redundant verify-with-witness function.
 *
 * Storage: cv is stored as 33-byte compressed-pubkey form on the wire
 * (matches libsecp256k1 conventions). The `Hash cv` (32 bytes) field
 * on ShieldedSpend / ShieldedOutput holds the 32-byte x-coordinate;
 * the parity bit is recovered from the curve equation when consensus
 * needs the full point. Convenience helpers below return either form.
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
 * @param out_cv  Output: 32-byte x-coordinate of the resulting point.
 *                The implicit y-parity follows libsecp256k1's
 *                canonical compressed encoding.
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
