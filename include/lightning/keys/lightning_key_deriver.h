#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// Lightning Key Deriver - BIP32-based implementation
// ═══════════════════════════════════════════════════════════════════════════════
//
// Implements ILightningKeyProvider using BIP32Deriver for deterministic
// key derivation. All keys are derived from the wallet's HD seed but are
// logically separate from on-chain wallet operations.
//
// Thread Safety: NOT thread-safe. Create one instance per thread if needed.
//
// Memory Security: All intermediate key material is zeroized via BIP32Deriver's
// RAII destructor (OPENSSL_cleanse).
//
// ═══════════════════════════════════════════════════════════════════════════════

#include "lightning/keys/ilightning_key_provider.h"
#include <cstdint>
#include <vector>

namespace dinero {
namespace lightning {

/**
 * @brief BIP32-based Lightning key derivation
 *
 * Derives all Lightning protocol keys from an HD seed using standardized
 * BIP32 derivation paths. Keys are deterministic and recoverable from
 * the seed phrase.
 */
class LightningKeyDeriver : public ILightningKeyProvider {
public:
    /**
     * @brief Construct deriver from HD seed
     * @param seed 64-byte BIP39 seed
     * @param seed_len Length of seed (must be 64)
     * @param coin_type SLIP-44 coin type (1448 for Dinero)
     */
    LightningKeyDeriver(const uint8_t* seed, size_t seed_len, uint32_t coin_type);

    /**
     * @brief Construct deriver from seed vector
     * @param seed 64-byte BIP39 seed
     * @param coin_type SLIP-44 coin type (1448 for Dinero)
     */
    LightningKeyDeriver(const std::vector<uint8_t>& seed, uint32_t coin_type);

    ~LightningKeyDeriver() override;

    // Non-copyable (holds seed reference)
    LightningKeyDeriver(const LightningKeyDeriver&) = delete;
    LightningKeyDeriver& operator=(const LightningKeyDeriver&) = delete;

    // Movable
    LightningKeyDeriver(LightningKeyDeriver&&) noexcept;
    LightningKeyDeriver& operator=(LightningKeyDeriver&&) noexcept;

    // =========================================================================
    // ILightningKeyProvider implementation
    // =========================================================================

    std::vector<uint8_t> GetFundingKey(uint32_t channel_index) override;
    std::vector<uint8_t> GetRevocationBaseKey(uint32_t channel_index) override;
    std::vector<uint8_t> GetPaymentBaseKey(uint32_t channel_index) override;
    std::vector<uint8_t> GetDelayedPaymentBaseKey(uint32_t channel_index) override;
    std::vector<uint8_t> GetHTLCBaseKey(uint32_t channel_index) override;

    std::vector<uint8_t> GetRevocationBasepointSecret(const std::string& channel_id) override;

    NodeIdentity GetNodeIdentity(uint32_t account = 0, uint32_t key_index = 0) override;

    std::vector<uint8_t> GetPublicKey(const std::vector<uint8_t>& private_key) override;

private:
    // Derive a channel key at given chain index
    std::vector<uint8_t> DeriveChannelKey(uint32_t chain, uint32_t channel_index);

    // Seed storage (zeroized on destruction)
    std::vector<uint8_t> seed_;
    uint32_t coin_type_;
};

} // namespace lightning
} // namespace dinero
