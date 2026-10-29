#pragma once

#include <cstdint>
#include <cstddef>

namespace dinero {
namespace crypto {

// PBKDF2-HMAC-SHA512 implementation
// Used for BIP39 and wallet encryption; binary lengths are explicit.
// Nonempty output requires nonnull buffers for nonzero lengths, at least one
// iteration, password_len <= INT_MAX, salt_len + 4 representable, and no more
// than UINT32_MAX SHA-512 output blocks. Invalid parameters throw invalid_argument.
// Provider failures throw runtime_error. Output is unchanged on any exception.
// Zero output length is a no-op. Password/salt may overlap the output buffer.
void PBKDF2_HMAC_SHA512(
    const uint8_t* password, size_t password_len,
    const uint8_t* salt, size_t salt_len,
    uint32_t iterations,
    uint8_t* output, size_t output_len
);

} // namespace crypto
} // namespace dinero

