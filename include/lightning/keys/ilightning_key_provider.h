#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// Lightning Network Key Provider Interface
// ═══════════════════════════════════════════════════════════════════════════════
//
// This interface defines the key derivation surface for Lightning Network
// protocol operations. It is intentionally separate from the wallet:
//
//   - Lightning keys are NOT UTXO-backed
//   - Lightning keys are NOT spendable via wallet
//   - Lightning keys are NOT subject to wallet ownership invariants
//   - Lightning keys are NOT imported/exported as addresses
//
// They are PROTOCOL KEYS for channel lifecycle and node identity.
//
// Derivation paths (all use BIP84 base):
//   Chain 3: m/84'/coin'/0'/3/idx  - Funding keys (MuSig2 aggregate)
//   Chain 4: m/84'/coin'/0'/4/idx  - Revocation base keys
//   Chain 5: m/84'/coin'/0'/5/idx  - Payment base keys
//   Chain 6: m/84'/coin'/0'/6/idx  - Delayed payment base keys
//   Chain 7: m/84'/coin'/0'/7/idx  - HTLC base keys
//   m/84'/coin'/9735'/acct'/key'   - Node identity (all hardened)
//
// ═══════════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <vector>
#include <string>

namespace dinero {
namespace lightning {

/**
 * @brief Interface for Lightning Network key derivation
 *
 * Implementations provide deterministic key derivation for all Lightning
 * protocol operations. Keys are derived from an HD seed but are logically
 * separate from on-chain wallet keys.
 */
class ILightningKeyProvider {
public:
    virtual ~ILightningKeyProvider() = default;

    // =========================================================================
    // Channel Keys (per-channel, indexed by channel_index)
    // =========================================================================

    /**
     * @brief Get funding key for channel (m/84'/coin'/0'/3/channel_index)
     * @param channel_index Channel index
     * @return 32-byte private key for MuSig2 aggregate funding key
     */
    virtual std::vector<uint8_t> GetFundingKey(uint32_t channel_index) = 0;

    /**
     * @brief Get revocation base key (m/84'/coin'/0'/4/channel_index)
     * @param channel_index Channel index
     * @return 32-byte private key for penalty/justice transactions
     */
    virtual std::vector<uint8_t> GetRevocationBaseKey(uint32_t channel_index) = 0;

    /**
     * @brief Get payment base key (m/84'/coin'/0'/5/channel_index)
     * @param channel_index Channel index
     * @return 32-byte private key for HTLC outputs
     */
    virtual std::vector<uint8_t> GetPaymentBaseKey(uint32_t channel_index) = 0;

    /**
     * @brief Get delayed payment base key (m/84'/coin'/0'/6/channel_index)
     * @param channel_index Channel index
     * @return 32-byte private key for to_self outputs with CSV delay
     */
    virtual std::vector<uint8_t> GetDelayedPaymentBaseKey(uint32_t channel_index) = 0;

    /**
     * @brief Get HTLC base key (m/84'/coin'/0'/7/channel_index)
     * @param channel_index Channel index
     * @return 32-byte private key for HTLC transaction signing
     */
    virtual std::vector<uint8_t> GetHTLCBaseKey(uint32_t channel_index) = 0;

    // =========================================================================
    // Revocation Secrets (per-channel, keyed by channel_id)
    // =========================================================================

    /**
     * @brief Get revocation basepoint secret for justice transactions
     *
     * Derives: HMAC-SHA256(wallet_seed, "dinero-lightning-revocation" || channel_id)
     *
     * @param channel_id Channel identifier (hex string)
     * @return 32-byte revocation basepoint secret
     */
    virtual std::vector<uint8_t> GetRevocationBasepointSecret(const std::string& channel_id) = 0;

    // =========================================================================
    // Node Identity (persistent across restarts)
    // =========================================================================

    /**
     * @brief Node identity keypair for Lightning gossip and peer auth
     */
    struct NodeIdentity {
        std::vector<uint8_t> privkey;  // 32-byte private key
        std::vector<uint8_t> pubkey;   // 33-byte compressed public key
    };

    /**
     * @brief Get Lightning node identity (m/84'/coin'/9735'/account'/key_index')
     * @param account Account index (0 = primary node)
     * @param key_index Key index (0 = primary identity)
     * @return Node identity keypair
     */
    virtual NodeIdentity GetNodeIdentity(uint32_t account = 0, uint32_t key_index = 0) = 0;

    // =========================================================================
    // Utility
    // =========================================================================

    /**
     * @brief Derive compressed public key from private key
     * @param private_key 32-byte private key
     * @return 33-byte compressed public key
     */
    virtual std::vector<uint8_t> GetPublicKey(const std::vector<uint8_t>& private_key) = 0;
};

} // namespace lightning
} // namespace dinero
