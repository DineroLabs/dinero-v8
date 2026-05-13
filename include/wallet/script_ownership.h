#pragma once

#include "wallet/key_identity.h"
#include "wallet/key_origin.h"
#include <vector>
#include <optional>

namespace dinero {
namespace wallet {

// Forward declaration
class WalletKeyStore;

/**
 * Script ownership classification - Bitcoin Core compatible.
 *
 * This replaces boolean "ismine" with a 3-state model:
 * - NO: Not our script
 * - WATCH_ONLY: We recognize it but can't spend (no privkey)
 * - SPENDABLE: We can spend it (have master seed)
 */
enum class ScriptOwnership {
    NO = 0,          // Not ours
    WATCH_ONLY = 1,  // Ours to watch, but can't spend (no privkey)
    SPENDABLE = 2,   // Ours and spendable (have privkey)
};

/**
 * Bitcoin's IsMine logic - determines wallet's relationship to a script.
 *
 * This is the KEY to solving the Taproot spending bug:
 * - Replaces address-based ownership with KeyID-based ownership
 * - Handles Taproot tweaked keys correctly (output_key_id matching)
 * - Returns SPENDABLE only if we have master seed for re-derivation
 *
 * Usage:
 *   ScriptOwnershipResolver resolver(&keystore);
 *   auto ownership = resolver.IsMine(scriptPubKey);
 *   if (ownership == ScriptOwnership::SPENDABLE) {
 *       // We can spend this UTXO
 *   }
 */
class ScriptOwnershipResolver {
public:
    explicit ScriptOwnershipResolver(WalletKeyStore* keystore);

    /**
     * Primary interface - replaces address-based ownership checks.
     *
     * Bitcoin Core equivalent: IsMine(script)
     *
     * @param scriptPubKey Script to check (from UTXO)
     * @return Ownership level (NO, WATCH_ONLY, SPENDABLE)
     */
    ScriptOwnership IsMine(const std::vector<uint8_t>& scriptPubKey) const;

    /**
     * Extract key IDs from script (may be multiple for multisig).
     *
     * For P2WPKH: extracts 1 KeyID from pubkey hash
     * For P2TR: extracts output_key_id from tweaked key
     *
     * @param scriptPubKey Script to analyze
     * @return Vector of KeyIDs referenced by script (empty if unrecognized)
     */
    std::vector<KeyID> ExtractKeyIDs(const std::vector<uint8_t>& scriptPubKey) const;

    /**
     * Check if we have a specific key.
     *
     * @param key_id KeyID to check
     * @return true if key exists in wallet
     */
    bool HaveKey(const KeyID& key_id) const;

    /**
     * Get key origin info (for deterministic re-derivation).
     *
     * @param key_id KeyID to lookup
     * @return KeyOriginInfo if found, nullopt otherwise
     */
    std::optional<KeyOriginInfo> GetKeyOrigin(const KeyID& key_id) const;

private:
    WalletKeyStore* keystore_;

    // ═══ Script Type Detection ═══

    /**
     * Check if script is P2WPKH (BIP84).
     * Format: 0x0014 <20-byte-pubkey-hash>
     */
    bool IsP2WPKH(const std::vector<uint8_t>& script) const;

    /**
     * Check if script is P2TR (BIP86 Taproot).
     * Format: 0x5120 <32-byte-tweaked-pubkey>
     */
    bool IsP2TR(const std::vector<uint8_t>& script) const;

    // ═══ Key Extraction ═══

    /**
     * Extract KeyID from P2WPKH script.
     *
     * P2WPKH format: 0x00 0x14 <20-byte-hash>
     * The hash IS the KeyID (HASH160 of pubkey).
     *
     * @param script P2WPKH scriptPubKey
     * @return KeyID if valid P2WPKH, nullopt otherwise
     */
    std::optional<KeyID> ExtractP2WPKHKeyID(const std::vector<uint8_t>& script) const;

    /**
     * Extract output_key_id from P2TR script.
     *
     * P2TR format: 0x51 0x20 <32-byte-tweaked-pubkey>
     *
     * CRITICAL: The script contains the TWEAKED output key, not internal key.
     * We compute output_key_id = HASH160(tweaked_key) and match against
     * the output_key_id column in wallet_keys (set during address generation).
     *
     * @param script P2TR scriptPubKey
     * @return output_key_id if valid P2TR, nullopt otherwise
     */
    std::optional<KeyID> ExtractP2TRKeyID(const std::vector<uint8_t>& script) const;
};

} // namespace wallet
} // namespace dinero
