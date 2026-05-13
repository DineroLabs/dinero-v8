#ifndef DINEROCOIN_WALLET_REFERENCE_CRYPTO_H
#define DINEROCOIN_WALLET_REFERENCE_CRYPTO_H

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace wallet {
namespace reference {
namespace crypto {

/**
 * BIP39 Mnemonic generation and validation
 */
class BIP39 {
public:
    /**
     * Generate random mnemonic
     * @param word_count 12, 15, 18, 21, or 24 words
     * @return Space-separated mnemonic phrase
     */
    static std::string GenerateMnemonic(int word_count = 24);

    /**
     * Validate mnemonic format and checksum
     * @param mnemonic Space-separated words
     * @return true if valid
     */
    static bool ValidateMnemonic(const std::string& mnemonic);

    /**
     * Convert mnemonic to seed (BIP39)
     * @param mnemonic Space-separated words
     * @param passphrase Optional passphrase (default empty)
     * @return 64-byte seed
     */
    static std::vector<uint8_t> MnemonicToSeed(
        const std::string& mnemonic,
        const std::string& passphrase = ""
    );
};

/**
 * BIP32 HD Key Derivation
 */
class BIP32 {
public:
    struct ExtendedKey {
        std::vector<uint8_t> private_key;  // 32 bytes
        std::vector<uint8_t> chain_code;   // 32 bytes
        std::vector<uint8_t> public_key;   // 33 bytes (compressed)
    };

    /**
     * Derive master key from seed
     * @param seed 64-byte seed from BIP39
     * @return Master extended key
     */
    static ExtendedKey MasterKeyFromSeed(const std::vector<uint8_t>& seed);

    /**
     * Derive child key
     * @param parent Parent extended key
     * @param index Child index (use 0x80000000+ for hardened)
     * @return Child extended key
     */
    static ExtendedKey DeriveChild(
        const ExtendedKey& parent,
        uint32_t index
    );

    /**
     * Derive key from path (e.g., "m/84'/1448'/0'/0/0")
     * @param master Master extended key
     * @param path Derivation path
     * @return Derived extended key
     */
    static ExtendedKey DerivePath(
        const ExtendedKey& master,
        const std::string& path
    );
};

/**
 * Elliptic Curve operations (secp256k1)
 */
class ECC {
public:
    /**
     * Generate random private key
     * @return 32-byte private key
     */
    static std::vector<uint8_t> GeneratePrivateKey();

    /**
     * Derive public key from private key
     * @param private_key 32-byte private key
     * @param compressed Return compressed format (33 bytes)
     * @return Public key
     */
    static std::vector<uint8_t> DerivePublicKey(
        const std::vector<uint8_t>& private_key,
        bool compressed = true
    );

    /**
     * Sign message with private key (ECDSA)
     * @param private_key 32-byte private key
     * @param message_hash 32-byte message hash
     * @return 64-byte signature (r||s)
     */
    static std::vector<uint8_t> Sign(
        const std::vector<uint8_t>& private_key,
        const std::vector<uint8_t>& message_hash
    );

    /**
     * Verify ECDSA signature
     * @param public_key Compressed public key
     * @param message_hash 32-byte message hash
     * @param signature 64-byte signature
     * @return true if valid
     */
    static bool Verify(
        const std::vector<uint8_t>& public_key,
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& signature
    );
};

/**
 * Address encoding/decoding (Bech32)
 */
class Address {
public:
    /**
     * Encode public key as Bech32 address
     * @param public_key 33-byte compressed public key
     * @param hrp Human-readable part (e.g., "din")
     * @param witness_version Witness version (0 for SegWit)
     * @return Bech32 address (e.g., "din1q...")
     */
    static std::string Encode(
        const std::vector<uint8_t>& public_key_hash,
        const std::string& hrp = "din",
        int witness_version = 0
    );

    /**
     * Decode Bech32 address
     * @param address Bech32 address
     * @return Public key hash (20 bytes for P2WPKH)
     */
    static std::vector<uint8_t> Decode(const std::string& address);

    /**
     * Validate Bech32 address format
     * @param address Address to validate
     * @param hrp Expected HRP (default "din")
     * @return true if valid
     */
    static bool Validate(
        const std::string& address,
        const std::string& hrp = "din"
    );

    /**
     * Create P2WPKH address from public key
     * @param public_key 33-byte compressed public key
     * @return Bech32 address
     */
    static std::string PublicKeyToAddress(
        const std::vector<uint8_t>& public_key
    );
};

/**
 * Hashing utilities
 */
class Hash {
public:
    /**
     * SHA256 hash
     * @param data Input data
     * @return 32-byte hash
     */
    static std::vector<uint8_t> SHA256(const std::vector<uint8_t>& data);

    /**
     * Double SHA256 (Bitcoin-style)
     * @param data Input data
     * @return 32-byte hash
     */
    static std::vector<uint8_t> Hash256(const std::vector<uint8_t>& data);

    /**
     * RIPEMD160 hash
     * @param data Input data
     * @return 20-byte hash
     */
    static std::vector<uint8_t> RIPEMD160(const std::vector<uint8_t>& data);

    /**
     * Hash160 (SHA256 then RIPEMD160)
     * @param data Input data
     * @return 20-byte hash
     */
    static std::vector<uint8_t> Hash160(const std::vector<uint8_t>& data);

    /**
     * HMAC-SHA512
     * @param key HMAC key
     * @param data Input data
     * @return 64-byte HMAC
     */
    static std::vector<uint8_t> HMAC_SHA512(
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& data
    );
};

/**
 * WIF (Wallet Import Format) encoding/decoding
 */
class WIF {
public:
    /**
     * Encode private key as WIF
     * @param private_key 32-byte private key
     * @param compressed Use compressed public key
     * @param version Version byte (0x80 for mainnet)
     * @return WIF string
     */
    static std::string Encode(
        const std::vector<uint8_t>& private_key,
        bool compressed = true,
        uint8_t version = 0x80
    );

    /**
     * Decode WIF to private key
     * @param wif WIF string
     * @return Private key (32 bytes)
     */
    static std::vector<uint8_t> Decode(const std::string& wif);
};

} // namespace crypto
} // namespace reference
} // namespace wallet
} // namespace dinero

#endif // DINEROCOIN_WALLET_REFERENCE_CRYPTO_H
