#pragma once
/**
 * Phase 3 wave 3 (Path C) — high-level shielded bundle builder.
 *
 * Wallets call this to assemble a v5-ready `ShieldedBundle` from a
 * list of spends-to-perform and outputs-to-create. The builder
 * handles every cryptographic primitive Wave 1B / Wave 2 introduced:
 *
 *   - Consumes caller-supplied fresh `rcv` (Pedersen blinding randomness)
 *     and range-proof nonces per spend and per output.
 *   - Computes `cv = blind·G + value·V` for each via `PedersenCommit`.
 *   - Generates per-cv Borromean range proofs via `SignRangeProof`
 *     and concatenates them into `aggregated_range_proof`.
 *   - Sums the rcvs into `bsk` and computes `bvk_commitment = bsk·G`.
 *   - Computes the canonical binding sighash incorporating
 *     `tx_sighash` (transparent-envelope wrap-attack guard).
 *   - Signs `binding_sig` with bsk under BIP340 Schnorr.
 *   - Populates `value_balance = sum(output_value) - sum(spend_value)`
 *     (Dinero convention: positive = shield, negative = unshield).
 *
 * The wallet supplies:
 *   - For each spend: the secret_key, value, randomness, leaf_index,
 *     anchor, and zk_proof bytes (Spartan spend proof). The builder
 *     does NOT generate spend ZK proofs — wallet code already does
 *     that via `ProveSpend`. Builder only adds cv + range proof +
 *     binding-sig contribution.
 *   - For each output: value, public_key (recipient), randomness,
 *     commitment, encrypted_note, and zk_proof bytes (Spartan output
 *     proof, similarly pre-computed by wallet via `ProveOutput`).
 *
 * The builder enforces:
 *   - Per-cv values must fit in 64 bits (range proof rejects others).
 *   - Bundle size limits (`kMaxSpendsPerBundle`, `kMaxOutputsPerBundle`).
 *   - All cryptographic operations succeed; on failure, returns a
 *     non-Ok status and `out_bundle` is left in a partial state.
 */

#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/commitment_tree.h"
#include "consensus/shielded/shielded_tx.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dinero::consensus::shielded {

enum class BundleBuildResult : uint8_t {
    Ok                    = 0,
    GeneratorNotReady     = 1,
    PedersenFailure       = 2,
    RangeProofFailure     = 3,
    BlindSumFailure       = 4,
    BvkFailure            = 5,
    BindingSigFailure     = 6,
    TooManySpends         = 7,
    TooManyOutputs        = 8,
};

/// One spend the wallet wants to perform. The wallet has already
/// generated the Spartan spend proof and knows the spend's value
/// and blinding randomness.
struct PlannedSpend {
    Hash                   nullifier;
    Hash                   anchor;
    uint64_t               value_una;
    Hash                   rcv;          ///< 32-byte Pedersen blind, fresh from CSPRNG
    std::vector<uint8_t>   spend_proof;  ///< Spartan spend proof bytes
    Hash                   nonce;        ///< 32-byte rangeproof nonce, fresh from CSPRNG
};

/// One output the wallet wants to create. Wallet has the Spartan
/// output proof; builder adds cv + range proof.
struct PlannedOutput {
    Hash                   commitment;       ///< Poseidon commitment (note tree leaf)
    uint64_t               value_una;
    Hash                   rcv;              ///< Pedersen blind
    std::vector<uint8_t>   encrypted_note;
    std::vector<uint8_t>   output_proof;     ///< Spartan output proof bytes
    Hash                   nonce;            ///< rangeproof nonce
};

/**
 * Build a complete, validation-ready ShieldedBundle.
 *
 * @param spends      list of planned spends (in any order — builder
 *                    sorts canonically before serialization)
 * @param outputs     list of planned outputs (same)
 * @param tx_sighash  the transparent-envelope sighash this bundle
 *                    will ride in. Use `ComputeShieldedTxSighash(tx)`
 *                    on the post-bundle-attached tx — but note the
 *                    sighash doesn't depend on the bundle bytes, so
 *                    callers can compute it before this call.
 * @param out_bundle  output: the constructed bundle, ready to attach
 *                    to `Transaction.shielded_bundle_bytes` after
 *                    `SerializeShieldedBundle`.
 */
BundleBuildResult BuildShieldedBundle(const std::vector<PlannedSpend>& spends,
                                      const std::vector<PlannedOutput>& outputs,
                                      const Hash& tx_sighash,
                                      ShieldedBundle& out_bundle);

}  // namespace dinero::consensus::shielded
