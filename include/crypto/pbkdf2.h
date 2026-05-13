#pragma once

#include <cstdint>
#include <cstddef>

namespace dinero {
namespace crypto {

// PBKDF2-HMAC-SHA512 implementation
// Used for BIP39 mnemonic → seed derivation
void PBKDF2_HMAC_SHA512(
    const uint8_t* password, size_t password_len,
    const uint8_t* salt, size_t salt_len,
    uint32_t iterations,
    uint8_t* output, size_t output_len
);

} // namespace crypto
} // namespace dinero

