#include "crypto/decrypt_encrypted_key.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>

namespace dinero::crypto {

static void cleanse(void* p, size_t n) { 
    OPENSSL_cleanse(p, n); 
}

DecryptResult decrypt_pbkdf2_aes256_gcm(const EncryptedKeyParams& p) {
    // Validate encryption method
    if (p.enc != "pbkdf2-hmac-sha256") {
        return {false, "Unsupported KDF", {}};
    }
    
    if (p.cipher != "aes-256-gcm") {
        return {false, "Unsupported cipher", {}};
    }
    
    // Validate parameters
    if (p.iter < 1) {
        return {false, "Invalid iterations", {}};
    }
    
    if (p.salt.size() < 8) {
        return {false, "Salt too short (minimum 8 bytes)", {}};
    }
    
    if (p.iv.size() < 12) {
        return {false, "IV must be 12+ bytes for GCM", {}};
    }
    
    if (p.tag.size() != 16) {
        return {false, "GCM tag must be 16 bytes", {}};
    }
    
    if (p.ct.empty()) {
        return {false, "Ciphertext empty", {}};
    }
    
    // Derive key using PBKDF2-HMAC-SHA256
    std::vector<uint8_t> key(32);
    if (!PKCS5_PBKDF2_HMAC(
            p.passphrase.data(), (int)p.passphrase.size(),
            p.salt.data(), (int)p.salt.size(),
            p.iter, EVP_sha256(), (int)key.size(), key.data())) {
        return {false, "PBKDF2 failed", {}};
    }
    
    // Prepare for decryption
    std::vector<uint8_t> pt(p.ct.size());
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        cleanse(key.data(), key.size());
        return {false, "EVP_CIPHER_CTX_new failed", {}};
    }
    
    // Initialize decryption
    int ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)p.iv.size(), nullptr);
    ok &= EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), p.iv.data());
    
    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        cleanse(key.data(), key.size());
        return {false, "EVP_DecryptInit_ex failed", {}};
    }
    
    // Decrypt the ciphertext
    int len = 0, total = 0;
    if (!EVP_DecryptUpdate(ctx, pt.data(), &len, p.ct.data(), (int)p.ct.size())) {
        EVP_CIPHER_CTX_free(ctx);
        cleanse(key.data(), key.size());
        cleanse(pt.data(), pt.size());
        return {false, "EVP_DecryptUpdate failed", {}};
    }
    total = len;
    
    // Set authentication tag before finalization
    if (!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)p.tag.size(), (void*)p.tag.data())) {
        EVP_CIPHER_CTX_free(ctx);
        cleanse(key.data(), key.size());
        cleanse(pt.data(), pt.size());
        return {false, "Set GCM tag failed", {}};
    }
    
    // Finalize decryption (this verifies the authentication tag)
    int len2 = 0;
    if (EVP_DecryptFinal_ex(ctx, pt.data() + total, &len2) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        cleanse(key.data(), key.size());
        cleanse(pt.data(), pt.size());
        return {false, "Authentication failed (bad tag/passphrase/params)", {}};
    }
    total += len2;
    
    // Cleanup
    EVP_CIPHER_CTX_free(ctx);
    cleanse(key.data(), key.size());
    
    // Validate plaintext length
    pt.resize((size_t)total);
    if (pt.size() != 32) {
        cleanse(pt.data(), pt.size());
        return {false, "Plaintext is not a 32-byte private key", {}};
    }
    
    return {true, "", std::move(pt)};
}

} // namespace dinero::crypto
