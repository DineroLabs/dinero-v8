// Dinero Wallet Encryption Implementation
// Uses Argon2id (password hashing) + AES-256-GCM (encryption)
// November 2025

#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>

// Argon2id for password hashing (bundled)
#include <argon2.h>

// OpenSSL for AES-256-GCM encryption (already a dependency)
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

namespace dinero {
namespace crypto {

/**
 * Derive encryption key from password using Argon2id
 * 
 * Argon2id parameters (OWASP 2023 recommendations):
 *   - Memory: 64 MB (65536 KB)
 *   - Iterations: 3
 *   - Parallelism: 1 (portable)
 *   - Output: 32 bytes (256 bits)
 * 
 * @param password User's password
 * @param salt Random 16-byte salt (must be unique per wallet)
 * @param iterations Number of iterations (3+ recommended)
 * @param memory_kb Memory usage in KB (65536 = 64 MB)
 * @param parallelism Degree of parallelism (1-4)
 * @param output 32-byte derived key (output parameter)
 * @return true on success, false on error
 */
bool deriveKeyArgon2id(
    const std::string& password,
    const std::vector<uint8_t>& salt,
    int iterations,
    int memory_kb,
    int parallelism,
    std::array<uint8_t, 32>& output
) {
    if (password.empty()) {
        return false;
    }
    
    if (salt.size() < 16) {
        return false;  // Salt must be at least 16 bytes
    }
    
    // Call Argon2id from bundled library
    int result = argon2id_hash_raw(
        iterations,                    // t_cost (time cost)
        memory_kb,                     // m_cost (memory cost in KB)
        parallelism,                   // parallelism (threads)
        password.data(),               // pwd
        password.size(),               // pwdlen
        salt.data(),                   // salt
        salt.size(),                   // saltlen
        output.data(),                 // hash (output)
        output.size()                  // hashlen
    );
    
    return result == ARGON2_OK;
}

/**
 * Encrypt data using AES-256-GCM
 * 
 * AES-256-GCM provides:
 *   - Confidentiality (encryption)
 *   - Authenticity (MAC tag)
 *   - Associated data support (optional)
 * 
 * @param plaintext Data to encrypt
 * @param key 32-byte encryption key (from Argon2id)
 * @param nonce 12-byte nonce (must be unique per encryption)
 * @return Encrypted data (ciphertext + 16-byte GCM tag appended)
 */
std::vector<uint8_t> encryptAesGcm(
    const std::vector<uint8_t>& plaintext,
    const std::array<uint8_t, 32>& key,
    const std::vector<uint8_t>& nonce
) {
    if (plaintext.empty()) {
        throw std::runtime_error("Cannot encrypt empty plaintext");
    }
    
    if (nonce.size() != 12) {
        throw std::runtime_error("AES-GCM nonce must be exactly 12 bytes");
    }
    
    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }
    
    // Initialize encryption
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), nonce.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize AES-256-GCM encryption");
    }
    
    // Allocate output buffer (ciphertext + 16-byte tag)
    std::vector<uint8_t> output(plaintext.size() + 16);
    int len = 0;
    
    // Encrypt plaintext
    if (EVP_EncryptUpdate(ctx, output.data(), &len, plaintext.data(), plaintext.size()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to encrypt data");
    }
    int ciphertext_len = len;
    
    // Finalize encryption
    if (EVP_EncryptFinal_ex(ctx, output.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to finalize encryption");
    }
    ciphertext_len += len;
    
    // Get GCM authentication tag (16 bytes)
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, output.data() + ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to get GCM tag");
    }
    
    // Clean up
    EVP_CIPHER_CTX_free(ctx);
    
    // Resize to actual size (ciphertext + 16-byte tag)
    output.resize(ciphertext_len + 16);
    
    return output;
}

/**
 * Decrypt data using AES-256-GCM
 * 
 * @param ciphertext Encrypted data (with 16-byte GCM tag appended)
 * @param key 32-byte decryption key (same as encryption key)
 * @param nonce 12-byte nonce (same as encryption nonce)
 * @return Decrypted plaintext
 * @throws std::runtime_error if authentication fails (tampered data)
 */
std::vector<uint8_t> decryptAesGcm(
    const std::vector<uint8_t>& ciphertext,
    const std::array<uint8_t, 32>& key,
    const std::vector<uint8_t>& nonce
) {
    if (ciphertext.size() < 16) {
        throw std::runtime_error("Ciphertext too short (must include 16-byte GCM tag)");
    }
    
    if (nonce.size() != 12) {
        throw std::runtime_error("AES-GCM nonce must be exactly 12 bytes");
    }
    
    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }
    
    // Initialize decryption
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), nonce.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize AES-256-GCM decryption");
    }
    
    // Separate ciphertext and tag
    size_t ciphertext_len = ciphertext.size() - 16;
    const uint8_t* tag = ciphertext.data() + ciphertext_len;
    
    // Allocate output buffer
    std::vector<uint8_t> output(ciphertext_len);
    int len = 0;
    
    // Decrypt ciphertext
    if (EVP_DecryptUpdate(ctx, output.data(), &len, ciphertext.data(), ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to decrypt data");
    }
    int plaintext_len = len;
    
    // Set expected GCM tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set GCM tag");
    }
    
    // Finalize decryption (verifies tag)
    int final_len = 0;
    if (EVP_DecryptFinal_ex(ctx, output.data() + len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Authentication failed - data may be corrupted or tampered");
    }
    plaintext_len += final_len;
    
    // Clean up
    EVP_CIPHER_CTX_free(ctx);
    
    // Resize to actual plaintext size
    output.resize(plaintext_len);
    
    return output;
}

} // namespace crypto
} // namespace dinero

