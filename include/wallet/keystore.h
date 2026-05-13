#pragma once

#include "wallet/key_identity.h"
#include "wallet/key_origin.h"
#include <vector>
#include <optional>

namespace dinero {
namespace wallet {

/**
 * Wallet key storage interface - knows which keys we have and where they came from.
 *
 * CRITICAL DESIGN PRINCIPLE: Does NOT store private keys.
 * Only stores metadata (KeyID + KeyOriginInfo) for deterministic derivation.
 *
 * Private keys are derived on-demand from:
 *   master_seed + KeyOriginInfo → private_key
 *
 * This is Bitcoin Core's descriptor wallet architecture.
 */
class WalletKeyStore {
public:
    virtual ~WalletKeyStore() = default;

    // ═══ Key Queries ═══

    /**
     * Check if we have a key.
     *
     * @param key_id KeyID to check
     * @return true if key exists in wallet
     */
    virtual bool HaveKey(const KeyID& key_id) const = 0;

    /**
     * Get key metadata (not private key).
     *
     * @param key_id KeyID to lookup
     * @return WalletKey metadata if found, nullopt otherwise
     */
    virtual std::optional<WalletKey> GetKey(const KeyID& key_id) const = 0;

    /**
     * Get all keys in wallet.
     *
     * @return Vector of all WalletKey metadata
     */
    virtual std::vector<WalletKey> GetAllKeys() const = 0;

    /**
     * Lookup key by output_key_id (for Taproot).
     *
     * When matching Taproot scriptPubKeys, we need to find the key
     * by its tweaked output_key_id, not internal_key_id.
     *
     * @param output_key_id Tweaked output KeyID from scriptPubKey
     * @return WalletKey metadata if found, nullopt otherwise
     */
    virtual std::optional<WalletKey> GetKeyByOutputKeyID(const KeyID& output_key_id) const = 0;

    // ═══ Key Addition ═══

    /**
     * Add key metadata to wallet.
     *
     * @param key WalletKey metadata to add
     * @return true if added successfully
     */
    virtual bool AddKey(const WalletKey& key) = 0;

    // ═══ Master Seed Access ═══

    /**
     * Check if wallet has master seed (for spending).
     *
     * @return true if master seed available (SPENDABLE wallet)
     *         false if watch-only (WATCH_ONLY wallet)
     */
    virtual bool HaveMasterSeed() const = 0;

    /**
     * Get master seed for key derivation.
     *
     * SECURITY: This is highly sensitive. Only use for deriving private keys.
     *
     * @return Master seed bytes if available, nullopt if watch-only
     */
    virtual std::optional<std::vector<uint8_t>> GetMasterSeed() const = 0;

    // ═══ Key Derivation ═══

    /**
     * Derive private key on-demand from KeyOriginInfo.
     *
     * This is the core of descriptor wallets:
     *   master_seed + KeyOriginInfo → private_key
     *
     * Example:
     *   origin = [f23a9c12/86'/1448'/0'/0/12]
     *   privkey = master.derive(86').derive(1448').derive(0').derive(0).derive(12)
     *
     * @param origin KeyOriginInfo specifying derivation path
     * @return Private key bytes if derivation succeeds, nullopt otherwise
     */
    virtual std::optional<std::vector<uint8_t>> DerivePrivateKey(
        const KeyOriginInfo& origin) const = 0;
};

} // namespace wallet
} // namespace dinero
