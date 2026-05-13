#include "crypto/pbkdf2.h"
#include "crypto/dinero_crypto_minimal.h"
#include <cstring>
#include <algorithm>
#include <vector>

namespace dinero {
namespace crypto {

// PBKDF2-HMAC-SHA512 as per RFC 2898
void PBKDF2_HMAC_SHA512(
    const uint8_t* password, size_t password_len,
    const uint8_t* salt, size_t salt_len,
    uint32_t iterations,
    uint8_t* output, size_t output_len
) {
    constexpr size_t HASH_LEN = 64; // SHA-512 output size
    
    uint8_t U[HASH_LEN];
    uint8_t T[HASH_LEN];
    std::vector<uint8_t> salt_block(salt_len + 4);
    
    // Copy salt for block construction
    std::memcpy(salt_block.data(), salt, salt_len);
    
    // Number of blocks needed
    size_t blocks = (output_len + HASH_LEN - 1) / HASH_LEN;
    
    for (uint32_t block = 1; block <= blocks; block++) {
        // Append block number to salt (big-endian)
        salt_block[salt_len + 0] = (block >> 24) & 0xFF;
        salt_block[salt_len + 1] = (block >> 16) & 0xFF;
        salt_block[salt_len + 2] = (block >> 8) & 0xFF;
        salt_block[salt_len + 3] = block & 0xFF;

        // First iteration: U_1 = HMAC(password, salt || block)
        hmac_sha512(password, password_len, salt_block.data(), salt_len + 4, U);
        std::memcpy(T, U, HASH_LEN);

        // Subsequent iterations: U_i = HMAC(password, U_{i-1})
        for (uint32_t iter = 1; iter < iterations; iter++) {
            hmac_sha512(password, password_len, U, HASH_LEN, U);
            
            // XOR into T
            for (size_t i = 0; i < HASH_LEN; i++) {
                T[i] ^= U[i];
            }
        }
        
        // Copy to output
        size_t offset = (block - 1) * HASH_LEN;
        size_t to_copy = std::min(HASH_LEN, output_len - offset);
        std::memcpy(output + offset, T, to_copy);
    }
}

} // namespace crypto
} // namespace dinero

