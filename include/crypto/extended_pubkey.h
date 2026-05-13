#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>

// Forward declarations
typedef struct secp256k1_context_struct secp256k1_context;

namespace dinero {
namespace crypto {

/**
 * @brief Extended Public Key (BIP32 xpub deserialization)
 *
 * Supports:
 * - xpub/zpub deserialization (base58check)
 * - Non-hardened BIP32 public key derivation only
 * - Address generation for BIP84/BIP86
 *
 * Security: NO private key material, watch-only only
 *
 * Phase 2 requirement for descriptor wallet active signing.
 */
class ExtendedPubKey {
public:
    static constexpr size_t CHAINCODE_SIZE = 32;
    static constexpr size_t PUBKEY_SIZE = 33; // Compressed
    static constexpr size_t HASH160_SIZE = 20;
    static constexpr uint32_t HARDENED_KEY_START = 0x80000000;

    // BIP32 version bytes (mainnet)
    static constexpr uint32_t VERSION_XPUB = 0x0488B21E;  // Legacy P2PKH
    static constexpr uint32_t VERSION_YPUB = 0x049D7CB2;  // P2SH-wrapped SegWit
    static constexpr uint32_t VERSION_ZPUB = 0x04B24746;  // BIP84 native SegWit

    /**
     * @brief Deserialize extended public key from string
     *
     * @param xpub_string Base58check encoded xpub/ypub/zpub
     * @return ExtendedPubKey Deserialized public key
     * @throws std::invalid_argument Invalid format, checksum, or version
     */
    static ExtendedPubKey FromString(const std::string& xpub_string);

    /**
     * @brief Derive child public key (non-hardened only)
     *
     * @param index Child index (must be < 0x80000000)
     * @return ExtendedPubKey Derived child key
     * @throws std::invalid_argument Hardened index not supported
     * @throws std::runtime_error Derivation failed
     */
    ExtendedPubKey Derive(uint32_t index) const;

    /**
     * @brief Get compressed public key bytes
     * @return std::vector<uint8_t> 33-byte compressed public key
     */
    std::vector<uint8_t> GetPublicKey() const;

    /**
     * @brief Get chain code
     * @return std::vector<uint8_t> 32-byte chain code
     */
    std::vector<uint8_t> GetChainCode() const;

    /**
     * @brief Get key fingerprint (first 4 bytes of HASH160(pubkey))
     * @return uint32_t Fingerprint in big-endian
     */
    uint32_t GetFingerprint() const;

    /**
     * @brief Get depth in derivation path
     * @return uint8_t Depth (0 = master)
     */
    uint8_t GetDepth() const { return depth_; }

    /**
     * @brief Get child number
     * @return uint32_t Child index used to derive this key
     */
    uint32_t GetChildNumber() const { return child_number_; }

    /**
     * @brief Get parent fingerprint
     * @return uint32_t Parent key fingerprint
     */
    uint32_t GetParentFingerprint() const { return parent_fingerprint_; }

    /**
     * @brief Serialize to xpub/zpub string
     *
     * @param version Version bytes (default: VERSION_ZPUB for BIP84)
     * @return std::string Base58check encoded xpub
     */
    std::string Serialize(uint32_t version = VERSION_ZPUB) const;

    /**
     * @brief Check if index is hardened
     * @param index Index to check
     * @return bool True if hardened
     */
    static bool IsHardened(uint32_t index) {
        return index >= HARDENED_KEY_START;
    }

private:
    std::array<uint8_t, CHAINCODE_SIZE> chain_code_;
    std::array<uint8_t, PUBKEY_SIZE> public_key_;
    uint32_t parent_fingerprint_;
    uint8_t depth_;
    uint32_t child_number_;

    // Private constructor (use FromString)
    ExtendedPubKey() : parent_fingerprint_(0), depth_(0), child_number_(0) {
        chain_code_.fill(0);
        public_key_.fill(0);
    }

    // Get global secp256k1 context
    static secp256k1_context* GetContext();
};

} // namespace crypto
} // namespace dinero
