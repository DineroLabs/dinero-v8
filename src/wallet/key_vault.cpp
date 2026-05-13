#include "wallet/key_vault.h"
#include "database/sqlite_conn.h"
#include "common/logger.h"
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <json/json.h>
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

// KeyVault implementation
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
    
    // Create KDF parameters (prefer Argon2id, fallback to PBKDF2)
    KdfParams params;
    try {
        params = KdfParams::createArgon2id(salt);
    } catch (...) {
        g_logger.info("Argon2id not available, using PBKDF2 for master key derivation");
        params = KdfParams::createPbkdf2(salt);
    }
    
    auto vault = std::unique_ptr<KeyVault>(new KeyVault(params, db_path));
    
    if (!vault->initializeDatabase()) {
        throw std::runtime_error("Failed to initialize key vault database");
    }
    
    if (!vault->unlock(passphrase)) {
        throw std::runtime_error("Failed to derive master key during vault creation");
    }
    
    return vault;
}

std::unique_ptr<KeyVault> KeyVault::open(const std::string& passphrase, const WalletMeta& meta, const std::string& db_path) {
    KdfParams params = KdfParams::fromJson(meta.kdf_params_json);
    
    auto vault = std::unique_ptr<KeyVault>(new KeyVault(params, db_path));
    
    if (!vault->initializeDatabase()) {
        throw std::runtime_error("Failed to open key vault database");
    }
    
    if (!vault->unlock(passphrase)) {
        return nullptr; // Wrong passphrase
    }
    
    return vault;
}

bool KeyVault::initializeDatabase() {
    try {
        db_ = std::make_unique<SqliteHandle>(db_path_);
        return createTables();
    } catch (const std::exception& e) {
        g_logger.error("Failed to initialize key vault database: " + std::string(e.what()));
        return false;
    }
}

bool KeyVault::createTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS wallet_keys (
            fp             BLOB PRIMARY KEY,     -- hash160(pubkey) fingerprint
            enc_blob       BLOB NOT NULL,        -- AES-GCM ciphertext
            iv             BLOB NOT NULL,        -- 12 bytes IV
            created_at     INTEGER NOT NULL,     -- Unix timestamp
            label          TEXT,                 -- User label
            compressed     INTEGER NOT NULL,     -- 0/1
            origin         TEXT,                 -- "import", "mnemonic", "generated"
            kdf_name       TEXT NOT NULL,        -- For reference
            kdf_params     TEXT NOT NULL         -- JSON KDF parameters
        );
        
        CREATE TABLE IF NOT EXISTS wallet_meta (
            key            TEXT PRIMARY KEY,
            value          TEXT NOT NULL
        );
        
        -- Store KDF parameters in meta table
        INSERT OR REPLACE INTO wallet_meta (key, value) VALUES ('kdf_params', ?);
        INSERT OR REPLACE INTO wallet_meta (key, value) VALUES ('created_at', datetime('now'));
        INSERT OR REPLACE INTO wallet_meta (key, value) VALUES ('version', '1.0');
    )";
    
    try {
        db_->exec(sql, kdf_params_.toJson());
        return true;
    } catch (const std::exception& e) {
        g_logger.error("Failed to create key vault tables: " + std::string(e.what()));
        return false;
    }
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
    
    if (kdf_params_.kdf_name == "argon2id") {
        success = crypto::deriveKeyArgon2id(passphrase, kdf_params_.salt, 
                                          kdf_params_.iterations, kdf_params_.memory_kb, 
                                          kdf_params_.parallelism, derived_key);
    } else if (kdf_params_.kdf_name == "pbkdf2-hmac-sha256") {
        success = crypto::deriveKeyPbkdf2(passphrase, kdf_params_.salt, 
                                        kdf_params_.iterations, derived_key);
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
    
    try {
        // Generate random IV for this key
        auto iv = generateIV(12);
        
        // Encrypt the private key
        auto encrypted = crypto::encryptAesGcm(private_key, master_key_.data(), iv);
        
        // Convert fingerprint to blob
        std::vector<uint8_t> fp_blob(fingerprint.begin(), fingerprint.end());
        
        // Store in database
        const char* sql = R"(
            INSERT OR REPLACE INTO wallet_keys 
            (fp, enc_blob, iv, created_at, label, compressed, origin, kdf_name, kdf_params)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";
        
        auto stmt = db_->prepare(sql);
        stmt.bind(1, fp_blob);
        stmt.bind(2, encrypted);
        stmt.bind(3, iv);
        stmt.bind(4, static_cast<int64_t>(time(nullptr)));
        stmt.bind(5, label);
        stmt.bind(6, compressed ? 1 : 0);
        stmt.bind(7, origin);
        stmt.bind(8, kdf_params_.kdf_name);
        stmt.bind(9, kdf_params_.toJson());
        
        stmt.step();
        
        g_logger.info("KeyVault: Stored encrypted key with fingerprint " + 
                     uint160ToString(fingerprint) + " (origin: " + origin + ")");
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("KeyVault: Failed to store key: " + std::string(e.what()));
        return false;
    }
}

bool KeyVault::load(const uint160& fingerprint, std::array<uint8_t, 32>& out_private_key,
                   bool& compressed, std::string* label) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (locked_) {
        g_logger.error("KeyVault: Cannot load key while vault is locked");
        return false;
    }
    
    try {
        std::vector<uint8_t> fp_blob(fingerprint.begin(), fingerprint.end());
        
        const char* sql = "SELECT enc_blob, iv, compressed, label FROM wallet_keys WHERE fp = ?";
        auto stmt = db_->prepare(sql);
        stmt.bind(1, fp_blob);
        
        if (!stmt.step()) {
            return false; // Key not found
        }
        
        auto encrypted = stmt.getBlob(0);
        auto iv = stmt.getBlob(1);
        compressed = stmt.getInt(2) != 0;
        if (label) {
            *label = stmt.getText(3);
        }
        
        // Decrypt the private key
        auto decrypted = crypto::decryptAesGcm(encrypted, master_key_.data(), iv);
        
        if (decrypted.size() != 32) {
            g_logger.error("KeyVault: Decrypted key has wrong size: " + std::to_string(decrypted.size()));
            return false;
        }
        
        std::copy(decrypted.begin(), decrypted.end(), out_private_key.begin());
        
        // Securely clear decrypted data
        OPENSSL_cleanse(decrypted.data(), decrypted.size());
        
        return true;
        
    } catch (const std::exception& e) {
        g_logger.error("KeyVault: Failed to load key: " + std::string(e.what()));
        return false;
    }
}

bool KeyVault::has(const uint160& fingerprint) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        std::vector<uint8_t> fp_blob(fingerprint.begin(), fingerprint.end());
        
        const char* sql = "SELECT 1 FROM wallet_keys WHERE fp = ?";
        auto stmt = db_->prepare(sql);
        stmt.bind(1, fp_blob);
        
        return stmt.step();
        
    } catch (const std::exception& e) {
        g_logger.error("KeyVault: Failed to check key existence: " + std::string(e.what()));
        return false;
    }
}

std::vector<std::tuple<uint160, std::string, bool, std::string>> KeyVault::listKeys() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::tuple<uint160, std::string, bool, std::string>> result;
    
    try {
        const char* sql = "SELECT fp, label, compressed, origin FROM wallet_keys ORDER BY created_at";
        auto stmt = db_->prepare(sql);
        
        while (stmt.step()) {
            auto fp_blob = stmt.getBlob(0);
            std::string label = stmt.getText(1);
            bool compressed = stmt.getInt(2) != 0;
            std::string origin = stmt.getText(3);
            
            if (fp_blob.size() == 20) {
                uint160 fingerprint;
                std::copy(fp_blob.begin(), fp_blob.end(), fingerprint.begin());
                result.emplace_back(fingerprint, label, compressed, origin);
            }
        }
        
    } catch (const std::exception& e) {
        g_logger.error("KeyVault: Failed to list keys: " + std::string(e.what()));
    }
    
    return result;
}

bool KeyVault::changePassphrase(const std::string& old_passphrase, const std::string& new_passphrase) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Verify old passphrase
    SecureBuffer<32> old_master;
    std::array<uint8_t, 32> temp_key;
    bool old_valid = false;
    
    if (kdf_params_.kdf_name == "argon2id") {
        old_valid = crypto::deriveKeyArgon2id(old_passphrase, kdf_params_.salt,
                                            kdf_params_.iterations, kdf_params_.memory_kb,
                                            kdf_params_.parallelism, temp_key);
    } else if (kdf_params_.kdf_name == "pbkdf2-hmac-sha256") {
        old_valid = crypto::deriveKeyPbkdf2(old_passphrase, kdf_params_.salt,
                                          kdf_params_.iterations, temp_key);
    }
    
    if (!old_valid) {
        OPENSSL_cleanse(temp_key.data(), temp_key.size());
        return false;
    }
    
    std::copy(temp_key.begin(), temp_key.end(), old_master.data());
    OPENSSL_cleanse(temp_key.data(), temp_key.size());
    
    // Generate new salt and derive new master key
    auto new_salt = generateSalt(32);
    KdfParams new_params = kdf_params_;
    new_params.salt = new_salt;
    
    SecureBuffer<32> new_master;
    bool new_valid = false;
    
    if (new_params.kdf_name == "argon2id") {
        new_valid = crypto::deriveKeyArgon2id(new_passphrase, new_params.salt,
                                            new_params.iterations, new_params.memory_kb,
                                            new_params.parallelism, temp_key);
    } else if (new_params.kdf_name == "pbkdf2-hmac-sha256") {
        new_valid = crypto::deriveKeyPbkdf2(new_passphrase, new_params.salt,
                                          new_params.iterations, temp_key);
    }
    
    if (!new_valid) {
        OPENSSL_cleanse(temp_key.data(), temp_key.size());
        return false;
    }
    
    std::copy(temp_key.begin(), temp_key.end(), new_master.data());
    OPENSSL_cleanse(temp_key.data(), temp_key.size());
    
    try {
        // Begin transaction
        db_->exec("BEGIN TRANSACTION");
        
        // Get all encrypted keys
        const char* select_sql = "SELECT fp, enc_blob, iv FROM wallet_keys";
        auto select_stmt = db_->prepare(select_sql);
        
        std::vector<std::tuple<std::vector<uint8_t>, std::vector<uint8_t>, std::vector<uint8_t>>> keys_to_rewrap;
        
        while (select_stmt.step()) {
            keys_to_rewrap.emplace_back(
                select_stmt.getBlob(0),  // fingerprint
                select_stmt.getBlob(1),  // encrypted blob
                select_stmt.getBlob(2)   // iv
            );
        }
        
        // Rewrap each key
        const char* update_sql = "UPDATE wallet_keys SET enc_blob = ?, iv = ?, kdf_params = ? WHERE fp = ?";
        
        for (const auto& [fp, old_encrypted, old_iv] : keys_to_rewrap) {
            // Decrypt with old master key
            auto decrypted = crypto::decryptAesGcm(old_encrypted, old_master.data(), old_iv);
            
            if (decrypted.size() != 32) {
                throw std::runtime_error("Invalid decrypted key size during rewrap");
            }
            
            // Generate new IV and encrypt with new master key
            auto new_iv = generateIV(12);
            auto new_encrypted = crypto::encryptAesGcm(std::span<const uint8_t>(decrypted), new_master.data(), new_iv);
            
            // Update database
            auto update_stmt = db_->prepare(update_sql);
            update_stmt.bind(1, new_encrypted);
            update_stmt.bind(2, new_iv);
            update_stmt.bind(3, new_params.toJson());
            update_stmt.bind(4, fp);
            update_stmt.step();
            
            // Securely clear decrypted key
            OPENSSL_cleanse(decrypted.data(), decrypted.size());
        }
        
        // Update KDF parameters in meta table
        const char* meta_sql = "UPDATE wallet_meta SET value = ? WHERE key = 'kdf_params'";
        auto meta_stmt = db_->prepare(meta_sql);
        meta_stmt.bind(1, new_params.toJson());
        meta_stmt.step();
        
        // Commit transaction
        db_->exec("COMMIT");
        
        // Update instance parameters and master key
        kdf_params_ = new_params;
        std::copy(new_master.data(), new_master.data() + 32, master_key_.data());
        locked_ = false;
        
        g_logger.info("KeyVault: Successfully changed passphrase and rewrapped " + 
                     std::to_string(keys_to_rewrap.size()) + " keys");
        
        return true;
        
    } catch (const std::exception& e) {
        db_->exec("ROLLBACK");
        g_logger.error("KeyVault: Failed to change passphrase: " + std::string(e.what()));
        return false;
    }
}

WalletMeta KeyVault::getWalletMeta() const {
    WalletMeta meta;
    meta.kdf_params_json = kdf_params_.toJson();
    
    try {
        const char* sql = "SELECT value FROM wallet_meta WHERE key = ?";
        
        auto stmt = db_->prepare(sql);
        stmt.bind(1, std::string("created_at"));
        if (stmt.step()) {
            meta.created_at = stmt.getText(0);
        }
        
        stmt = db_->prepare(sql);
        stmt.bind(1, std::string("version"));
        if (stmt.step()) {
            meta.version = stmt.getText(0);
        }
        
    } catch (const std::exception& e) {
        g_logger.error("KeyVault: Failed to get wallet meta: " + std::string(e.what()));
    }
    
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
    // For now, fallback to PBKDF2 if Argon2 is not available
    // In production, you would use libargon2 or OpenSSL 3.2+ EVP_KDF with Argon2
    g_logger.info("Argon2id not implemented, falling back to PBKDF2");
    return deriveKeyPbkdf2(passphrase, salt, std::max(iterations * 1000, 100000), out_key);
}

std::vector<uint8_t> encryptAesGcm(std::span<const uint8_t> plaintext,
                                  const std::array<uint8_t, 32>& key,
                                  const std::vector<uint8_t>& iv) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }
    
    try {
        // Initialize encryption
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            throw std::runtime_error("Failed to initialize AES-GCM encryption");
        }
        
        // Set IV length
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1) {
            throw std::runtime_error("Failed to set IV length");
        }
        
        // Set key and IV
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
            throw std::runtime_error("Failed to set key and IV");
        }
        
        // Encrypt
        std::vector<uint8_t> ciphertext(plaintext.size());
        int len = 0;
        if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), static_cast<int>(plaintext.size())) != 1) {
            throw std::runtime_error("Failed to encrypt data");
        }
        
        // Finalize
        int final_len = 0;
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &final_len) != 1) {
            throw std::runtime_error("Failed to finalize encryption");
        }
        
        // Get authentication tag
        std::vector<uint8_t> tag(16);
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) {
            throw std::runtime_error("Failed to get authentication tag");
        }
        
        // Combine ciphertext + tag
        ciphertext.resize(len + final_len);
        ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
        
        EVP_CIPHER_CTX_free(ctx);
        return ciphertext;
        
    } catch (...) {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }
}

std::vector<uint8_t> decryptAesGcm(const std::vector<uint8_t>& ciphertext_with_tag,
                                  const std::array<uint8_t, 32>& key,
                                  const std::vector<uint8_t>& iv) {
    if (ciphertext_with_tag.size() < 16) {
        throw std::runtime_error("Ciphertext too short for GCM tag");
    }
    
    size_t ciphertext_len = ciphertext_with_tag.size() - 16;
    const uint8_t* ciphertext = ciphertext_with_tag.data();
    const uint8_t* tag = ciphertext_with_tag.data() + ciphertext_len;
    
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create cipher context");
    }
    
    try {
        // Initialize decryption
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            throw std::runtime_error("Failed to initialize AES-GCM decryption");
        }
        
        // Set IV length
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(iv.size()), nullptr) != 1) {
            throw std::runtime_error("Failed to set IV length");
        }
        
        // Set key and IV
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv.data()) != 1) {
            throw std::runtime_error("Failed to set key and IV");
        }
        
        // Decrypt
        std::vector<uint8_t> plaintext(ciphertext_len);
        int len = 0;
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext, static_cast<int>(ciphertext_len)) != 1) {
            throw std::runtime_error("Failed to decrypt data");
        }
        
        // Set expected tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) != 1) {
            throw std::runtime_error("Failed to set authentication tag");
        }
        
        // Finalize and verify tag
        int final_len = 0;
        if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &final_len) != 1) {
            throw std::runtime_error("Authentication failed (bad tag or corrupted data)");
        }
        
        plaintext.resize(len + final_len);
        
        EVP_CIPHER_CTX_free(ctx);
        return plaintext;
        
    } catch (...) {
        EVP_CIPHER_CTX_free(ctx);
        throw;
    }
}

} // namespace crypto

} // namespace dinero
