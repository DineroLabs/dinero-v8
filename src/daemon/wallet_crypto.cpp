#include "wallet_crypto.h"
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/core_names.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <cstring>
#include <stdexcept>
#include <array>

// Compatibility defines for older OpenSSL versions
#ifndef OSSL_KDF_PARAM_ARGON2_MEMCOST
#define OSSL_KDF_PARAM_ARGON2_MEMCOST    "memcost"
#endif
#ifndef OSSL_KDF_PARAM_THREADS
#define OSSL_KDF_PARAM_THREADS            "threads"
#endif

namespace dinero {

bool WalletCrypto::generateRandomBytes(uint8_t* out, size_t len) {
    return RAND_bytes(out, static_cast<int>(len)) == 1;
}

bool WalletCrypto::deriveKey(const std::string& password,
                             const std::vector<uint8_t>& salt,
                             uint8_t key_out[32]) {
    if (salt.size() != SALT_SIZE) {
        return false;
    }
    
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (!kdf) {
        fprintf(stderr, "Failed to fetch Argon2ID KDF\n");
        ERR_print_errors_fp(stderr);
        return false;
    }
    
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    
    if (!ctx) {
        fprintf(stderr, "Failed to create KDF context\n");
        return false;
    }
    
    // Set Argon2id parameters
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                          const_cast<char*>(password.c_str()),
                                          password.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                          const_cast<uint8_t*>(salt.data()),
                                          salt.size()),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, 
                                    const_cast<uint32_t*>(&ARGON2_TIME_COST)),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST,
                                    const_cast<uint32_t*>(&ARGON2_MEMORY_COST)),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS,
                                    const_cast<uint32_t*>(&ARGON2_PARALLELISM)),
        OSSL_PARAM_construct_end()
    };
    
    int ret = EVP_KDF_derive(ctx, key_out, KEY_SIZE, params);
    
    if (ret != 1) {
        fprintf(stderr, "Failed to derive key with Argon2ID\n");
        ERR_print_errors_fp(stderr);
    }
    
    EVP_KDF_CTX_free(ctx);
    
    return ret == 1;
}

bool WalletCrypto::encrypt(const std::string& plaintext,
                           const std::string& password,
                           const std::string& aad_json,
                           std::vector<uint8_t>& output) {
    // Generate random salt and nonce
    std::vector<uint8_t> salt(SALT_SIZE);
    std::vector<uint8_t> nonce(NONCE_SIZE);
    
    if (!generateRandomBytes(salt.data(), SALT_SIZE)) {
        return false;
    }
    
    if (!generateRandomBytes(nonce.data(), NONCE_SIZE)) {
        return false;
    }
    
    // Derive encryption key
    uint8_t key[KEY_SIZE];
    if (!deriveKey(password, salt, key)) {
        OPENSSL_cleanse(key, KEY_SIZE);
        return false;
    }
    
    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        OPENSSL_cleanse(key, KEY_SIZE);
        return false;
    }
    
    bool success = false;
    
    do {
        // Initialize encryption
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, nonce.data()) != 1) {
            break;
        }
        
        // Set AAD (Additional Authenticated Data) - authenticated but not encrypted
        if (!aad_json.empty()) {
            int len = 0;
            if (EVP_EncryptUpdate(ctx, nullptr, &len,
                                 reinterpret_cast<const uint8_t*>(aad_json.data()),
                                 aad_json.size()) != 1) {
                break;
            }
        }
        
        // Allocate output buffer: salt + nonce + ciphertext + tag
        output.resize(SALT_SIZE + NONCE_SIZE + plaintext.size() + TAG_SIZE);
        
        // Copy salt and nonce
        std::memcpy(output.data(), salt.data(), SALT_SIZE);
        std::memcpy(output.data() + SALT_SIZE, nonce.data(), NONCE_SIZE);
        
        // Encrypt
        int len = 0;
        uint8_t* ciphertext_ptr = output.data() + SALT_SIZE + NONCE_SIZE;
        
        if (EVP_EncryptUpdate(ctx, ciphertext_ptr, &len,
                             reinterpret_cast<const uint8_t*>(plaintext.data()),
                             plaintext.size()) != 1) {
            break;
        }
        
        int ciphertext_len = len;
        
        // Finalize encryption
        if (EVP_EncryptFinal_ex(ctx, ciphertext_ptr + len, &len) != 1) {
            break;
        }
        
        ciphertext_len += len;
        
        // Get authentication tag
        uint8_t* tag_ptr = output.data() + SALT_SIZE + NONCE_SIZE + ciphertext_len;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_SIZE, tag_ptr) != 1) {
            break;
        }
        
        // Resize to actual size
        output.resize(SALT_SIZE + NONCE_SIZE + ciphertext_len + TAG_SIZE);
        success = true;
        
    } while (false);
    
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, KEY_SIZE);
    
    return success;
}

bool WalletCrypto::decrypt(const std::vector<uint8_t>& encrypted_data,
                           const std::string& password,
                           const std::string& aad_json,
                           std::string& plaintext_out) {
    // Validate minimum size: salt + nonce + tag
    if (encrypted_data.size() < SALT_SIZE + NONCE_SIZE + TAG_SIZE) {
        return false;
    }
    
    // Extract salt, nonce, ciphertext, and tag
    std::vector<uint8_t> salt(encrypted_data.begin(), 
                              encrypted_data.begin() + SALT_SIZE);
    std::vector<uint8_t> nonce(encrypted_data.begin() + SALT_SIZE,
                               encrypted_data.begin() + SALT_SIZE + NONCE_SIZE);
    
    size_t ciphertext_len = encrypted_data.size() - SALT_SIZE - NONCE_SIZE - TAG_SIZE;
    const uint8_t* ciphertext = encrypted_data.data() + SALT_SIZE + NONCE_SIZE;
    const uint8_t* tag = ciphertext + ciphertext_len;
    
    // Derive decryption key
    uint8_t key[KEY_SIZE];
    if (!deriveKey(password, salt, key)) {
        OPENSSL_cleanse(key, KEY_SIZE);
        return false;
    }
    
    // Create cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        OPENSSL_cleanse(key, KEY_SIZE);
        return false;
    }
    
    bool success = false;
    
    do {
        // Initialize decryption
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key, nonce.data()) != 1) {
            break;
        }
        
        // Set AAD (must match encryption AAD or tag verification will fail)
        if (!aad_json.empty()) {
            int len_aad = 0;
            if (EVP_DecryptUpdate(ctx, nullptr, &len_aad,
                                 reinterpret_cast<const uint8_t*>(aad_json.data()),
                                 aad_json.size()) != 1) {
                break;
            }
        }
        
        // Allocate plaintext buffer
        std::vector<uint8_t> plaintext(ciphertext_len);
        int len = 0;
        
        // Decrypt
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext, ciphertext_len) != 1) {
            break;
        }
        
        int plaintext_len = len;
        
        // Set expected tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, TAG_SIZE,
                               const_cast<uint8_t*>(tag)) != 1) {
            break;
        }
        
        // Finalize decryption (verifies authentication tag)
        if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
            // Authentication failed - wrong password or corrupted data
            break;
        }
        
        plaintext_len += len;
        plaintext_out.assign(reinterpret_cast<char*>(plaintext.data()), plaintext_len);
        success = true;
        
    } while (false);
    
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, KEY_SIZE);
    
    return success;
}

std::string WalletCrypto::base64Encode(const std::vector<uint8_t>& data) {
    if (data.empty()) return "";
    
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    
    BIO_write(b64, data.data(), data.size());
    BIO_flush(b64);
    
    BUF_MEM* buf;
    BIO_get_mem_ptr(b64, &buf);
    
    std::string result(buf->data, buf->length);
    BIO_free_all(b64);
    
    return result;
}

bool WalletCrypto::base64Decode(const std::string& encoded, std::vector<uint8_t>& output) {
    if (encoded.empty()) return false;
    
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new_mem_buf(encoded.data(), encoded.size());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    
    output.resize(encoded.size()); // Over-allocate
    int decoded_len = BIO_read(b64, output.data(), encoded.size());
    BIO_free_all(b64);
    
    if (decoded_len < 0) {
        return false;
    }
    
    output.resize(decoded_len);
    return true;
}

} // namespace dinero

// NOTE: dinero::crypto::deriveKeyArgon2id, encryptAesGcm, decryptAesGcm
// are provided by src/crypto/wallet_crypto.cpp (dinero_crypto library).
// Do NOT re-define them here — previous compat shims used "dummy" password
// and were functionally broken.

