#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <array>
#include "crypto/decrypt_encrypted_key.h"

namespace dinero {
    class WalletManager;
}

namespace dinero::rpc {

// Error taxonomy for encrypted key import
enum class EncryptedKeyError {
    SUCCESS = 0,
    INVALID_PARAMS,
    UNSUPPORTED_CIPHER_OR_KDF,
    WRONG_PASSPHRASE_OR_TAG,
    WALLET_LOCKED,
    RATE_LIMITED,
    DUPLICATE_KEY,
    NETWORK_MISMATCH,
    COMPRESSION_REQUIRED
};

// Rate limiter for decrypt attempts (prevents brute force)
class DecryptRateLimiter {
public:
    DecryptRateLimiter(int max_attempts = 5, std::chrono::seconds window = std::chrono::seconds(300));
    
    bool AllowAttempt(const std::string& identifier); // IP or PID
    void RecordFailure(const std::string& identifier);
    void RecordSuccess(const std::string& identifier);
    void Cleanup(); // Remove expired entries
    
private:
    struct AttemptRecord {
        int failed_attempts;
        std::chrono::steady_clock::time_point last_attempt;
        std::chrono::steady_clock::time_point window_start;
    };
    
    int max_attempts_;
    std::chrono::seconds window_;
    std::unordered_map<std::string, AttemptRecord> attempts_;
    std::mutex mutex_;
};

// Enhanced parameter validator
class EncryptedKeyValidator {
public:
    struct ValidationResult {
        EncryptedKeyError error;
        std::string message;
        bool valid;
    };
    
    static ValidationResult ValidateParams(const dinero::crypto::EncryptedKeyParams& params);
    static ValidationResult ValidateKDF(const std::string& kdf, int iterations);
    static ValidationResult ValidateCipher(const std::string& cipher);
    static ValidationResult ValidateIV(const std::vector<uint8_t>& iv, const std::string& cipher);
    static ValidationResult ValidateTag(const std::vector<uint8_t>& tag, const std::string& cipher);
    static ValidationResult ValidateSalt(const std::vector<uint8_t>& salt);
    
private:
    static constexpr int MIN_PBKDF2_ITERATIONS = 100000;
    static constexpr size_t MIN_SALT_SIZE = 16;
    static constexpr size_t GCM_IV_SIZE = 12;
    static constexpr size_t GCM_TAG_SIZE = 16;
};

// Key fingerprint generator (for duplicate detection)
class KeyFingerprint {
public:
    // Generate fingerprint from public key (hash160 of compressed pubkey)
    static std::string Generate(const std::array<uint8_t, 32>& private_key);
    
    // Check if fingerprint already exists in wallet
    static bool Exists(const std::string& fingerprint, dinero::WalletManager* wallet_manager);
    
private:
    static std::vector<uint8_t> CompressPublicKey(const std::vector<uint8_t>& pubkey);
    static std::string Hash160ToHex(const std::vector<uint8_t>& data);
};

} // namespace dinero::rpc
