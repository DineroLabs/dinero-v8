#pragma once
/**
 * Phase 3 wave 1B (Path C) — per-cv range-proof construction & verification.
 *
 * Closes the **negative-value attack**: without this, a sender could
 * publish a cv that opens to a negative or > 2^64 value, smuggling
 * supply through the binding-sig equation in Wave 2.
 *
 * Per-cv design (NOT bundle-level Bulletproofs aggregation):
 *
 *   libsecp256k1-zkp's bppp module currently only exposes generator
 *   management — the actual aggregated range-proof verifier is a TODO
 *   in upstream. We use the older `secp256k1_rangeproof_*` Borromean-
 *   shape proof per cv, which provides the same security property
 *   (proves v ∈ [0, 2^64)) at higher byte-cost (~5 KB per proof vs.
 *   ~700 bytes for an aggregated BP).
 *
 * Despite the historical field name, this encoding is NOT safely
 * reinterpret-able. A future aggregated proof changes consensus semantics and
 * MUST use an explicit protocol version/encoding plus coordinated activation.
 *
 * Wire format of `aggregated_range_proof`:
 *
 *   varint N                                    // must equal spends + outputs
 *   for i in [0, N):
 *     varint plen_i
 *     plen_i bytes proof_i
 *
 * Proofs appear in canonical bundle order: sorted spend cvs first
 * (nullifier-ascending), then sorted output cvs (commitment-ascending).
 *
 * Consensus invariant: every cv MUST have a matching range proof. An
 * empty bundle short-circuits before this check fires. Any other shape
 * (count mismatch, parse error, verify failure) fails closed.
 */

#include "consensus/shielded/shielded_tx.h"

#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded {

enum class RangeProofResult : uint8_t {
    Ok                = 0,
    GeneratorNotReady = 1,
    ParseError        = 2,
    CountMismatch     = 3,   ///< number of proofs ≠ spends + outputs
    CommitmentInvalid = 4,   ///< failed to parse a cv into a Pedersen commitment
    VerifyFailed      = 5,   ///< rangeproof_verify returned 0 for some cv
};

/**
 * Verify every per-cv range proof in `bundle.aggregated_range_proof`
 * against its corresponding cv. Caller must already have run the
 * structural and ordering checks in `DeserializeShieldedBundle`.
 *
 * Pure function; no state mutation. Cost is O(num_cvs · ~5ms) on
 * Mac M-series.
 */
RangeProofResult VerifyBundleRangeProofs(const ShieldedBundle& bundle);

// ── Wallet/test helpers ──────────────────────────────────────────────

/**
 * Build a range proof for a single (blind, value, cv) triple.
 * `blind` is the same scalar used to construct cv via PedersenCommit.
 * `nonce` is a 32-byte fresh-randomness scalar (rangeproof_sign
 * needs it; can be derived deterministically from the witness).
 *
 * On success, `out_proof` is filled with the canonical proof bytes.
 * Returns RangeProofResult::Ok or a failure code.
 */
RangeProofResult SignRangeProof(const Hash& blind,
                                const Hash& nonce,
                                uint64_t value,
                                std::vector<uint8_t>& out_proof);

/**
 * Encode a list of per-cv proofs into the wire format for
 * `ShieldedBundle.aggregated_range_proof`. Caller is responsible for
 * passing proofs in canonical bundle order.
 */
std::vector<uint8_t> EncodeAggregatedRangeProof(
    const std::vector<std::vector<uint8_t>>& per_cv_proofs);

}  // namespace dinero::consensus::shielded
