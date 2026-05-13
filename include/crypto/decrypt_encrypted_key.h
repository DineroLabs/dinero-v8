#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero::crypto {

struct EncryptedKeyParams {
    std::string enc;                    // "pbkdf2-hmac-sha256"
    int iter;                          // e.g. 100000
    std::vector<uint8_t> salt;         // bytes
    std::string cipher;                // "aes-256-gcm"
    std::vector<uint8_t> iv;           // 12 bytes recommended
    std::vector<uint8_t> ct;           // ciphertext
    std::vector<uint8_t> tag;          // 16 bytes (GCM)
    std::string passphrase;            // utf-8
};

struct DecryptResult {
    bool ok;
    std::string err;
    std::vector<uint8_t> key32;        // 32-byte private key on success
};

/**
 * Production-safe PBKDF2-HMAC-SHA256 + AES-256-GCM decryptor
 * 
 * @param p Encrypted key parameters
 * @return DecryptResult with success status and decrypted key or error
 */
DecryptResult decrypt_pbkdf2_aes256_gcm(const EncryptedKeyParams& p);

} // namespace dinero::crypto
