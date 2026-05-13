#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero {

/// Default extension commitment: 32 zero bytes.
/// Ordinary transparent Taproot spends still use this value. CT-aware Taproot
/// spends may derive a non-zero extension commitment from prevout commitments.
constexpr std::array<uint8_t, 32> DEFAULT_EXT_COMMITMENT = {};

/**
 * Intent Descriptor for wallet-level transaction intent binding.
 *
 * Binds a Schnorr signature to the intent of the transaction, preventing
 * sign-and-drain attacks where a compromised signer substitutes outputs.
 *
 * The ext_commitment is computed as:
 *   TaggedHash("dinero/intent/v1", serialize(IntentDescriptor))
 *
 * Ordinary transparent spends can continue to use DEFAULT_EXT_COMMITMENT.
 * Additional wallet-local binding material may be composed with the CT-aware
 * prevout extension when SigHash v1 is used.
 */
struct IntentDescriptor {
    /// SHA256 hash of the intended recipient address
    std::array<uint8_t, 32> recipient_hash;

    /// Intended amount in una
    uint64_t amount;

    /// Maximum acceptable fee in una
    uint64_t max_fee;

    /// Block height after which this intent expires
    uint32_t expiry_height;

    /// Human-readable purpose tag (max 64 bytes, truncated if longer)
    std::string purpose_tag;

    /**
     * Serialize to canonical byte representation.
     * Format: recipient_hash(32) || amount(8 LE) || max_fee(8 LE) ||
     *         expiry_height(4 LE) || purpose_tag_len(1) || purpose_tag(N)
     */
    std::vector<uint8_t> Serialize() const;

    /**
     * Compute the 32-byte ext_commitment for SigHash v1.
     * Returns TaggedHash("dinero/intent/v1", Serialize())
     */
    std::array<uint8_t, 32> ComputeExtCommitment() const;

    /**
     * Deserialize from bytes.
     * @throws std::runtime_error on malformed data
     */
    static IntentDescriptor Deserialize(const std::vector<uint8_t>& data);
};

} // namespace dinero
