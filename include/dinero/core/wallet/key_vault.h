#pragma once

#include <string>
#include <array>
#include <vector>
#include <memory>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <iomanip>

// Simple 160-bit hash type (20 bytes)
using uint160 = std::array<uint8_t, 20>;

// Helper function to convert uint160 to hex string
inline std::string uint160ToString(const uint160& hash) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        oss << std::setw(2) << static_cast<unsigned>(byte);
    }
    return oss.str();
}

namespace dinero {

// Secure buffer that zeroizes on destruction
template<size_t N>
class SecureBuffer {
public:
    SecureBuffer() { data_.fill(0); }
    ~SecureBuffer() { secure_zero(); }
    
    // No copy/move to prevent accidental duplication
    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;
    SecureBuffer(SecureBuffer&&) = delete;
    SecureBuffer& operator=(SecureBuffer&&) = delete;
    
    uint8_t* data() { return data_.data(); }
    const uint8_t* data() const { return data_.data(); }
    constexpr size_t size() const { return N; }
    
    void secure_zero();
    
private:
    std::array<uint8_t, N> data_;
};

// KDF parameters for master key derivation
struct KdfParams {
    std::string kdf_name;        // "argon2id" or "pbkdf2-hmac-sha256"
    std::vector<uint8_t> salt;   // 32 bytes recommended
    int iterations;              // PBKDF2: 600k+, Argon2id: 3+
    int memory_kb;               // Argon2id only: 64MB+ recommended
    int parallelism;             // Argon2id only: 4+ recommended
    
    std::string toJson() const;
    static KdfParams fromJson(const std::string& json);
    static KdfParams createArgon2id(const std::vector<uint8_t>& salt);
    static KdfParams createPbkdf2(const std::vector<uint8_t>& salt);
};

// Wallet metadata for KDF parameters
struct WalletMeta {
    std::string kdf_params_json;
    std::string created_at;
    std::string version;
};

// Secure key vault for encrypted key storage at rest
class KeyVault {
public:
    // Factory methods
    static std::unique_ptr<KeyVault> createNew(const std::string& passphrase, const std::string& db_path);
    static std::unique_ptr<KeyVault> open(const std::string& passphrase, const WalletMeta& meta, const std::string& db_path);
    
    // Destructor ensures secure cleanup
    ~KeyVault();
    
    // Lock/unlock operations
    void lock();
    bool unlock(const std::string& passphrase);
    bool isLocked() const { return locked_; }
    
    // Key storage operations (thread-safe)
    bool store(const uint160& fingerprint, const std::array<uint8_t, 32>& private_key, 
               bool compressed, const std::string& label, const std::string& origin = "import");
    
    bool load(const uint160& fingerprint, std::array<uint8_t, 32>& out_private_key, 
              bool& compressed, std::string* label = nullptr) const;
    
    bool has(const uint160& fingerprint) const;
    bool remove(const uint160& fingerprint);
    
    // List all stored keys (returns fingerprints and metadata)
    std::vector<std::tuple<uint160, std::string, bool, std::string>> listKeys() const;
    
    // Passphrase change (rewrap all keys with new master key)
    bool changePassphrase(const std::string& old_passphrase, const std::string& new_passphrase);
    
    // Get KDF parameters for storage
    const KdfParams& getKdfParams() const { return kdf_params_; }
    WalletMeta getWalletMeta() const;
    
private:
    KeyVault(const KdfParams& params, const std::string& db_path);
    
    // Master key derivation
    bool deriveMasterKey(const std::string& passphrase);
    void zeroizeMasterKey();
    
    // Database operations
    bool initializeDatabase();
    bool createTables();
    
    // Encryption/decryption helpers
    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext) const;
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext, const std::vector<uint8_t>& iv) const;
    
    // Generate secure random data
    static std::vector<uint8_t> generateSalt(size_t size = 32);
    static std::vector<uint8_t> generateIV(size_t size = 12);
    
private:
    KdfParams kdf_params_;
    SecureBuffer<32> master_key_;
    bool locked_;
    std::string db_path_;
    // Database connection will be added later
    // std::unique_ptr<SqliteConn> db_;
    mutable std::mutex mutex_;
};

// Argon2id and PBKDF2 implementations
namespace crypto {
    bool deriveKeyArgon2id(const std::string& passphrase, 
                          const std::vector<uint8_t>& salt,
                          int iterations, int memory_kb, int parallelism,
                          std::array<uint8_t, 32>& out_key);
                          
    bool deriveKeyPbkdf2(const std::string& passphrase,
                        const std::vector<uint8_t>& salt, 
                        int iterations,
                        std::array<uint8_t, 32>& out_key);
                        
    std::vector<uint8_t> encryptAesGcm(const std::vector<uint8_t>& plaintext,
                                      const std::array<uint8_t, 32>& key,
                                      const std::vector<uint8_t>& iv);
                                      
    std::vector<uint8_t> decryptAesGcm(const std::vector<uint8_t>& ciphertext_with_tag,
                                      const std::array<uint8_t, 32>& key,
                                      const std::vector<uint8_t>& iv);
}

} // namespace dinero
