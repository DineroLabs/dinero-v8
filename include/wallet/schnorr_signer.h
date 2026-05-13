#pragma once
#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace din {

/**
 * @brief Schnorr signature implementation (BIP-340)
 * 
 * Implements 64-byte Schnorr signatures for Taproot with:
 * - Deterministic nonce generation (RFC 6979)
 * - Batch verification support
 * - Taproot-specific adaptations
 */
class SchnorrSigner {
public:
    /**
     * @brief Sign a message hash with Schnorr signature
     * 
     * @param message_hash 32-byte message hash
     * @param private_key 32-byte private key
     * @param aux_rand Optional 32-byte auxiliary randomness
     * @return 64-byte Schnorr signature, or nullopt on failure
     */
    static std::optional<std::vector<uint8_t>> sign(
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& private_key,
        const std::optional<std::vector<uint8_t>>& aux_rand = std::nullopt
    );
    
    /**
     * @brief Verify a Schnorr signature
     * 
     * @param signature 64-byte signature
     * @param message_hash 32-byte message hash
     * @param public_key 32-byte x-only public key
     * @return true if signature is valid
     */
    static bool verify(
        const std::vector<uint8_t>& signature,
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& public_key
    );
    
    /**
     * @brief Batch verify multiple Schnorr signatures
     * 
     * @param signatures Vector of 64-byte signatures
     * @param message_hashes Vector of 32-byte message hashes
     * @param public_keys Vector of 32-byte public keys
     * @return true if all signatures are valid
     */
    static bool batchVerify(
        const std::vector<std::vector<uint8_t>>& signatures,
        const std::vector<std::vector<uint8_t>>& message_hashes,
        const std::vector<std::vector<uint8_t>>& public_keys
    );
    
    /**
     * @brief Generate deterministic nonce (RFC 6979)
     * 
     * @param private_key 32-byte private key
     * @param message_hash 32-byte message hash
     * @param aux_rand Optional 32-byte auxiliary randomness
     * @return 32-byte nonce
     */
    static std::vector<uint8_t> generateNonce(
        const std::vector<uint8_t>& private_key,
        const std::vector<uint8_t>& message_hash,
        const std::optional<std::vector<uint8_t>>& aux_rand = std::nullopt
    );
    
    /**
     * @brief Compute public key from private key
     * 
     * @param private_key 32-byte private key
     * @return 32-byte x-only public key
     */
    static std::vector<uint8_t> getPublicKey(const std::vector<uint8_t>& private_key);
    
    /**
     * @brief Validate Schnorr signature format
     * 
     * @param signature Signature to validate
     * @return true if signature has correct format
     */
    static bool isValidSignatureFormat(const std::vector<uint8_t>& signature);
    
    /**
     * @brief Validate x-only public key format
     * 
     * @param public_key Public key to validate
     * @return true if public key has correct format
     */
    static bool isValidPublicKeyFormat(const std::vector<uint8_t>& public_key);

private:
    /**
     * @brief Hash function for nonce generation
     */
    static std::vector<uint8_t> hash(const std::vector<uint8_t>& data);
    
    /**
     * @brief Point multiplication on secp256k1
     */
    static std::vector<uint8_t> pointMultiply(
        const std::vector<uint8_t>& point,
        const std::vector<uint8_t>& scalar
    );
    
    /**
     * @brief Point addition on secp256k1
     */
    static std::vector<uint8_t> pointAdd(
        const std::vector<uint8_t>& point1,
        const std::vector<uint8_t>& point2
    );
};

/**
 * @brief Taproot key tweaking utilities (BIP-341)
 * 
 * Implements Taproot key tweaking for script commitments
 * and output key computation.
 */
class TaprootTweaking {
public:
    /**
     * @brief Compute Taproot output key
     * 
     * @param internal_pubkey 32-byte internal public key
     * @param merkle_root Optional 32-byte Merkle root of script tree
     * @return 32-byte Taproot output public key
     */
    static std::vector<uint8_t> computeOutputKey(
        const std::vector<uint8_t>& internal_pubkey,
        const std::optional<std::vector<uint8_t>>& merkle_root = std::nullopt
    );
    
    /**
     * @brief Compute Taproot internal key
     * 
     * @param output_pubkey 32-byte Taproot output public key
     * @param merkle_root Optional 32-byte Merkle root of script tree
     * @return 32-byte internal public key
     */
    static std::vector<uint8_t> computeInternalKey(
        const std::vector<uint8_t>& output_pubkey,
        const std::optional<std::vector<uint8_t>>& merkle_root = std::nullopt
    );
    
    /**
     * @brief Compute Taproot tweak
     * 
     * @param merkle_root Optional 32-byte Merkle root of script tree
     * @return 32-byte tweak value
     */
    static std::vector<uint8_t> computeTweak(
        const std::optional<std::vector<uint8_t>>& merkle_root = std::nullopt
    );
    
    /**
     * @brief Verify Taproot key relationship
     * 
     * @param output_pubkey 32-byte Taproot output public key
     * @param internal_pubkey 32-byte internal public key
     * @param merkle_root Optional 32-byte Merkle root of script tree
     * @return true if keys have correct Taproot relationship
     */
    static bool verifyKeyRelationship(
        const std::vector<uint8_t>& output_pubkey,
        const std::vector<uint8_t>& internal_pubkey,
        const std::optional<std::vector<uint8_t>>& merkle_root = std::nullopt
    );

private:
    /**
     * @brief Tagged hash function (BIP-340)
     */
    static std::vector<uint8_t> taggedHash(
        const std::string& tag,
        const std::vector<uint8_t>& data
    );
    
    /**
     * @brief Compute Taproot tweak hash
     */
    static std::vector<uint8_t> computeTweakHash(
        const std::optional<std::vector<uint8_t>>& merkle_root
    );
};

} // namespace din
