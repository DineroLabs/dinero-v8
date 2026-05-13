#pragma once

#include "wallet/key_identity.h"
#include <vector>
#include <string>
#include <optional>
#include <cstdint>

namespace dinero {
namespace wallet {

/**
 * Key origin information for BIP32 hierarchical deterministic keys.
 *
 * This is what enables deterministic re-derivation:
 * - Fingerprint identifies the master key
 * - Path specifies exact derivation from master
 *
 * Example: [f23a9c12/86'/1448'/0'/0/12]
 *   fingerprint: 0xf23a9c12 (first 4 bytes of master key's KeyID)
 *   path: [86 | HARDENED, 1448 | HARDENED, 0 | HARDENED, 0, 12]
 *
 * This metadata is stored in wallet DB and used to:
 * 1. Re-derive private keys on-demand (never stored)
 * 2. Export/import wallet descriptors
 * 3. Hardware wallet compatibility
 * 4. Multi-wallet fingerprint verification
 */
struct KeyOriginInfo {
    static constexpr uint32_t HARDENED_BIT = 0x80000000;

    uint32_t fingerprint;           // Master key fingerprint (first 4 bytes of master KeyID)
    std::vector<uint32_t> path;     // Full derivation path from master

    KeyOriginInfo() : fingerprint(0) {}
    KeyOriginInfo(uint32_t fp, const std::vector<uint32_t>& p)
        : fingerprint(fp), path(p) {}

    /**
     * Serialize to Bitcoin descriptor format: [fingerprint/path]
     * Example: [f23a9c12/86'/1448'/0'/0/12]
     */
    std::string toString() const;

    /**
     * Get derivation path as string: m/86'/1448'/0'/0/12
     */
    std::string getPathString() const;

    /**
     * Parse origin from string: "[f23a9c12/86'/1448'/0'/0/12]"
     */
    static std::optional<KeyOriginInfo> parse(const std::string& str);

    /**
     * Parse path from BIP32 path string: "m/86'/1448'/0'/0/12"
     * Creates KeyOriginInfo with fingerprint=0 (caller must set manually)
     *
     * @param path_str BIP32 path string (e.g., "m/86'/1448'/0'/0/12")
     * @return KeyOriginInfo with parsed path, fingerprint=0
     */
    static std::optional<KeyOriginInfo> parsePathString(const std::string& path_str);

    /**
     * Validation
     */
    bool isValid() const {
        return fingerprint != 0 && !path.empty();
    }

    /**
     * Path component accessors
     */
    uint32_t getPurpose() const {
        return path.empty() ? 0 : (path[0] & ~HARDENED_BIT);
    }

    uint32_t getCoinType() const {
        return path.size() < 2 ? 0 : (path[1] & ~HARDENED_BIT);
    }

    uint32_t getAccount() const {
        return path.size() < 3 ? 0 : (path[2] & ~HARDENED_BIT);
    }

    uint32_t getChange() const {
        return path.size() < 4 ? 0 : (path[3] & ~HARDENED_BIT);
    }

    uint32_t getIndex() const {
        return path.size() < 5 ? 0 : (path[4] & ~HARDENED_BIT);
    }

    /**
     * BIP standard detection
     */
    bool isBIP84() const { return getPurpose() == 84; }  // P2WPKH
    bool isBIP86() const { return getPurpose() == 86; }  // Taproot

    /**
     * Path manipulation
     */
    KeyOriginInfo withChildIndex(uint32_t index) const {
        KeyOriginInfo result = *this;
        if (!path.empty()) {
            result.path.back() = index;  // Replace last component
        } else {
            result.path.push_back(index);
        }
        return result;
    }

    /**
     * Equality
     */
    bool operator==(const KeyOriginInfo& other) const {
        return fingerprint == other.fingerprint && path == other.path;
    }

    bool operator!=(const KeyOriginInfo& other) const {
        return !(*this == other);
    }
};

/**
 * Complete key metadata stored in wallet database.
 *
 * This is what gets stored in the wallet_keys table.
 * Private keys are NEVER stored - only this metadata
 * which allows deterministic re-derivation from master seed.
 */
struct WalletKey {
    KeyID id;                           // Primary identifier (HASH160 of pubkey)
    KeyOriginInfo origin;               // Where this key came from
    bool spendable;                     // true = have master seed, false = watch-only
    std::optional<std::string> label;   // User label
    uint64_t created_at;                // Unix timestamp

    // Descriptor wallet fields
    std::optional<uint32_t> descriptor_id;      // Which descriptor produced this key
    std::optional<uint32_t> descriptor_index;   // Index within descriptor range

    // Taproot-specific: track both internal and output key IDs
    // - internal_key_id: the key before TapTweak (for derivation)
    // - output_key_id: the key after TapTweak (for script matching)
    std::optional<KeyID> output_key_id;         // Tweaked key (scriptPubKey)
    std::optional<KeyID> internal_key_id;       // Internal key (descriptor)

    WalletKey()
        : id{}, spendable(false), created_at(0) {}

    /**
     * Is this a Taproot key?
     */
    bool isTaproot() const {
        return output_key_id.has_value() && internal_key_id.has_value();
    }

    /**
     * Get the KeyID for script matching.
     * For Taproot: use output_key_id (tweaked)
     * For others: use id (internal)
     */
    KeyID getScriptMatchingKeyID() const {
        return output_key_id.value_or(id);
    }
};

} // namespace wallet
} // namespace dinero
