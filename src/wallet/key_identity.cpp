#include "wallet/key_identity.h"
#include <openssl/sha.h>
#include <openssl/ripemd.h>
#include <sstream>
#include <iomanip>
#include <cstring>

// OpenSSL 3.x deprecates the low-level RIPEMD160() function.
// Suppress until migrated to EVP API.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

namespace dinero {
namespace wallet {

KeyID ComputeKeyID(const std::vector<uint8_t>& pubkey) {
    if (pubkey.size() != 33) {
        throw std::invalid_argument("ComputeKeyID: pubkey must be 33 bytes (compressed)");
    }

    // Step 1: SHA256(pubkey)
    uint8_t sha256_hash[32];
    SHA256(pubkey.data(), pubkey.size(), sha256_hash);

    // Step 2: RIPEMD160(sha256_hash)
    uint8_t hash160[20];
    RIPEMD160(sha256_hash, 32, hash160);

    // Copy to KeyID
    KeyID result;
    std::copy(hash160, hash160 + 20, result.begin());

    return result;
}

KeyID ComputeKeyIDFromXOnly(const std::array<uint8_t, 32>& xonly_pubkey) {
    // Step 1: SHA256(xonly_pubkey)
    uint8_t sha256_hash[32];
    SHA256(xonly_pubkey.data(), xonly_pubkey.size(), sha256_hash);

    // Step 2: RIPEMD160(sha256_hash)
    uint8_t hash160[20];
    RIPEMD160(sha256_hash, 32, hash160);

    // Copy to KeyID
    KeyID result;
    std::copy(hash160, hash160 + 20, result.begin());

    return result;
}

std::string KeyIDToHex(const KeyID& key_id) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : key_id) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::optional<KeyID> KeyIDFromHex(const std::string& hex) {
    if (hex.length() != 40) {
        return std::nullopt;  // KeyID is 20 bytes = 40 hex chars
    }

    KeyID result;
    for (size_t i = 0; i < 20; ++i) {
        std::string byte_str = hex.substr(i * 2, 2);
        try {
            result[i] = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        } catch (...) {
            return std::nullopt;  // Invalid hex
        }
    }

    return result;
}

} // namespace wallet
} // namespace dinero
#pragma GCC diagnostic pop
