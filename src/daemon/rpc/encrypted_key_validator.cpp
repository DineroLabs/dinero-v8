#include "daemon/rpc/encrypted_key_validator.h"
#include "wallet/wallet_manager.h"
#include "common/logger.h"
#include "crypto/hash.h"
#include <secp256k1.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace dinero::rpc {

// DecryptRateLimiter implementation
DecryptRateLimiter::DecryptRateLimiter(int max_attempts, std::chrono::seconds window)
    : max_attempts_(max_attempts), window_(window) {}

bool DecryptRateLimiter::AllowAttempt(const std::string& identifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto& record = attempts_[identifier];
    
    // Initialize window_start for new records
    if (record.window_start == std::chrono::steady_clock::time_point{}) {
        record.window_start = now;
        record.failed_attempts = 0;
    }
    
    // Reset if window has passed
    if (now - record.window_start > window_) {
        record.failed_attempts = 0;
        record.window_start = now;
    }
    
    // Check if rate limited (allow up to max_attempts, block after that)
    if (record.failed_attempts >= max_attempts_) {
        auto remaining = window_ - (now - record.window_start);
        if (remaining > std::chrono::seconds(0)) {
            g_logger.info("Rate limit exceeded for decrypt attempts from " + identifier + 
                         ". Try again in " + std::to_string(remaining.count()) + " seconds.");
            return false;
        } else {
            // Window expired, reset
            record.failed_attempts = 0;
            record.window_start = now;
        }
    }
    
    record.last_attempt = now;
    return true;
}

void DecryptRateLimiter::RecordFailure(const std::string& identifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    attempts_[identifier].failed_attempts++;
}

void DecryptRateLimiter::RecordSuccess(const std::string& identifier) {
    std::lock_guard<std::mutex> lock(mutex_);
    attempts_.erase(identifier); // Clear on success
}

void DecryptRateLimiter::Cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = attempts_.begin(); it != attempts_.end();) {
        if (now - it->second.last_attempt > window_ * 2) {
            it = attempts_.erase(it);
        } else {
            ++it;
        }
    }
}

// EncryptedKeyValidator implementation
EncryptedKeyValidator::ValidationResult EncryptedKeyValidator::ValidateParams(
    const dinero::crypto::EncryptedKeyParams& params) {
    
    // Validate KDF
    auto kdf_result = ValidateKDF(params.enc, params.iter);
    if (!kdf_result.valid) return kdf_result;
    
    // Validate cipher
    auto cipher_result = ValidateCipher(params.cipher);
    if (!cipher_result.valid) return cipher_result;
    
    // Validate salt
    auto salt_result = ValidateSalt(params.salt);
    if (!salt_result.valid) return salt_result;
    
    // Validate IV
    auto iv_result = ValidateIV(params.iv, params.cipher);
    if (!iv_result.valid) return iv_result;
    
    // Validate tag (for AEAD ciphers)
    auto tag_result = ValidateTag(params.tag, params.cipher);
    if (!tag_result.valid) return tag_result;
    
    // Validate ciphertext
    if (params.ct.empty()) {
        return {EncryptedKeyError::INVALID_PARAMS, "Ciphertext cannot be empty", false};
    }
    
    // Validate passphrase
    if (params.passphrase.empty()) {
        return {EncryptedKeyError::INVALID_PARAMS, "Passphrase cannot be empty", false};
    }
    
    return {EncryptedKeyError::SUCCESS, "Parameters valid", true};
}

EncryptedKeyValidator::ValidationResult EncryptedKeyValidator::ValidateKDF(
    const std::string& kdf, int iterations) {
    
    if (kdf != "pbkdf2-hmac-sha256") {
        return {EncryptedKeyError::UNSUPPORTED_CIPHER_OR_KDF, 
                "Unsupported KDF: " + kdf + ". Only pbkdf2-hmac-sha256 is supported.", false};
    }
    
    if (iterations < MIN_PBKDF2_ITERATIONS) {
        return {EncryptedKeyError::INVALID_PARAMS,
                "PBKDF2 iterations too low. Minimum: " + std::to_string(MIN_PBKDF2_ITERATIONS), false};
    }
    
    return {EncryptedKeyError::SUCCESS, "KDF valid", true};
}

EncryptedKeyValidator::ValidationResult EncryptedKeyValidator::ValidateCipher(
    const std::string& cipher) {
    
    if (cipher != "aes-256-gcm") {
        return {EncryptedKeyError::UNSUPPORTED_CIPHER_OR_KDF,
                "Unsupported cipher: " + cipher + ". Only aes-256-gcm is supported.", false};
    }
    
    return {EncryptedKeyError::SUCCESS, "Cipher valid", true};
}

EncryptedKeyValidator::ValidationResult EncryptedKeyValidator::ValidateIV(
    const std::vector<uint8_t>& iv, const std::string& cipher) {
    
    if (cipher == "aes-256-gcm") {
        if (iv.size() != GCM_IV_SIZE) {
            return {EncryptedKeyError::INVALID_PARAMS,
                    "Invalid IV size for AES-GCM. Expected: " + std::to_string(GCM_IV_SIZE) + 
                    " bytes, got: " + std::to_string(iv.size()), false};
        }
    }
    
    return {EncryptedKeyError::SUCCESS, "IV valid", true};
}

EncryptedKeyValidator::ValidationResult EncryptedKeyValidator::ValidateTag(
    const std::vector<uint8_t>& tag, const std::string& cipher) {
    
    if (cipher == "aes-256-gcm") {
        if (tag.size() != GCM_TAG_SIZE) {
            return {EncryptedKeyError::INVALID_PARAMS,
                    "Invalid authentication tag size for AES-GCM. Expected: " + std::to_string(GCM_TAG_SIZE) + 
                    " bytes, got: " + std::to_string(tag.size()), false};
        }
    }
    
    return {EncryptedKeyError::SUCCESS, "Authentication tag valid", true};
}

EncryptedKeyValidator::ValidationResult EncryptedKeyValidator::ValidateSalt(
    const std::vector<uint8_t>& salt) {
    
    if (salt.size() < MIN_SALT_SIZE) {
        return {EncryptedKeyError::INVALID_PARAMS,
                "Salt too short. Minimum: " + std::to_string(MIN_SALT_SIZE) + 
                " bytes, got: " + std::to_string(salt.size()), false};
    }
    
    return {EncryptedKeyError::SUCCESS, "Salt valid", true};
}

// KeyFingerprint implementation
std::string KeyFingerprint::Generate(const std::array<uint8_t, 32>& private_key) {
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        throw std::runtime_error("Failed to create secp256k1 context");
    }
    
    try {
        // Generate public key
        secp256k1_pubkey pubkey;
        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, private_key.data())) {
            secp256k1_context_destroy(ctx);
            throw std::runtime_error("Failed to generate public key");
        }
        
        // Serialize compressed public key
        unsigned char pubkey_bytes[33];
        size_t pubkey_len = 33;
        if (!secp256k1_ec_pubkey_serialize(ctx, pubkey_bytes, &pubkey_len, &pubkey, SECP256K1_EC_COMPRESSED)) {
            secp256k1_context_destroy(ctx);
            throw std::runtime_error("Failed to serialize public key");
        }
        
        secp256k1_context_destroy(ctx);
        
        // Calculate HASH160 (SHA256 + RIPEMD160)
        std::vector<uint8_t> pubkey_vec(pubkey_bytes, pubkey_bytes + pubkey_len);
        return Hash160ToHex(pubkey_vec);
        
    } catch (...) {
        secp256k1_context_destroy(ctx);
        throw;
    }
}

bool KeyFingerprint::Exists(const std::string& fingerprint, dinero::WalletManager* wallet_manager) {
    // TODO: Implement fingerprint lookup in wallet database
    // For now, return false (no duplicates detected)
    return false;
}

std::string KeyFingerprint::Hash160ToHex(const std::vector<uint8_t>& data) {
    // Use centralized EVP-based HASH160
    auto hash160 = din::crypto::HASH160(data);
    
    // Convert to hex string
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 20; ++i) {
        oss << std::setw(2) << static_cast<unsigned>(hash160[i]);
    }
    
    return oss.str();
}

} // namespace dinero::rpc
