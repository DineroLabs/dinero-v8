#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// BIP32 Hierarchical Deterministic Key Derivation
// ═══════════════════════════════════════════════════════════════════════════════
//
// This class consolidates the duplicated derive_hard/derive_norm lambdas that
// were scattered across 20+ locations in hd_wallet.cpp.
//
// SECURITY:
//   - RAII pattern ensures secp256k1 context cleanup
//   - Destructor zeroizes all key material (private key, chain code, intermediates)
//   - No key material left on stack after destruction
//
// USAGE:
//   BIP32Deriver deriver(seed, 64);
//   deriver.deriveHardened(86);      // m/86'
//   deriver.deriveHardened(1448);    // m/86'/1448'
//   deriver.deriveHardened(0);       // m/86'/1448'/0'
//   deriver.deriveNormal(0);         // m/86'/1448'/0'/0
//   deriver.deriveNormal(index);     // m/86'/1448'/0'/0/index
//   auto [privkey, pubkey] = deriver.getKeys();
//
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <array>

// Forward declaration to avoid secp256k1 header dependency
typedef struct secp256k1_context_struct secp256k1_context;

namespace dinero {

/**
 * @brief BIP32 hierarchical deterministic key derivation with secure memory handling
 *
 * Encapsulates the derivation state (private key, chain code) and provides
 * methods for hardened and normal derivation. Automatically zeroizes all
 * sensitive material on destruction.
 */
class BIP32Deriver {
public:
    // Hardened derivation offset (BIP32)
    static constexpr uint32_t HARDENED = 0x80000000;

    /**
     * @brief Initialize deriver from BIP32 seed
     * @param seed 64-byte BIP39 seed (from mnemonic + passphrase)
     * @param seed_len Length of seed (must be 64)
     * @throws std::runtime_error if seed_len != 64 or secp256k1 init fails
     */
    BIP32Deriver(const uint8_t* seed, size_t seed_len);

    /**
     * @brief Initialize deriver from existing key material
     * @param private_key 32-byte private key
     * @param chain_code 32-byte chain code
     * @throws std::runtime_error if key is invalid
     */
    BIP32Deriver(const uint8_t* private_key, const uint8_t* chain_code);

    /**
     * @brief Destructor - zeroizes all key material
     */
    ~BIP32Deriver();

    // Non-copyable (contains sensitive key material)
    BIP32Deriver(const BIP32Deriver&) = delete;
    BIP32Deriver& operator=(const BIP32Deriver&) = delete;

    // Movable
    BIP32Deriver(BIP32Deriver&& other) noexcept;
    BIP32Deriver& operator=(BIP32Deriver&& other) noexcept;

    /**
     * @brief Derive child key with hardened index
     * @param index Child index (will have 0x80000000 added)
     * @throws std::runtime_error on derivation failure
     *
     * Hardened derivation uses the private key as input, preventing
     * public key derivation from the parent public key.
     */
    void deriveHardened(uint32_t index);

    /**
     * @brief Derive child key with normal (non-hardened) index
     * @param index Child index
     * @throws std::runtime_error on derivation failure
     *
     * Normal derivation uses the public key as input, allowing
     * public key derivation from parent public key (watch-only wallets).
     */
    void deriveNormal(uint32_t index);

    /**
     * @brief Get the current private key (32 bytes)
     * @return Copy of private key
     *
     * WARNING: Caller is responsible for zeroizing the returned data.
     */
    std::array<uint8_t, 32> getPrivateKey() const;

    /**
     * @brief Get the current chain code (32 bytes)
     * @return Copy of chain code
     */
    std::array<uint8_t, 32> getChainCode() const;

    /**
     * @brief Get compressed public key (33 bytes)
     * @return Compressed SEC1 public key
     * @throws std::runtime_error on failure
     */
    std::array<uint8_t, 33> getCompressedPubkey() const;

    /**
     * @brief Get x-only public key for Taproot (32 bytes)
     * @return X-coordinate of public key
     * @throws std::runtime_error on failure
     */
    std::array<uint8_t, 32> getXOnlyPubkey() const;

    /**
     * @brief Check if the current key is valid
     * @return true if private key is valid on secp256k1 curve
     */
    bool isValid() const;

private:
    secp256k1_context* ctx_;
    uint8_t k_[32];          // Private key
    uint8_t c_[32];          // Chain code
    uint8_t I_[64];          // Intermediate HMAC buffer

    void zeroize();
    void deriveChild(uint32_t index, bool hardened);
};

// Helper function for common derivation patterns
inline uint32_t hardened(uint32_t index) {
    return index | BIP32Deriver::HARDENED;
}

} // namespace dinero
