#pragma once
/**
 * Canonical v5 shielded bundle serialization.
 *
 * INVARIANT: exactly ONE byte representation per shielded bundle.
 * No alternative encodings. No ordering ambiguity. Two nodes that
 * see the same logical bundle MUST produce identical bytes.
 *
 * Wire format (all integers little-endian):
 *
 *   [8 bytes]  value_balance (int64_t LE)
 *   [varint]   num_spends
 *   for each spend:
 *     [32 bytes] nullifier
 *     [32 bytes] anchor
 *     [varint]   proof_len
 *     [N bytes]  zk_proof
 *   [varint]   num_outputs
 *   for each output:
 *     [32 bytes] commitment
 *     [varint]   encrypted_note_len
 *     [N bytes]  encrypted_note
 *     [varint]   proof_len
 *     [N bytes]  zk_proof
 *   [32 bytes] binding_sig
 *
 * Canonical constraints:
 *   - Spends are ordered by nullifier (lexicographic, ascending)
 *   - Outputs are ordered by commitment (lexicographic, ascending)
 *   - Varints use Bitcoin's CompactSize (minimal encoding only)
 *   - No trailing bytes after binding_sig
 *   - Reserialize-equals-original check (same as P2MR witness)
 */

#include "consensus/shielded/shielded_tx.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace dinero::consensus::shielded {

/**
 * Serialize a ShieldedBundle to its canonical byte representation.
 * Spends are sorted by nullifier, outputs by commitment.
 * Returns empty vector if the bundle is structurally invalid.
 */
std::vector<uint8_t> SerializeShieldedBundle(const ShieldedBundle& bundle);

enum class BundleDecodeError : uint8_t {
    Ok              = 0,
    Truncated       = 1,
    VarintOverflow  = 2,
    TrailingBytes   = 3,
    NotCanonical    = 4,   ///< re-serialization differs from input
    OrderViolation  = 5,   ///< spends/outputs not in canonical order
};

/**
 * Deserialize a ShieldedBundle from bytes. Enforces canonical ordering
 * and minimal varint encoding. On success, `*out` is populated.
 *
 * The NonCanonical check: if Serialize(Deserialize(bytes)) != bytes,
 * the input is rejected. This catches alternative encodings.
 */
BundleDecodeError DeserializeShieldedBundle(const uint8_t* data,
                                            size_t len,
                                            ShieldedBundle* out);

inline BundleDecodeError DeserializeShieldedBundle(const std::vector<uint8_t>& data,
                                                    ShieldedBundle* out) {
    return DeserializeShieldedBundle(data.data(), data.size(), out);
}

} // namespace dinero::consensus::shielded
