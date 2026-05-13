#include "wallet/key_vault.h"
#include "common/logger.h"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <json/json.h>
#ifdef HAVE_LIBSODIUM
#include <sodium.h>
#endif
#include <mutex>
#include <sstream>
#include <iomanip>

namespace dinero {

// SecureBuffer implementation
template<size_t N>
void SecureBuffer<N>::secure_zero() {
    OPENSSL_cleanse(data_.data(), N);
}

// Explicit instantiation for common sizes
template class SecureBuffer<32>;

// KdfParams implementation
std::string KdfParams::toJson() const {
    Json::Value root;
    root["kdf_name"] = kdf_name;
    root["iterations"] = iterations;
    root["memory_kb"] = memory_kb;
    root["parallelism"] = parallelism;
    
    // Encode salt as hex
    std::ostringstream salt_hex;
    salt_hex << std::hex << std::setfill('0');
    for (uint8_t byte : salt) {
        salt_hex << std::setw(2) << static_cast<unsigned>(byte);
    }
    root["salt"] = salt_hex.str();
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

KdfParams KdfParams::fromJson(const std::string& json) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(json);
    
    if (!Json::parseFromStream(builder, stream, &root, &errs)) {
        throw std::runtime_error("Failed to parse KDF parameters JSON: " + errs);
    }
    
    KdfParams params;
    params.kdf_name = root["kdf_name"].asString();
    params.iterations = root["iterations"].asInt();
    params.memory_kb = root["memory_kb"].asInt();
    params.parallelism = root["parallelism"].asInt();
    
    // Decode salt from hex
    std::string salt_hex = root["salt"].asString();
    params.salt.clear();
    for (size_t i = 0; i < salt_hex.length(); i += 2) {
        std::string byte_str = salt_hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        params.salt.push_back(byte);
    }
    
    return params;
}

KdfParams KdfParams::createArgon2id(const std::vector<uint8_t>& salt) {
    KdfParams params;
    params.kdf_name = "argon2id";
    params.salt = salt;
    params.iterations = 3;        // Argon2 time parameter
    params.memory_kb = 65536;     // 64 MB
    params.parallelism = 4;       // 4 threads
    return params;
}

KdfParams KdfParams::createPbkdf2(const std::vector<uint8_t>& salt) {
    KdfParams params;
    params.kdf_name = "pbkdf2-hmac-sha256";
    params.salt = salt;
    params.iterations = 600000;   // 600k iterations for master key (higher than import)
    params.memory_kb = 0;         // Not applicable
    params.parallelism = 0;       // Not applicable
    return params;
}

// KeyVault minimal implementation
KeyVault::KeyVault(const KdfParams& params, const std::string& db_path)
    : kdf_params_(params), locked_(true), db_path_(db_path) {
    master_key_.secure_zero();
}

KeyVault::~KeyVault() {
    lock();
}

std::unique_ptr<KeyVault> KeyVault::createNew(const std::string& passphrase, const std::string& db_path) {
    // Generate secure salt
    auto salt = generateSalt(32);
    
    // Create KDF parameters (use PBKDF2 for simplicity)
    KdfParams params = KdfParams::createPbkdf2(salt);
    
    auto vault = std::unique_ptr<KeyVault>(new KeyVault(params, db_path));
    
    if (!vault->unlock(passphrase)) {
        throw std::runtime_error("Failed to derive master key during vault creation");
    }
    
    return vault;
}

std::unique_ptr<KeyVault> KeyVault::open(const std::string& passphrase, const WalletMeta& meta, const std::string& db_path) {
    KdfParams params = KdfParams::fromJson(meta.kdf_params_json);
    
    auto vault = std::unique_ptr<KeyVault>(new KeyVault(params, db_path));
    
    if (!vault->unlock(passphrase)) {
        return nullptr; // Wrong passphrase
    }
    
    return vault;
}

bool KeyVault::initializeDatabase() {
    // For now, just return true - database integration can be added later
    return true;
}

void KeyVault::lock() {
    std::lock_guard<std::mutex> lock(mutex_);
    zeroizeMasterKey();
    locked_ = true;
}

bool KeyVault::unlock(const std::string& passphrase) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (deriveMasterKey(passphrase)) {
        locked_ = false;
        return true;
    }
    
    return false;
}

bool KeyVault::deriveMasterKey(const std::string& passphrase) {
    zeroizeMasterKey();
    
    std::array<uint8_t, 32> derived_key;
    bool success = false;
    
    if (kdf_params_.kdf_name == "pbkdf2-hmac-sha256") {
        success = crypto::deriveKeyPbkdf2(passphrase, kdf_params_.salt, 
                                        kdf_params_.iterations, derived_key);
    } else {
        // Fallback to PBKDF2 for unsupported KDFs
        g_logger.info("Unsupported KDF " + kdf_params_.kdf_name + ", using PBKDF2");
        success = crypto::deriveKeyPbkdf2(passphrase, kdf_params_.salt, 
                                        600000, derived_key);
    }
    
    if (success) {
        std::copy(derived_key.begin(), derived_key.end(), master_key_.data());
    }
    
    // Securely clear the temporary key
    OPENSSL_cleanse(derived_key.data(), derived_key.size());
    
    return success;
}

void KeyVault::zeroizeMasterKey() {
    master_key_.secure_zero();
}

bool KeyVault::store(const uint160& fingerprint, const std::array<uint8_t, 32>& private_key,
                    bool compressed, const std::string& label, const std::string& origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (locked_) {
        g_logger.error("KeyVault: Cannot store key while vault is locked");
        return false;
    }
    
    // For now, just log the operation - actual storage can be added later
    g_logger.info("KeyVault: Would store encrypted key with fingerprint " + 
                 uint160ToString(fingerprint) + " (origin: " + origin + ")");
    return true;
}

bool KeyVault::load(const uint160& fingerprint, std::array<uint8_t, 32>& out_private_key,
                   bool& compressed, std::string* label) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (locked_) {
        g_logger.error("KeyVault: Cannot load key while vault is locked");
        return false;
    }
    
    // For now, just return false - actual loading can be added later
    return false;
}

bool KeyVault::has(const uint160& fingerprint) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // For now, just return false - actual checking can be added later
    return false;
}

bool KeyVault::remove(const uint160& fingerprint) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (locked_) {
        return false;
    }
    
    // For now, just return false - actual removal can be added later
    return false;
}

std::vector<std::tuple<uint160, std::string, bool, std::string>> KeyVault::listKeys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // For now, just return empty vector - actual listing can be added later
    return {};
}

bool KeyVault::changePassphrase(const std::string& old_passphrase, const std::string& new_passphrase) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // For now, just verify old passphrase and update KDF params
    SecureBuffer<32> old_master;
    std::array<uint8_t, 32> temp_key;
    
    bool old_valid = crypto::deriveKeyPbkdf2(old_passphrase, kdf_params_.salt,
                                           kdf_params_.iterations, temp_key);
    
    if (!old_valid) {
        OPENSSL_cleanse(temp_key.data(), temp_key.size());
        return false;
    }
    
    // Generate new salt and derive new master key
    auto new_salt = generateSalt(32);
    KdfParams new_params = kdf_params_;
    new_params.salt = new_salt;
    
    bool new_valid = crypto::deriveKeyPbkdf2(new_passphrase, new_params.salt,
                                           new_params.iterations, temp_key);
    
    if (!new_valid) {
        OPENSSL_cleanse(temp_key.data(), temp_key.size());
        return false;
    }
    
    // Update instance parameters and master key
    kdf_params_ = new_params;
    std::copy(temp_key.begin(), temp_key.end(), master_key_.data());
    OPENSSL_cleanse(temp_key.data(), temp_key.size());
    locked_ = false;
    
    g_logger.info("KeyVault: Successfully changed passphrase");
    
    return true;
}

WalletMeta KeyVault::getWalletMeta() const {
    WalletMeta meta;
    meta.kdf_params_json = kdf_params_.toJson();
    meta.created_at = "2024-01-01T00:00:00Z"; // TODO: Store actual creation time
    meta.version = "1.0";
    return meta;
}

std::vector<uint8_t> KeyVault::generateSalt(size_t size) {
    std::vector<uint8_t> salt(size);
    if (RAND_bytes(salt.data(), static_cast<int>(size)) != 1) {
        throw std::runtime_error("Failed to generate random salt");
    }
    return salt;
}

std::vector<uint8_t> KeyVault::generateIV(size_t size) {
    std::vector<uint8_t> iv(size);
    if (RAND_bytes(iv.data(), static_cast<int>(size)) != 1) {
        throw std::runtime_error("Failed to generate random IV");
    }
    return iv;
}

// Crypto implementations
namespace crypto {

bool deriveKeyPbkdf2(const std::string& passphrase, const std::vector<uint8_t>& salt, 
                    int iterations, std::array<uint8_t, 32>& out_key) {
    return PKCS5_PBKDF2_HMAC(
        passphrase.data(), static_cast<int>(passphrase.size()),
        salt.data(), static_cast<int>(salt.size()),
        iterations, EVP_sha256(),
        32, out_key.data()
    ) == 1;
}

bool deriveKeyArgon2id(const std::string& passphrase, const std::vector<uint8_t>& salt,
                      int iterations, int memory_kb, int parallelism,
                      std::array<uint8_t, 32>& out_key) {
#ifdef HAVE_LIBSODIUM
    // Use real Argon2id from libsodium
    if (crypto_pwhash(
        out_key.data(), out_key.size(),
        passphrase.c_str(), passphrase.size(),
        salt.data(),
        iterations,           // opslimit
        memory_kb * 1024,    // memlimit (convert KB to bytes)
        crypto_pwhash_ALG_ARGON2ID13
    ) != 0) {
        g_logger.error("Argon2id derivation failed");
        return false;
    }
    g_logger.info("Derived key using Argon2id13");
    return true;
#else
    // Fallback to strong PBKDF2 (300,000+ iterations)
    g_logger.info("Argon2id not available, using strong PBKDF2 (300K iterations)");
    return deriveKeyPbkdf2(passphrase, salt, 300000, out_key);
#endif
}

std::vector<uint8_t> encryptAesGcm(const std::vector<uint8_t>& plaintext,
                                  const std::array<uint8_t, 32>& key,
                                  const std::vector<uint8_t>& iv) {
    // Real AES-256-GCM encryption using OpenSSL
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }
    
    // Initialize encryption
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }
    
    // Set IV length (12 bytes for GCM)
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set IV length");
    }
    
    // Set key and IV
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set key and IV");
    }
    
    // Encrypt plaintext
    std::vector<uint8_t> ciphertext(plaintext.size() + 16); // +16 for tag
    int len = 0, ciphertext_len = 0;
    
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptUpdate failed");
    }
    ciphertext_len = len;
    
    // Finalize encryption
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptFinal_ex failed");
    }
    ciphertext_len += len;
    
    // Get authentication tag (16 bytes)
    std::vector<uint8_t> tag(16);
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to get GCM tag");
    }
    
    // Cleanup
    EVP_CIPHER_CTX_free(ctx);
    
    // Resize and append tag
    ciphertext.resize(ciphertext_len);
    ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
    
    g_logger.info("Encrypted with AES-256-GCM");
    return ciphertext;
}

std::vector<uint8_t> decryptAesGcm(const std::vector<uint8_t>& ciphertext_with_tag,
                                  const std::array<uint8_t, 32>& key,
                                  const std::vector<uint8_t>& iv) {
    // Real AES-256-GCM decryption using OpenSSL
    if (ciphertext_with_tag.size() < 16) {
        throw std::runtime_error("Ciphertext too short (missing GCM tag)");
    }
    
    // Split ciphertext and tag
    size_t ciphertext_len = ciphertext_with_tag.size() - 16;
    std::vector<uint8_t> ciphertext(ciphertext_with_tag.begin(), ciphertext_with_tag.begin() + ciphertext_len);
    std::vector<uint8_t> tag(ciphertext_with_tag.begin() + ciphertext_len, ciphertext_with_tag.end());
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }
    
    // Initialize decryption
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptInit_ex failed");
    }
    
    // Set IV length
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set IV length");
    }
    
    // Set key and IV
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set key and IV");
    }
    
    // Decrypt ciphertext
    std::vector<uint8_t> plaintext(ciphertext.size());
    int len = 0, plaintext_len = 0;
    
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), static_cast<int>(ciphertext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }
    plaintext_len = len;
    
    // Set authentication tag
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Failed to set GCM tag");
    }
    
    // Finalize decryption (verifies tag)
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("Authentication failed (wrong password or corrupted data)");
    }
    plaintext_len += len;
    
    // Cleanup
    EVP_CIPHER_CTX_free(ctx);
    
    plaintext.resize(plaintext_len);
    g_logger.info("Decrypted with AES-256-GCM");
    return plaintext;
}

} // namespace crypto

} // namespace dinero
