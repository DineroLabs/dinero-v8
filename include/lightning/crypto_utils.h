#pragma once

#include <array>
#include <vector>
#include <cstdint>
#include <string>

namespace dinero {
namespace lightning {
namespace crypto {

// Initialize cryptographic library (call once at startup)
void init_crypto();

// Secure random number generation (CSPRNG)
std::vector<uint8_t> secure_random_bytes(size_t num_bytes);
std::array<uint8_t, 32> secure_random_32();
std::array<uint8_t, 16> secure_random_16();
uint64_t secure_random_uint64();

// SHA256 hashing
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);
std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data);
std::array<uint8_t, 32> sha256(const std::string& data);
std::array<uint8_t, 32> sha256(const std::array<uint8_t, 32>& data);

// Double SHA256 (for Dinero compatibility)
std::array<uint8_t, 32> sha256_double(const uint8_t* data, size_t len);

// HMAC-SHA256
std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len,
                                     const uint8_t* data, size_t data_len);
std::array<uint8_t, 32> hmac_sha256(const std::array<uint8_t, 32>& key,
                                     const std::vector<uint8_t>& data);

// RIPEMD160 hash
std::array<uint8_t, 20> ripemd160(const uint8_t* data, size_t len);

// Hash160 (RIPEMD160(SHA256(x)))
std::array<uint8_t, 20> hash160(const uint8_t* data, size_t len);

// Secure memory operations
bool secure_memcmp(const void* a, const void* b, size_t len);
void secure_zero_memory(void* ptr, size_t len);

// Lightning-specific crypto functions
std::array<uint8_t, 32> generate_payment_preimage();
std::array<uint8_t, 32> generate_payment_secret();
std::array<uint8_t, 32> generate_payment_hash(const std::array<uint8_t, 32>& preimage);
bool verify_payment_preimage(const std::array<uint8_t, 32>& preimage,
                              const std::array<uint8_t, 32>& expected_hash);

// Key generation
std::array<uint8_t, 32> generate_private_key();

// Key derivation (HKDF-SHA256)
std::vector<uint8_t> hkdf_sha256(const std::vector<uint8_t>& input_key_material,
                                  const std::vector<uint8_t>& salt,
                                  const std::vector<uint8_t>& info,
                                  size_t output_len);

// Utility
bool is_crypto_initialized();

} // namespace crypto
} // namespace lightning
} // namespace dinero
