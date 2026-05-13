#ifndef DINERO_WALLET_CRYPTO_H
#define DINERO_WALLET_CRYPTO_H

#include <array>
#include <vector>
#include <string>

namespace dinero {
namespace crypto {

/**
 * Derive encryption key from password using Argon2id
 *
 * @param password User's password
 * @param salt Random 16-byte salt (must be unique per wallet)
 * @param iterations Number of iterations (3+ recommended)
 * @param memory_kb Memory usage in KB (65536 = 64 MB recommended)
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
);

/**
 * Encrypt data using AES-256-GCM
 *
 * @param plaintext Data to encrypt
 * @param key 32-byte encryption key
 * @param nonce 12-byte nonce (must be unique per encryption)
 * @return Encrypted data (ciphertext + 16-byte GCM tag appended)
 */
std::vector<uint8_t> encryptAesGcm(
    const std::vector<uint8_t>& plaintext,
    const std::array<uint8_t, 32>& key,
    const std::vector<uint8_t>& nonce
);

/**
 * Decrypt data using AES-256-GCM
 *
 * @param ciphertext Encrypted data (with 16-byte GCM tag appended)
 * @param key 32-byte decryption key
 * @param nonce 12-byte nonce (same as used for encryption)
 * @return Decrypted plaintext
 * @throws std::runtime_error if authentication fails
 */
std::vector<uint8_t> decryptAesGcm(
    const std::vector<uint8_t>& ciphertext,
    const std::array<uint8_t, 32>& key,
    const std::vector<uint8_t>& nonce
);

} // namespace crypto
} // namespace dinero

#endif // DINERO_WALLET_CRYPTO_H
