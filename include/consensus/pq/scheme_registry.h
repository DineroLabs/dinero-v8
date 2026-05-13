#pragma once
/**
 * V7 PQ Signature Scheme Registry
 *
 * Spec: docs/consensus/V7_GENESIS_SPEC.md § "Signature Scheme Registry".
 *
 * The registry is the ONE thing that consensus, mempool-fee, and template-
 * builder code consult when handling a P2MR spend. Adding a new PQ scheme
 * later is a two-line change: one new row, one new verifier wrapper file.
 *
 *   - `GetSchemeParams(scheme_id)` always returns a valid reference. Unknown /
 *     not-yet-assigned scheme_id values resolve to the RESERVED sentinel row
 *     so the caller never has to null-check.
 *
 *   - `IsSchemeAcceptedAtHeight(scheme_id, height)` is the consensus-gate
 *     helper. For ACCEPT schemes this is `height >= activation_height`. For
 *     DARK_RESERVED and RESERVED it is always `false`.
 *
 *   - Wire format:   scheme_id is ONE byte carried in the P2MR witness prefix.
 *     The byte is stable across the life of the chain. Values are assigned
 *     once and never re-bound to a different scheme.
 *
 * This header is consensus-critical. Any change to the registry values is a
 * consensus change and requires an activation-height fork.
 */

#include <cstdint>

namespace dinero::consensus::pq {

/** Wire byte for ML-DSA-65. */
constexpr uint8_t SCHEME_ID_ML_DSA_65   = 0x01;
/** Wire byte for FALCON-512. DARK_RESERVED at v7 genesis. */
constexpr uint8_t SCHEME_ID_FALCON_512  = 0x02;
/** Wire byte for SPHINCS+-128s. DARK_RESERVED at v7 genesis. */
constexpr uint8_t SCHEME_ID_SPHINCS_128 = 0x03;

enum class SchemeState : uint8_t {
    /** scheme_id is consensus-accepted at-or-above activation_height. */
    Accept = 0,
    /**
     * scheme_id is permanently bound to a named scheme but is NOT yet
     * consensus-accepted. Witnesses carrying this scheme_id MUST be rejected
     * at consensus today; a future activation-height fork flips this row to
     * Accept. Prevents adversarial standards-squatting on the wire byte.
     */
    DarkReserved = 1,
    /**
     * scheme_id is not bound to any scheme. Witnesses carrying this
     * scheme_id MUST be rejected at consensus. The value is available for
     * future binding via activation-height fork.
     */
    Reserved = 2,
};

/**
 * Immutable row in the PQ scheme registry.
 *
 * `witness_byte_weight` and `verify_cost_weight` are consulted by the weight /
 * fee machinery for any spend using this scheme. See
 * docs/consensus/V7_GENESIS_SPEC.md § "Block Weight And Fee Policy".
 *
 * Fields are zero for RESERVED / DARK_RESERVED rows to make a stray use
 * obviously nonsensical if consensus code ever forgets to check state.
 */
struct PQSchemeParams {
    uint8_t      scheme_id;
    const char*  name;                ///< Human-readable, debug-only, NOT consensus.
    SchemeState  state;
    uint32_t     pubkey_bytes_max;    ///< Per-scheme max pubkey length. Same value as pubkey_bytes for ML-DSA.
    uint32_t     signature_bytes_max; ///< Per-scheme max signature length.
    uint32_t     witness_byte_weight; ///< Per-scheme multiplier on witness wire bytes.
    uint32_t     verify_cost_weight;  ///< Flat WU added per spend-input, independent of wire size.
    uint32_t     activation_height;   ///< Mainnet height at which state becomes Accept.
};

/**
 * Returns the registry row for `scheme_id`. Always valid. Unknown or
 * not-yet-assigned scheme_ids resolve to the `Reserved` sentinel row.
 */
const PQSchemeParams& GetSchemeParams(uint8_t scheme_id);

/**
 * Consensus gate. Returns true iff `scheme_id` resolves to a row with
 * `state == Accept` and `height >= row.activation_height`.
 *
 * Consensus code should call this — never inspect `state` or
 * `activation_height` directly.
 */
bool IsSchemeAcceptedAtHeight(uint8_t scheme_id, uint32_t height);

/** Convenience: true iff `state == DarkReserved` for `scheme_id`. */
bool IsSchemeDarkReserved(uint8_t scheme_id);

} // namespace dinero::consensus::pq
