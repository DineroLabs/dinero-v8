#pragma once

#include "lightning/lightning_types.h"
#include "lightning/onion.h"
#include <vector>
#include <array>
#include <cstdint>
#include <string>
#include <optional>

namespace dinero {
namespace lightning {

/**
 * @file onion_error.h
 * @brief BOLT #4 Onion Failure Messages
 *
 * Implements Lightning Network failure packet construction and propagation.
 * When a payment fails at any hop, the failing node creates an encrypted
 * failure message that gets propagated back to the sender through the onion.
 *
 * BOLT #4 Specification:
 * - Failure codes indicate specific error conditions
 * - Errors are onion-encrypted using HMAC keys from packet construction
 * - Each hop adds a layer of encryption (backward onion)
 * - Sender decrypts layers to identify failing hop and reason
 */

// ═══════════════════════════════════════════════════════════════════════════
// BOLT #4 Failure Codes
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @enum FailureCode
 * @brief BOLT #4 standardized failure codes
 *
 * Failure codes are 16-bit values with specific semantics:
 * - Bit 15 (BADONION): malformed onion (perm failure, don't retry)
 * - Bit 14 (PERM): permanent failure (don't retry this route)
 * - Bit 13 (NODE): failure from node (not channel-specific)
 * - Bit 12 (UPDATE): includes channel_update in failure message
 *
 * Categories:
 * - 0x0xxx: Protocol errors (invalid onion, unknown version, etc.)
 * - 0x1xxx: Temporary node/channel failures (retry possible)
 * - 0x2xxx: Temporary node failures (retry with different route)
 * - 0x4xxx: Permanent failures (route/channel unusable)
 * - 0x8xxx: Bad onion (malformed packet, can't parse)
 */
enum class FailureCode : uint16_t {
    // ═══════════════════════════════════════════════════════════════════════
    // Invalid Onion Errors (BADONION = 1, malformed packet)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Onion version byte is unknown (BADONION | PERM)
     * Indicates sender used unsupported onion protocol version
     */
    INVALID_ONION_VERSION = 0xBADF,

    /**
     * Onion HMAC verification failed (BADONION)
     * Packet was corrupted or modified in transit
     */
    INVALID_ONION_HMAC = 0x8000,

    /**
     * Onion ephemeral key is invalid (BADONION | PERM)
     * ECDH key agreement failed, cannot decrypt
     */
    INVALID_ONION_KEY = 0x8001,

    /**
     * Onion blinding point is invalid (BADONION | PERM)
     * Used for route blinding, invalid ephemeral key
     */
    INVALID_ONION_BLINDING = 0x8002,

    /**
     * Onion payload parsing failed (BADONION | PERM)
     * TLV stream is malformed or has invalid fields
     */
    INVALID_ONION_PAYLOAD = 0x8003,

    // ═══════════════════════════════════════════════════════════════════════
    // Protocol Errors (general)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Realm byte is not 0 (PERM)
     * Only realm 0 is currently defined
     */
    INVALID_REALM = 0x0001,

    // ═══════════════════════════════════════════════════════════════════════
    // Temporary Node Failures (NODE, retry with different route)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Temporary node failure (NODE)
     * Node is experiencing issues, try another route
     */
    TEMPORARY_NODE_FAILURE = 0x2002,

    // ═══════════════════════════════════════════════════════════════════════
    // Permanent Node Failures (NODE | PERM, don't retry through this node)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Permanent node failure (PERM | NODE)
     * Node is permanently unavailable, remove from graph
     */
    PERMANENT_NODE_FAILURE = 0x4002,

    /**
     * Required node feature missing (PERM | NODE)
     * Node doesn't support required feature for this payment
     */
    REQUIRED_NODE_FEATURE_MISSING = 0x4003,

    /**
     * Invalid onion blinding (PERM | NODE)
     * Route blinding validation failed
     */
    INVALID_ONION_BLINDING_NODE = 0x4004,

    // ═══════════════════════════════════════════════════════════════════════
    // Temporary Channel Failures (UPDATE, includes channel_update)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Temporary channel failure (UPDATE)
     * Channel is temporarily unavailable, includes updated channel_update
     */
    TEMPORARY_CHANNEL_FAILURE = 0x1007,

    /**
     * Channel disabled (UPDATE)
     * Channel is administratively disabled
     */
    CHANNEL_DISABLED = 0x1008,

    // ═══════════════════════════════════════════════════════════════════════
    // Permanent Channel Failures (PERM, don't retry this channel)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Permanent channel failure (PERM)
     * Channel is permanently closed or unusable
     */
    PERMANENT_CHANNEL_FAILURE = 0x4007,

    /**
     * Required channel feature missing (PERM)
     * Channel doesn't support required feature for this payment
     */
    REQUIRED_CHANNEL_FEATURE_MISSING = 0x4009,

    /**
     * Unknown next peer (PERM)
     * Next hop specified in onion is not a peer of this node
     */
    UNKNOWN_NEXT_PEER = 0x400A,

    // ═══════════════════════════════════════════════════════════════════════
    // HTLC Parameter Errors (PERM | UPDATE, includes channel_update)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Amount below minimum (PERM | UPDATE)
     * HTLC amount is below channel's htlc_minimum_msat
     */
    AMOUNT_BELOW_MINIMUM = 0x400B,

    /**
     * Fee insufficient (PERM | UPDATE)
     * Forwarding fee is less than channel's fee requirements
     */
    FEE_INSUFFICIENT = 0x400C,

    /**
     * Incorrect CLTV expiry (PERM | UPDATE)
     * CLTV expiry doesn't match channel's cltv_expiry_delta
     */
    INCORRECT_CLTV_EXPIRY = 0x400D,

    /**
     * Expiry too soon (UPDATE)
     * CLTV expiry is too close to current block height
     */
    EXPIRY_TOO_SOON = 0x400E,

    /**
     * Expiry too far (PERM)
     * CLTV expiry is unreasonably far in the future
     */
    EXPIRY_TOO_FAR = 0x4015,

    /**
     * Amount too large (UPDATE)
     * HTLC amount exceeds channel's htlc_maximum_msat
     */
    AMOUNT_TOO_LARGE = 0x4016,

    // ═══════════════════════════════════════════════════════════════════════
    // Final Hop Errors (PERM, payment validation failures)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Incorrect or unknown payment details (PERM)
     * Payment hash unknown, or payment_secret incorrect
     */
    INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS = 0x400F,

    /**
     * Incorrect payment amount (PERM)
     * Final amount doesn't match invoice amount
     */
    FINAL_INCORRECT_HTLC_AMOUNT = 0x4010,

    /**
     * Final expiry too soon (PERM)
     * Final CLTV expiry is less than invoice min_final_cltv_expiry
     */
    FINAL_EXPIRY_TOO_SOON = 0x4011,

    /**
     * Final incorrect CLTV expiry (PERM)
     * Final CLTV doesn't match invoice requirements
     */
    FINAL_INCORRECT_CLTV_EXPIRY = 0x4012,

    // ═══════════════════════════════════════════════════════════════════════
    // Multi-Path Payment (MPP) Errors
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * MPP timeout (PERM)
     * Multi-path payment parts didn't arrive in time
     */
    MPP_TIMEOUT = 0x4017,

    /**
     * Incorrect total amount (PERM)
     * Sum of MPP parts doesn't match total_msat
     */
    INCORRECT_TOTAL_AMOUNT = 0x4018,
};

/**
 * @brief Convert failure code to human-readable string
 * @param code Failure code
 * @return Human-readable description
 */
std::string failureCodeToString(FailureCode code);

/**
 * @brief Check if failure is permanent (PERM bit set)
 * @param code Failure code
 * @return true if permanent failure (don't retry this route)
 */
inline bool isPermanentFailure(FailureCode code) {
    return (static_cast<uint16_t>(code) & 0x4000) != 0;
}

/**
 * @brief Check if failure is node-level (NODE bit set)
 * @param code Failure code
 * @return true if node failure (not channel-specific)
 */
inline bool isNodeFailure(FailureCode code) {
    return (static_cast<uint16_t>(code) & 0x2000) != 0;
}

/**
 * @brief Check if failure includes channel_update (UPDATE bit set)
 * @param code Failure code
 * @return true if failure includes channel_update data
 */
inline bool includesUpdate(FailureCode code) {
    return (static_cast<uint16_t>(code) & 0x1000) != 0;
}

/**
 * @brief Check if failure is bad onion (BADONION bit set)
 * @param code Failure code
 * @return true if onion packet was malformed
 */
inline bool isBadOnion(FailureCode code) {
    return (static_cast<uint16_t>(code) & 0x8000) != 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Onion Error Packet
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @struct OnionErrorPacket
 * @brief Encrypted failure message propagated backward through route
 *
 * When a payment fails, the failing hop creates an error packet that is
 * onion-encrypted and sent back through the route. Each hop adds a layer
 * of encryption, and the sender decrypts all layers to identify the failing
 * hop and failure reason.
 *
 * Encryption uses HMAC keys (`um`) derived during onion packet construction.
 */
struct OnionErrorPacket {
    /**
     * Encrypted failure data
     * Contains: failure_code (2 bytes) + failure_data (variable)
     * Encrypted with ChaCha20-Poly1305 using `um` keys
     */
    std::vector<uint8_t> encrypted_data;

    /**
     * HMAC for integrity verification
     * HMAC-SHA256(ammag_key, encrypted_data)
     */
    std::array<uint8_t, 32> hmac;

    OnionErrorPacket() {
        hmac.fill(0);
    }

    /**
     * @brief Serialize error packet to wire format
     * @return Serialized error packet
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize error packet from wire format
     * @param data Wire format bytes
     * @return Result with parsed error packet or error
     */
    static Result<OnionErrorPacket> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @struct FailureMessage
 * @brief Decrypted failure information
 *
 * After sender decrypts all layers of onion error packet,
 * this structure contains the original failure information.
 */
struct FailureMessage {
    FailureCode code;                           // Failure code
    std::vector<uint8_t> data;                  // Additional failure data
    size_t failing_hop_index;                   // Index of hop that failed (0-based)

    /**
     * Optional channel_update (if includesUpdate(code) == true)
     * Included for failures like TEMPORARY_CHANNEL_FAILURE, FEE_INSUFFICIENT
     */
    std::optional<std::vector<uint8_t>> channel_update;

    FailureMessage()
        : code(FailureCode::TEMPORARY_NODE_FAILURE),
          failing_hop_index(0) {}

    /**
     * @brief Get human-readable description
     * @return String describing failure
     */
    std::string description() const;
};

// ═══════════════════════════════════════════════════════════════════════════
// Onion Error Builder
// ═══════════════════════════════════════════════════════════════════════════

/**
 * @class OnionErrorBuilder
 * @brief Creates and processes onion failure packets
 *
 * Responsibilities:
 * - Create error packet at failing hop
 * - Add encryption layer at each hop on return path
 * - Decrypt all layers at sender to identify failure
 */
class OnionErrorBuilder {
public:
    /**
     * @brief Create error packet at failing hop
     *
     * Called by the node that detected the failure. Creates initial
     * error packet with failure code and optional data.
     *
     * @param code Failure code
     * @param shared_secret Shared secret for this hop (from ECDH)
     * @param channel_update Optional channel_update for UPDATE failures
     * @return Result with OnionErrorPacket or error
     */
    static Result<OnionErrorPacket> create(
        FailureCode code,
        const std::array<uint8_t, 32>& shared_secret,
        const std::optional<std::vector<uint8_t>>& channel_update = std::nullopt
    );

    /**
     * @brief Wrap error packet with additional encryption layer
     *
     * Called by each hop when forwarding error backward. Adds one layer
     * of encryption so sender can identify which hop added which layer.
     *
     * @param error Existing error packet
     * @param shared_secret Shared secret for this hop
     * @return Result with wrapped error packet or error
     */
    static Result<OnionErrorPacket> wrap(
        const OnionErrorPacket& error,
        const std::array<uint8_t, 32>& shared_secret
    );

    /**
     * @brief Decrypt error packet at sender
     *
     * Sender decrypts all layers using shared secrets from original
     * onion packet construction. Identifies which hop failed and why.
     *
     * @param error Received error packet
     * @param shared_secrets Shared secrets from onion construction
     * @return Result with FailureMessage or error
     */
    static Result<FailureMessage> decrypt(
        const OnionErrorPacket& error,
        const std::vector<std::array<uint8_t, 32>>& shared_secrets
    );

private:
    /**
     * @brief Derive error encryption key from shared secret
     * @param shared_secret ECDH shared secret
     * @return 32-byte ChaCha20-Poly1305 key
     */
    static std::array<uint8_t, 32> deriveErrorKey(
        const std::array<uint8_t, 32>& shared_secret
    );

    /**
     * @brief Derive error HMAC key from shared secret
     * @param shared_secret ECDH shared secret
     * @return 32-byte HMAC key
     */
    static std::array<uint8_t, 32> deriveHMACKey(
        const std::array<uint8_t, 32>& shared_secret
    );

    /**
     * @brief Serialize failure message to bytes
     * @param code Failure code
     * @param data Additional failure data
     * @return Serialized failure (code + data)
     */
    static std::vector<uint8_t> serializeFailure(
        FailureCode code,
        const std::vector<uint8_t>& data
    );

    /**
     * @brief Deserialize failure message from bytes
     * @param data Serialized failure
     * @return Result with parsed failure or error
     */
    static Result<std::pair<FailureCode, std::vector<uint8_t>>> deserializeFailure(
        const std::vector<uint8_t>& data
    );
};

} // namespace lightning
} // namespace dinero
