#pragma once

#include "lightning/lightning_types.h"
#include <vector>
#include <optional>
#include <string>

namespace dinero {
namespace lightning {

/**
 * @class LightningCrypto
 * @brief Cryptographic operations for Lightning Network
 *
 * Wraps existing Schnorr and MuSig2 implementations for Lightning-specific use.
 * Handles:
 * - Commitment transaction signing
 * - Funding output creation (2-of-2 MuSig2)
 * - Per-commitment secret derivation
 * - Message signing for BOLT protocol
 */
class LightningCrypto {
public:
    // ═══════════════════════════════════════════════════════════════════════════
    // Schnorr Signatures for Lightning Messages
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Sign a Lightning message with Schnorr signature
     *
     * Used for signing commitment transactions, funding transactions, etc.
     *
     * @param message_hash 32-byte SHA256 hash of message
     * @param private_key 32-byte private key
     * @return 64-byte Schnorr signature, or empty vector on failure
     */
    static std::vector<uint8_t> signMessage(
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& private_key
    );

    /**
     * @brief Verify a Schnorr signature on a Lightning message
     *
     * @param signature 64-byte Schnorr signature
     * @param message_hash 32-byte message hash
     * @param public_key 32-byte x-only public key
     * @return true if signature is valid
     */
    static bool verifySignature(
        const std::vector<uint8_t>& signature,
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& public_key
    );

    /**
     * @brief Get x-only public key from private key
     *
     * @param private_key 32-byte private key
     * @return 32-byte x-only public key
     */
    static std::vector<uint8_t> getPublicKey(
        const std::vector<uint8_t>& private_key
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // MuSig2 for 2-of-2 Funding Outputs
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Aggregate two public keys using MuSig2
     *
     * Creates the 2-of-2 funding output key from both parties' keys.
     *
     * @param pubkey1 First 32-byte x-only public key (local)
     * @param pubkey2 Second 32-byte x-only public key (remote)
     * @return Aggregated 32-byte public key for funding output
     */
    static std::optional<std::vector<uint8_t>> aggregatePublicKeys(
        const std::vector<uint8_t>& pubkey1,
        const std::vector<uint8_t>& pubkey2
    );

    /**
     * @brief Create MuSig2 nonce for signing session
     *
     * @param private_key Signer's private key
     * @param message_hash Message to be signed
     * @return 66-byte nonce (public nonce representation)
     */
    static std::optional<std::vector<uint8_t>> createMuSigNonce(
        const std::vector<uint8_t>& private_key,
        const std::vector<uint8_t>& message_hash
    );

    /**
     * @brief Create partial MuSig2 signature
     *
     * @param private_key Signer's private key
     * @param nonce Signer's secret nonce
     * @param aggregate_nonce Aggregated public nonce from both parties
     * @param message_hash Message hash to sign
     * @param aggregate_pubkey Aggregated public key
     * @return 32-byte partial signature
     */
    static std::optional<std::vector<uint8_t>> createPartialSignature(
        const std::vector<uint8_t>& private_key,
        const std::vector<uint8_t>& nonce,
        const std::vector<uint8_t>& aggregate_nonce,
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& aggregate_pubkey
    );

    /**
     * @brief Aggregate partial signatures into final MuSig2 signature
     *
     * @param partial_sig1 First partial signature
     * @param partial_sig2 Second partial signature
     * @return 64-byte final Schnorr signature
     */
    static std::optional<std::vector<uint8_t>> aggregatePartialSignatures(
        const std::vector<uint8_t>& partial_sig1,
        const std::vector<uint8_t>& partial_sig2
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Per-Commitment Secrets (BOLT #3)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Generate base commitment secret (seed)
     *
     * @return 32-byte random secret seed
     */
    static std::vector<uint8_t> generateCommitmentSeed();

    /**
     * @brief Derive per-commitment secret from seed
     *
     * Uses BOLT #3 secret derivation tree:
     * secret_i = SHA256(SHA256(seed) || i)
     *
     * @param seed 32-byte commitment seed
     * @param commitment_number Commitment transaction number
     * @return 32-byte per-commitment secret
     */
    static std::vector<uint8_t> derivePerCommitmentSecret(
        const std::vector<uint8_t>& seed,
        uint64_t commitment_number
    );

    /**
     * @brief Derive per-commitment point from secret
     *
     * per_commitment_point = secret * G (elliptic curve point multiplication)
     *
     * @param per_commitment_secret 32-byte secret
     * @return 33-byte compressed public key (per-commitment point)
     */
    static std::vector<uint8_t> derivePerCommitmentPoint(
        const std::vector<uint8_t>& per_commitment_secret
    );

    /**
     * @brief Derive revocation key for commitment transaction
     *
     * Used for penalty transactions if counterparty broadcasts old state.
     * revocation_key = SHA256(revocation_basepoint || per_commitment_point)
     *
     * @param revocation_basepoint Our revocation basepoint (33 bytes)
     * @param per_commitment_point Counterparty's per-commitment point (33 bytes)
     * @return 32-byte revocation private key
     */
    static std::vector<uint8_t> deriveRevocationKey(
        const std::vector<uint8_t>& revocation_basepoint,
        const std::vector<uint8_t>& per_commitment_point
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // ═══════════════════════════════════════════════════════════════════════════
    // Key Generation
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Generate a new keypair for Lightning channels
     *
     * Creates a cryptographically secure private key and derives the public key.
     *
     * @param out_privkey Output 32-byte private key
     * @param out_pubkey Output 32-byte x-only public key
     * @return true if successful
     */
    static bool generateKeyPair(
        std::vector<uint8_t>& out_privkey,
        std::vector<uint8_t>& out_pubkey
    );

    // ═══════════════════════════════════════════════════════════════════════════
    // Utility Functions
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * @brief Generate cryptographically secure random bytes
     *
     * @param length Number of bytes to generate
     * @return Vector of random bytes
     */
    static std::vector<uint8_t> generateRandomBytes(size_t length);

    /**
     * @brief SHA256 hash
     *
     * @param data Input data
     * @return 32-byte hash
     */
    static std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);

    /**
     * @brief Double SHA256 hash (SHA256(SHA256(x)))
     *
     * @param data Input data
     * @return 32-byte hash
     */
    static std::vector<uint8_t> doubleSha256(const std::vector<uint8_t>& data);

    /**
     * @brief Validate that a vector is 32 bytes (valid private key size)
     *
     * @param key Key to validate
     * @return true if valid size
     */
    static bool isValidPrivateKey(const std::vector<uint8_t>& key);

    /**
     * @brief Validate that a vector is 32 or 33 bytes (valid public key size)
     *
     * @param key Key to validate
     * @return true if valid size
     */
    static bool isValidPublicKey(const std::vector<uint8_t>& key);

    /**
     * @brief Validate that a vector is 64 bytes (valid Schnorr signature size)
     *
     * @param sig Signature to validate
     * @return true if valid size
     */
    static bool isValidSignature(const std::vector<uint8_t>& sig);
};

} // namespace lightning
} // namespace dinero
