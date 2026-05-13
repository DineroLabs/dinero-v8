#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <string>
#include <algorithm>
#include <optional>
#include <cstring>

namespace dinero {
namespace wallet {

/**
 * Primary wallet key identifier - Bitcoin Core compatible.
 *
 * KeyID = RIPEMD160(SHA256(pubkey))
 *
 * This is the FOUNDATION of descriptor wallets:
 * - Stable identity for keys (doesn't change with address format)
 * - Used for script ownership queries (IsMine)
 * - Links scripts → keys → master seed
 *
 * For Taproot: KeyID is computed from the INTERNAL x-only pubkey,
 * not the tweaked output key.
 */
using KeyID = std::array<uint8_t, 20>;  // 20 bytes (160 bits)

/**
 * Compute KeyID from compressed public key (33 bytes).
 *
 * @param pubkey Compressed public key (0x02/0x03 prefix + 32 bytes)
 * @return KeyID = RIPEMD160(SHA256(pubkey))
 */
KeyID ComputeKeyID(const std::vector<uint8_t>& pubkey);

/**
 * Compute KeyID from x-only public key (32 bytes) - Taproot.
 *
 * @param xonly_pubkey X-only public key (32 bytes, no prefix)
 * @return KeyID = RIPEMD160(SHA256(xonly_pubkey))
 *
 * CRITICAL: For Taproot, this must be the INTERNAL key,
 * not the tweaked output key. The internal key is what
 * appears in the descriptor and key origin.
 */
KeyID ComputeKeyIDFromXOnly(const std::array<uint8_t, 32>& xonly_pubkey);

/**
 * Convert KeyID to hex string for debugging/display.
 *
 * @param key_id KeyID to convert
 * @return Hex string (40 characters)
 */
std::string KeyIDToHex(const KeyID& key_id);

/**
 * Parse KeyID from hex string.
 *
 * @param hex Hex string (40 characters)
 * @return KeyID if valid, nullopt otherwise
 */
std::optional<KeyID> KeyIDFromHex(const std::string& hex);

/**
 * Compare KeyIDs for equality (needed for map lookups).
 * std::array already has operator== so these are not needed,
 * but included for clarity and future custom implementations.
 */
inline bool operator==(const KeyID& a, const KeyID& b) {
    return std::equal(a.begin(), a.end(), b.begin());
}

inline bool operator!=(const KeyID& a, const KeyID& b) {
    return !(a == b);
}

inline bool operator<(const KeyID& a, const KeyID& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

} // namespace wallet
} // namespace dinero

// Hash specialization for std::unordered_map<KeyID, ...>
namespace std {
    template<>
    struct hash<dinero::wallet::KeyID> {
        size_t operator()(const dinero::wallet::KeyID& key_id) const {
            // Use first 8 bytes of KeyID as hash
            size_t result = 0;
            memcpy(&result, key_id.data(), std::min(sizeof(size_t), key_id.size()));
            return result;
        }
    };
}
