#pragma once
/**
 * Shielded transaction components — the wire format for shielded transactions.
 *
 * A v5 transaction can carry BOTH transparent and shielded components:
 *
 *   transparent inputs   → Utreexo removes (normal spend)
 *   transparent outputs  → Utreexo adds (normal creation)
 *   shielded spends      → nullifier published, commitment tree unchanged
 *   shielded outputs     → commitment tree appends new leaf
 *
 * Value conservation (enforced by consensus):
 *   sum(transparent_in) + sum(shielded_spend_values)
 *     = sum(transparent_out) + sum(shielded_output_values) + fee
 *
 * The ZK proof demonstrates value conservation without revealing the
 * actual shielded values. The fee is always transparent (visible).
 *
 * CRITICAL BOUNDARY:
 *   - Shielded outputs NEVER enter Utreexo.
 *   - Transparent outputs ALWAYS enter Utreexo.
 *   - The commitment tree and Utreexo are structurally separate.
 */

#include "consensus/shielded/commitment_tree.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dinero::consensus::shielded {

/**
 * A shielded spend — consumes a previously-created shielded note.
 *
 * The ZK proof demonstrates:
 *   1. The spender knows the secret key for a valid note commitment
 *   2. That commitment exists in the commitment tree (Merkle path valid)
 *   3. The nullifier is correctly derived from (secret_key, leaf_index)
 *   4. The value committed in this spend balances with the tx outputs
 *
 * The proof hides: which commitment was spent, the value, the recipient.
 * The nullifier is public (consensus checks uniqueness).
 *
 * Phase 3 wave 1 (Path C): cv is the Pedersen value commitment
 * `cv = v · V_GEN + rcv · R_GEN` published alongside the proof.
 * Consensus uses cv for the cross-bundle balance check (wave 2).
 */
/// Pedersen value commitment in 33-byte compressed form. The libsecp
/// pedersen serialization uses prefix 0x08 (odd-y) / 0x09 (even-y);
/// preserving the prefix is consensus-critical, since dropping it
/// would force verifiers to guess parity 50/50 per cv, breaking
/// the bvk reconstruction in binding-sig verify.
using ValueCommitment = std::array<uint8_t, 33>;

struct ShieldedSpend {
    Hash                   nullifier;       ///< 32 bytes — double-spend key
    Hash                   anchor;          ///< commitment tree root at proof time
    ValueCommitment        cv{};            ///< Pedersen value commitment (33-byte compressed)
    std::vector<uint8_t>   zk_proof;        ///< Spartan/Nova proof bytes
};

/**
 * A shielded output — creates a new note in the commitment tree.
 *
 * The commitment hides the value and recipient. The encrypted_note
 * allows the recipient (who holds the viewing key) to decrypt and
 * learn the value + randomness needed to spend the note later.
 */
struct ShieldedOutput {
    Hash                   commitment;      ///< Poseidon(value_cm, pk, randomness)
    ValueCommitment        cv{};            ///< Pedersen value commitment (33-byte compressed)
    std::vector<uint8_t>   encrypted_note;  ///< AEAD-encrypted note for recipient
    std::vector<uint8_t>   zk_proof;        ///< Proof that commitment is well-formed
};

/**
 * The shielded bundle — attached to a v5 transaction alongside the
 * normal transparent inputs/outputs.
 *
 * A bundle can be:
 *   - Shield-only:   0 spends, N outputs (value flows in from transparent)
 *   - Transfer-only: N spends, M outputs (no transparent component needed)
 *   - Unshield-only: N spends, 0 outputs (value flows out to transparent)
 *   - Mixed: any combination
 *
 * The value_balance field is the NET transparent value absorbed by the
 * shielded pool. Positive = value flowing INTO the pool (shield).
 * Negative = value flowing OUT (unshield). Zero = balanced transfer.
 *
 * Consensus verifies: transparent_in - transparent_out - fee = value_balance
 * The ZK proofs verify: sum(spend_values) - sum(output_values) = value_balance
 */
/// Phase 3 wave 2: Schnorr binding signature, 64 bytes:
///   bytes [0..32) — R (32-byte x-only)
///   bytes [32..64) — s (32-byte scalar)
/// Per BIP340. Verifier reconstructs bvk = sum(cv_spend) - sum(cv_output)
/// - value_balance·V and checks the signature.
using BindingSignature = std::array<uint8_t, 64>;

struct ShieldedBundle {
    int64_t                          value_balance = 0;  ///< net value into pool (can be negative)
    std::vector<ShieldedSpend>       spends;
    std::vector<ShieldedOutput>      outputs;
    /// Aggregated range proof: per-cv Borromean rangeproof bytes today,
    /// reinterpreted as Bulletproofs aggregated proof when libsecp ships
    /// the verifier. Wire format documented in range_proof.h.
    std::vector<uint8_t>             aggregated_range_proof;
    /// Phase 3 wave 2: published bvk in pedersen_commitment form
    /// (33 bytes). bvk = bsk·G + 0·V where bsk = sum(rcv_spend) -
    /// sum(rcv_output). Verifier consumes via pedersen_verify_tally
    /// for balance, then extracts x for Schnorr verify.
    ValueCommitment                  bvk_commitment{};
    /// Phase 3 wave 2: 64-byte Schnorr binding signature over the
    /// canonical bundle sighash. Closes the cross-bundle inflation hole.
    BindingSignature                 binding_sig{};

    bool IsEmpty() const { return spends.empty() && outputs.empty(); }
    bool IsShieldOnly() const { return spends.empty() && !outputs.empty(); }
    bool IsUnshieldOnly() const { return !spends.empty() && outputs.empty(); }
};

/** Transaction versions for shielded bundles. */
constexpr int32_t TX_VERSION_SHIELDED = 5;
constexpr int32_t TX_VERSION_SHIELDED_V2 = 6;

// ── Bundle size limits (consensus-enforced) ─────────────────────────
// Prevents block-stuffing DoS. Worst-case verification cost is bounded
// at ~kMaxSpendsPerBundle * 250ms (Spartan spend verify on Mac M-series)
// per shielded transaction. Generous enough that legitimate use never
// hits the cap; raising these requires a coordinated fork.
constexpr size_t kMaxSpendsPerBundle  = 200;
constexpr size_t kMaxOutputsPerBundle = 200;

// Phase 3 wave 1 (Path C): max number of value commitments aggregated
// into a single Bulletproof. Verifier cost grows ~O(N log N); cap of
// 32 keeps per-BP work bounded. Bundles with > 32 cvs emit multiple
// BPs (concatenated in `aggregated_range_proof`).
constexpr size_t kMaxBPAggregationDepth = 32;

// ── VWU constants for shielded components ────────────────────────────
// These price ZK proof verification in the same fee market as transparent
// signature verification. Values chosen so a shielded spend (~5 ms verify)
// costs roughly the same VWU as a P2MR input (~3 ms verify + 5 KB witness).
constexpr uint64_t SHIELDED_SPEND_VWU  = 5000;
constexpr uint64_t SHIELDED_OUTPUT_VWU = 500;

/**
 * Compute the VWU contribution of a shielded bundle.
 * Pure function — no state access.
 */
inline uint64_t ComputeShieldedBundleVWU(const ShieldedBundle& bundle) {
    return bundle.spends.size() * SHIELDED_SPEND_VWU
         + bundle.outputs.size() * SHIELDED_OUTPUT_VWU;
}

} // namespace dinero::consensus::shielded
