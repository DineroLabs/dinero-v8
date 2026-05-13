#pragma once

#include "lightning/lightning_types.h"
#include "common/status.h"
#include <vector>
#include <array>
#include <optional>
#include <cstdint>

namespace dinero {
namespace lightning {

/**
 * @file onion.h
 * @brief BOLT #4 Onion Routing Implementation (Sphinx)
 *
 * Implements Lightning Network onion packet construction and decryption.
 * Uses Sphinx-style onion routing with ECDH key agreement and ChaCha20 encryption.
 *
 * BOLT #4 Specification:
 * - Fixed-size 1366-byte onion packets
 * - Per-hop TLV payloads (type-length-value)
 * - ECDH shared secret derivation
 * - HMAC integrity verification
 * - ChaCha20 stream cipher for payload encryption
 */

// Constants from BOLT #4
namespace constants {
    constexpr size_t ONION_PACKET_SIZE = 1366;      // Fixed packet size
    constexpr size_t ONION_HOP_DATA_SIZE = 65;      // Per-hop payload size
    constexpr size_t MAX_ONION_HOPS = 20;           // Maximum route length
    constexpr size_t SHARED_SECRET_SIZE = 32;       // ECDH shared secret size
    constexpr size_t EPHEMERAL_KEY_SIZE = 33;       // Compressed pubkey size
    constexpr size_t HMAC_SIZE = 32;                // HMAC-SHA256 size
    constexpr size_t ROUTING_INFO_SIZE = 1300;      // Encrypted routing data size
}

// TLV types for hop payloads (BOLT #4)
namespace TLVType {
    constexpr uint8_t AMT_TO_FORWARD = 2;           // Amount to forward (tu64)
    constexpr uint8_t OUTGOING_CLTV_VALUE = 4;      // Outgoing CLTV value (tu32)
    constexpr uint8_t SHORT_CHANNEL_ID = 6;         // Next channel (8 bytes)
    constexpr uint8_t PAYMENT_DATA = 8;             // Payment secret + total amount
    constexpr uint8_t PAYMENT_SECRET = 8;           // Payment secret (32 bytes)
    constexpr uint8_t TOTAL_AMOUNT_MSAT = 10;       // Total payment amount (tu64)
}

/**
 * @struct HopPayloadTLV
 * @brief Per-hop TLV payload data
 *
 * Contains routing instructions for each hop in the payment route.
 * Encoded as TLV (type-length-value) format per BOLT #4.
 */
struct HopPayloadTLV {
    // Required fields
    uint64_t amt_to_forward_muna;       // Amount to forward to next hop
    uint32_t outgoing_cltv_value;       // CLTV value for next hop

    // Intermediate hop fields
    std::optional<uint64_t> short_channel_id;  // Next channel ID (not present for final hop)

    // Final hop fields
    std::optional<std::vector<uint8_t>> payment_secret;  // 32-byte payment secret
    std::optional<uint64_t> total_amount_muna;          // Total multi-part payment amount

    HopPayloadTLV()
        : amt_to_forward_muna(0), outgoing_cltv_value(0) {}

    /**
     * @brief Check if this is a final hop payload
     * @return true if this hop is the payment destination
     */
    bool isFinal() const {
        return !short_channel_id.has_value();
    }

    /**
     * @brief Serialize to TLV format
     * @return Serialized TLV bytes
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize from TLV format
     * @param data TLV encoded data
     * @return Result with parsed payload or error
     */
    static Result<HopPayloadTLV> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @struct RouteHop
 * @brief Single hop in a payment route
 */
struct RouteHop {
    std::vector<uint8_t> node_id;       // 33-byte compressed node pubkey
    HopPayloadTLV payload;              // Per-hop routing instructions
    uint64_t fee_muna;                  // Fee for this hop (calculated)
    uint32_t cltv_expiry_delta;         // CLTV delta for this hop

    RouteHop()
        : fee_muna(0), cltv_expiry_delta(0) {}

    RouteHop(const std::vector<uint8_t>& id, const HopPayloadTLV& p)
        : node_id(id), payload(p), fee_muna(0), cltv_expiry_delta(0) {}
};

/**
 * @struct OnionPacket
 * @brief BOLT #4 onion packet (1366 bytes fixed size)
 *
 * Format:
 * - version (1 byte)
 * - ephemeral_pubkey (33 bytes)
 * - routing_info (1300 bytes) - encrypted hop data
 * - hmac (32 bytes) - integrity check
 */
struct OnionPacket {
    uint8_t version;                                    // Protocol version (0x00)
    std::array<uint8_t, 33> ephemeral_pubkey;          // Ephemeral public key
    std::array<uint8_t, 1300> routing_info;            // Encrypted routing data
    std::array<uint8_t, 32> hmac;                      // HMAC-SHA256

    OnionPacket()
        : version(0) {
        ephemeral_pubkey.fill(0);
        routing_info.fill(0);
        hmac.fill(0);
    }

    /**
     * @brief Serialize packet to wire format
     * @return 1366-byte serialized packet
     */
    std::vector<uint8_t> serialize() const;

    /**
     * @brief Deserialize packet from wire format
     * @param data Wire format bytes (must be 1366 bytes)
     * @return Result with parsed packet or error
     */
    static Result<OnionPacket> deserialize(const std::vector<uint8_t>& data);
};

/**
 * @struct SharedSecrets
 * @brief Derived keys for each hop
 *
 * Generated via ECDH + HKDF for encryption and MAC operations.
 */
struct SharedSecrets {
    std::vector<std::array<uint8_t, 32>> rho;      // Encryption keys (ChaCha20)
    std::vector<std::array<uint8_t, 32>> mu;       // HMAC keys
    std::vector<std::array<uint8_t, 32>> ammag;    // Blinding factors
    std::vector<std::array<uint8_t, 32>> um;       // Error return encryption
};

/**
 * @struct OnionPacketWithSecrets
 * @brief Bundles OnionPacket with SharedSecrets for error decryption
 *
 * Phase 5.1: When building onion packets, the sender needs to store the
 * shared secrets to later decrypt onion error packets and extract true
 * BOLT #4 failure codes from failed payment attempts.
 */
struct OnionPacketWithSecrets {
    OnionPacket packet;         // The encrypted onion packet to send
    SharedSecrets secrets;      // Shared secrets for error decryption (sender-only)
};

/**
 * @class OnionBuilder
 * @brief Constructs Sphinx onion packets for multi-hop payments
 *
 * Builds layered encryption onion packets where each hop can only decrypt
 * its own layer and forward the remaining onion to the next hop.
 */
class OnionBuilder {
public:
    /**
     * @brief Build an onion packet for a payment route
     *
     * Creates a Sphinx onion packet with layered encryption. Each hop:
     * 1. Performs ECDH with ephemeral key to derive shared secret
     * 2. Derives decryption keys via HKDF
     * 3. Decrypts one layer to reveal their hop payload
     * 4. Forwards remaining onion to next hop
     *
     * Phase 5.1 Update: Now returns OnionPacketWithSecrets containing both
     * the onion packet (to send) and shared secrets (to store for error decryption).
     *
     * @param route Vector of route hops (must be 1-20 hops)
     * @param payment_hash 32-byte payment hash
     * @param session_key Optional 32-byte session key (random if not provided)
     * @return Result with onion packet + shared secrets, or error
     *
     * Example:
     *   std::vector<RouteHop> route = {{node1, payload1}, {node2, payload2}};
     *   auto result = OnionBuilder::build(route, payment_hash);
     *   if (result.isOk()) {
     *       auto onion_with_secrets = result.unwrap();
     *       // Send onion_with_secrets.packet with add_htlc message
     *       // Store onion_with_secrets.secrets for error decryption
     *   }
     */
    static Result<OnionPacketWithSecrets> build(
        const std::vector<RouteHop>& route,
        const std::vector<uint8_t>& payment_hash,
        const std::optional<std::vector<uint8_t>>& session_key = std::nullopt
    );

    /**
     * @brief Generate shared secrets for all hops
     *
     * Performs ECDH with each hop's node public key and derives
     * encryption/MAC keys via HKDF-SHA256.
     *
     * @param route Payment route
     * @param session_key Ephemeral private key
     * @return Result with derived secrets or error
     */
    static Result<SharedSecrets> generateSharedSecrets(
        const std::vector<RouteHop>& route,
        const std::vector<uint8_t>& session_key
    );

    /**
     * @brief Derive keys from shared secret using HKDF
     * @param shared_secret 32-byte ECDH shared secret
     * @return Tuple of (rho, mu, ammag, um) keys
     */
    static std::tuple<
        std::array<uint8_t, 32>,  // rho (encryption)
        std::array<uint8_t, 32>,  // mu (MAC)
        std::array<uint8_t, 32>,  // ammag (blinding)
        std::array<uint8_t, 32>   // um (error encryption)
    > deriveKeys(const std::vector<uint8_t>& shared_secret);

private:

    /**
     * @brief Compute HMAC-SHA256
     * @param key HMAC key
     * @param data Data to MAC
     * @return 32-byte HMAC
     */
    static std::array<uint8_t, 32> computeHMAC(
        const std::array<uint8_t, 32>& key,
        const std::vector<uint8_t>& data
    );

    /**
     * @brief Encrypt data with ChaCha20
     * @param key 32-byte encryption key
     * @param nonce 12-byte nonce (or 8-byte for legacy)
     * @param data Data to encrypt
     * @return Encrypted data (same size as input)
     */
    static std::vector<uint8_t> chacha20Encrypt(
        const std::array<uint8_t, 32>& key,
        const std::vector<uint8_t>& nonce,
        const std::vector<uint8_t>& data
    );
};

/**
 * @class OnionPeeler
 * @brief Decrypts one layer of a Sphinx onion packet
 *
 * Each hop uses this to peel off their encrypted layer and extract
 * routing instructions for the next hop.
 */
class OnionPeeler {
public:
    /**
     * @struct PeelResult
     * @brief Result of peeling one onion layer
     */
    struct PeelResult {
        bool is_final;                  // True if this is the final hop
        HopPayloadTLV payload;          // Routing instructions for this hop
        OnionPacket next_onion;         // Onion for next hop (if not final)
        std::vector<uint8_t> shared_secret;  // ECDH shared secret (for error returns)

        PeelResult() : is_final(false) {}
    };

    /**
     * @brief Peel one layer from an onion packet
     *
     * Process:
     * 1. Perform ECDH with ephemeral key using our node private key
     * 2. Derive decryption keys via HKDF
     * 3. Verify HMAC
     * 4. Decrypt hop data to extract payload
     * 5. Shift routing info and re-encrypt for next hop
     *
     * @param packet Incoming onion packet
     * @param node_privkey Our node private key (32 bytes)
     * @param payment_hash Payment hash (for HMAC verification)
     * @return Result with peel result or error
     *
     * Example:
     *   auto result = OnionPeeler::peel(onion, my_privkey, payment_hash);
     *   if (result.isOk()) {
     *       auto peel = result.unwrap();
     *       if (peel.is_final) {
     *           // This payment is for us - settle with preimage
     *       } else {
     *           // Forward to next hop
     *           forwardHTLC(peel.payload.short_channel_id.value(), peel.next_onion);
     *       }
     *   }
     */
    static Result<PeelResult> peel(
        const OnionPacket& packet,
        const std::vector<uint8_t>& node_privkey,
        const std::vector<uint8_t>& payment_hash
    );

private:
    /**
     * @brief Verify HMAC on onion packet
     * @param mu HMAC key
     * @param packet Onion packet to verify
     * @param payment_hash Payment hash
     * @return true if HMAC is valid
     */
    static bool verifyHMAC(
        const std::array<uint8_t, 32>& mu,
        const OnionPacket& packet,
        const std::vector<uint8_t>& payment_hash
    );

    /**
     * @brief Extract hop data from routing info
     * @param routing_info Decrypted routing info
     * @return Hop payload bytes
     */
    static std::vector<uint8_t> extractHopData(
        const std::array<uint8_t, 1300>& routing_info
    );
};

} // namespace lightning
} // namespace dinero
