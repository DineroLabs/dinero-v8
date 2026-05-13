/**
 * Payment Handle Encryption System
 *
 * Uses hybrid cryptography:
 * - ECDH (secp256k1) for key exchange
 * - AES-256-GCM for symmetric encryption
 *
 * Flow:
 * 1. Seller generates ephemeral ECDH key pair
 * 2. Buyer derives shared secret using seller's public key
 * 3. Encrypt payment handle with AES-256-GCM using derived key
 * 4. Only trading parties can decrypt payment details
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace din {
namespace p2p {

/**
 * Encrypted payment handle structure
 */
struct EncryptedPaymentHandle {
    std::string method_id;                   // Payment method (e.g., "zelle")
    std::string ephemeral_pubkey;           // Seller's ephemeral public key (hex)
    std::vector<uint8_t> ciphertext;        // Encrypted payment handle
    std::vector<uint8_t> nonce;             // AES-GCM nonce (12 bytes)
    std::vector<uint8_t> tag;               // AES-GCM auth tag (16 bytes)
    std::string hash;                       // SHA-256 hash for verification
    int64_t encrypted_at;                   // Timestamp

    std::string toBase64() const;
    static EncryptedPaymentHandle fromBase64(const std::string& b64);
};

/**
 * PaymentEncryption - Hybrid encryption for sensitive payment data
 */
class PaymentEncryption {
public:
    /**
     * Encrypt payment handle for offer
     *
     * @param plaintext Payment handle (phone/email/account)
     * @param method_id Payment method identifier
     * @return Encrypted handle with ephemeral pubkey
     */
    static EncryptedPaymentHandle encrypt(
        const std::string& plaintext,
        const std::string& method_id
    );

    /**
     * Decrypt payment handle using private key
     *
     * @param encrypted Encrypted payment handle
     * @param privkey_hex User's private key (hex)
     * @return Decrypted payment handle, or nullopt if decryption fails
     */
    static std::optional<std::string> decrypt(
        const EncryptedPaymentHandle& encrypted,
        const std::string& privkey_hex
    );

    /**
     * Verify encrypted handle integrity (check hash)
     *
     * @param encrypted Encrypted payment handle
     * @return true if hash matches ciphertext
     */
    static bool verify(const EncryptedPaymentHandle& encrypted);

    /**
     * Generate ephemeral ECDH key pair
     *
     * @return {privkey_hex, pubkey_hex}
     */
    static std::pair<std::string, std::string> generateEphemeralKeyPair();

    /**
     * Derive shared secret using ECDH
     *
     * @param privkey_hex Our private key
     * @param pubkey_hex Their public key
     * @return Shared secret (32 bytes)
     */
    static std::vector<uint8_t> deriveSharedSecret(
        const std::string& privkey_hex,
        const std::string& pubkey_hex
    );

    /**
     * AES-256-GCM encryption
     *
     * @param plaintext Data to encrypt
     * @param key 32-byte encryption key
     * @param nonce 12-byte nonce (generated if empty)
     * @return {ciphertext, nonce, tag}
     */
    static std::tuple<std::vector<uint8_t>, std::vector<uint8_t>, std::vector<uint8_t>>
    aesGcmEncrypt(
        const std::string& plaintext,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& nonce = {}
    );

    /**
     * AES-256-GCM decryption
     *
     * @param ciphertext Encrypted data
     * @param key 32-byte decryption key
     * @param nonce 12-byte nonce
     * @param tag 16-byte auth tag
     * @return Decrypted plaintext, or nullopt on failure
     */
    static std::optional<std::string> aesGcmDecrypt(
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& nonce,
        const std::vector<uint8_t>& tag
    );

private:
    static std::vector<uint8_t> generateNonce();
    static std::string computeHash(const std::vector<uint8_t>& data);
};

} // namespace p2p
} // namespace din
