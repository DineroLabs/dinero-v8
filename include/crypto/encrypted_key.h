#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstdint>

// Forward declaration from decrypt_encrypted_key.h
namespace dinero::crypto {
    struct EncryptedKeyParams;
    struct DecryptResult;
}

namespace dinero::crypto {

/**
 * Decrypt an encrypted private key
 * 
 * @param params Encryption parameters
 * @param private_key Output buffer for decrypted 32-byte private key
 * @return true on success, false on failure
 */
bool DecryptPrivateKey(const EncryptedKeyParams& params, 
                       std::array<uint8_t, 32>& private_key);

/**
 * Parse base64-encoded data
 * 
 * @param base64_data Base64 string
 * @param output Output vector
 * @return true on success, false on invalid base64
 */
bool ParseBase64(const std::string& base64_data, std::vector<uint8_t>& output);

/**
 * Securely zero memory
 * 
 * @param ptr Pointer to memory
 * @param size Size in bytes
 */
void SecureZero(void* ptr, size_t size);

/**
 * Securely zero vector
 */
template<typename T>
void SecureZero(std::vector<T>& vec) {
    if (!vec.empty()) {
        SecureZero(vec.data(), vec.size() * sizeof(T));
        vec.clear();
    }
}

/**
 * Securely zero array
 */
template<typename T, size_t N>
void SecureZero(std::array<T, N>& arr) {
    SecureZero(arr.data(), N * sizeof(T));
}

/**
 * Securely zero string
 */
inline void SecureZero(std::string& str) {
    if (!str.empty()) {
        SecureZero(const_cast<char*>(str.data()), str.size());
        str.clear();
    }
}

} // namespace dinero::crypto
