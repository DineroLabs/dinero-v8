#pragma once
/**
 * Phase 3 wave 2 (Path C) — Schnorr binding signature.
 *
 * Closes the **cross-bundle inflation hole**: without this, the
 * sender could publish (spend value=10, output value=10000,
 * value_balance=-9990) and consensus would accept it because
 * `bundle.value_balance` is plaintext on the wire.
 *
 * Mechanism (Sapling-shape, adapted to secp256k1 + BIP340):
 *
 *   - Each spend / output carries a Pedersen value commitment
 *     cv = blind · G + value · V where G is the standard secp256k1
 *     generator and V is the DST-derived generator from
 *     `pedersen_generators.h`.
 *
 *   - Sender chooses blinds such that
 *       bsk = sum(blind_spend) - sum(blind_output)  (mod q)
 *     is a known scalar. The corresponding public key is
 *       bvk = bsk · G
 *           = sum(cv_spend) - sum(cv_output) - value_balance · V
 *     (the V-component cancels when value_balance matches the
 *     committed value flow; the G-component is bvk).
 *
 *   - Sender signs `binding_sighash` with bsk under BIP340 Schnorr.
 *     binding_sighash domain-separates over:
 *       "DIN/v7/shielded/binding/v1"
 *         || value_balance_LE_8bytes
 *         || tx_sighash_32bytes      // transparent envelope BIP143 sighash
 *         || canonical-sorted cv_spend_x's
 *         || canonical-sorted cv_output_x's
 *
 *   - Consensus reconstructs bvk and verifies the signature. Any
 *     mismatch — mutated value_balance, swapped cv, mutated tx
 *     envelope — produces a different bvk or a different sighash,
 *     and verification fails. There is no way to forge bsk given
 *     only the public bundle.
 *
 * The cv → range proof binding (Wave 1B-B) closes the negative-value
 * attack; this binding sig closes the inflation attack. Together
 * they're sufficient supply-integrity guards that the activation
 * gate can finally be lowered.
 *
 * Wrap-attack protection: `tx_sighash` is the BIP143 sighash of the
 * transparent envelope this bundle rides in. A miner who lifts the
 * bundle into a different transparent envelope produces a different
 * sighash, breaks binding_sighash, and binding-sig verify rejects.
 * Caller (block_validation.cpp) MUST supply the real tx_sighash;
 * passing zero defeats the wrap-attack check (regtest-only).
 */

#include "consensus/shielded/shielded_tx.h"

#include <cstdint>

namespace dinero::consensus::shielded {

enum class BindingSigResult : uint8_t {
    Ok                  = 0,
    GeneratorNotReady   = 1,
    CommitmentInvalid   = 2,   ///< failed to parse a cv into a libsecp pubkey
    BvkAtInfinity       = 3,   ///< reconstructed bvk is the identity (sum is zero)
    SignatureMalformed  = 4,
    SignatureInvalid    = 5,
};

/// Compute the canonical binding sighash per the spec memo §4 #3.
/// Includes value_balance, tx_sighash, and every cv (in canonical
/// sort order — same order serialization writes them).
Hash ComputeBindingSighash(const ShieldedBundle& bundle,
                           const Hash& tx_sighash);

/**
 * Phase 3 wave 3: compute the transparent-envelope sighash that
 * a shielded bundle binds against. Hashes everything OUTSIDE the
 * shielded bundle (prevouts, sequences, outputs, locktime, version)
 * so any miner who lifts the bundle into a different transparent
 * tx produces a different sighash → binding sig fails.
 *
 * Domain-separated hash (independent of BIP143 input-sighash):
 *   sighash = SHA-256(
 *       "DIN/v7/shielded/tx-sighash/v1"
 *       || version_LE_4bytes
 *       || prevouts_hash         (BIP143 GetPrevoutsHash)
 *       || sequence_hash         (BIP143 GetSequenceHash)
 *       || outputs_hash          (BIP143 GetOutputsHash)
 *       || locktime_LE_4bytes)
 */
struct Transaction;  // forward declared via primitives below
}  // namespace dinero::consensus::shielded

#include "primitives/transaction.h"

namespace dinero::consensus::shielded {

Hash ComputeShieldedTxSighash(const ::dinero::Transaction& tx);

/**
 * Compute the canonical bvk_commitment for a sender's bsk:
 *   bvk_commitment = pedersen_commit(blind=bsk, value=0, gen=V)
 *                  = bsk · G + 0 · V
 *                  = bsk · G
 * Sender publishes this on the wire alongside binding_sig. Returned
 * in pedersen 33-byte format (prefix 0x08 / 0x09 + x-coord).
 */
BindingSigResult ComputeBvkCommitment(const Hash& bsk,
                                      ValueCommitment& out_bvk);

/**
 * Sign the binding sighash using bsk. Used by the wallet at bundle
 * construction time. `bsk` is sum(blind_spend) - sum(blind_output)
 * (mod q) — caller computes via libsecp `pedersen_blind_sum` with
 * appropriate sign vector.
 *
 * On success, `out_sig` is the 64-byte BIP340 signature. Sender
 * MUST also publish `bvk_commitment` for the bundle to verify.
 */
BindingSigResult SignBinding(const Hash& bsk,
                             const Hash& binding_sighash,
                             BindingSignature& out_sig);

/**
 * Verify the binding sig against the bvk reconstructed from the
 * bundle. Consensus calls this. Returns Ok iff the signature is
 * cryptographically valid AND every intermediate step (cv parse,
 * bvk reconstruction) succeeded.
 */
BindingSigResult VerifyBinding(const ShieldedBundle& bundle,
                               const Hash& tx_sighash);

}  // namespace dinero::consensus::shielded
