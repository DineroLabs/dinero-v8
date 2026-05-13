#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

/**
 * Wallet encryption utilities using Argon2id + AES-256-GCM
 * Based on proven KeyVault implementation
 */
class WalletCrypto {
public:
    // Generate cryptographically secure random bytes
    static bool generateRandomBytes(uint8_t* out, size_t len);
    
    // Derive encryption key from password using Argon2id
    // Parameters: password, salt (32 bytes), output key (32 bytes)
    static bool deriveKey(const std::string& password,
                         const std::vector<uint8_t>& salt,
                         uint8_t key_out[32]);
    
    // Encrypt data using AES-256-GCM with AAD for header authentication
    // AAD (Additional Authenticated Data) prevents tampering with metadata
    // Returns: salt(32) + nonce(12) + ciphertext + tag(16)
    static bool encrypt(const std::string& plaintext,
                       const std::string& password,
                       const std::string& aad_json,
                       std::vector<uint8_t>& output);
    
    // Decrypt data encrypted with encrypt()
    // Verifies AAD matches - fails if metadata was tampered
    static bool decrypt(const std::vector<uint8_t>& encrypted_data,
                       const std::string& password,
                       const std::string& aad_json,
                       std::string& plaintext_out);
    
    // Base64 encode/decode for JSON storage
    static std::string base64Encode(const std::vector<uint8_t>& data);
    static bool base64Decode(const std::string& encoded, std::vector<uint8_t>& output);
    
private:
    // Argon2id parameters (secure defaults)
    static constexpr uint32_t ARGON2_TIME_COST = 3;      // iterations
    static constexpr uint32_t ARGON2_MEMORY_COST = 65536; // 64 MB in KB
    static constexpr uint32_t ARGON2_PARALLELISM = 1;     // threads (1 to avoid thread pool issues)
    static constexpr size_t KEY_SIZE = 32;                // 256 bits
    static constexpr size_t SALT_SIZE = 32;               // 256 bits
    static constexpr size_t NONCE_SIZE = 12;              // 96 bits for GCM
    static constexpr size_t TAG_SIZE = 16;                // 128 bits authentication tag
};

} // namespace dinero

