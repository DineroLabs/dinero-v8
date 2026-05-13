#include "hd_wallet_manager.h"
#include "secure_random.h"
#include "crypto/bip39.hpp"
#include "wallet_crypto.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <openssl/crypto.h>  // OPENSSL_cleanse
#ifdef _WIN32
#include <io.h>      // _commit, _fileno
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace dinero {

HDWalletManager::HDWalletManager(const std::string& wallet_file)
    : wallet_file_(wallet_file)
    , coin_type_(DINERO_COIN_TYPE)
    , account_(0)
    , locked_(false)
    , encrypted_(false) {

    // simple_wallet_ = std::make_unique<SimpleWallet>(wallet_file);  // DISABLED: SimpleWallet removed

    // Try to load existing wallet
    load();
}

HDWalletManager::~HDWalletManager() {
    // Compiler-proof zeroization of sensitive data (std::fill can be optimized out)
    if (!mnemonic_.empty()) {
        OPENSSL_cleanse(mnemonic_.data(), mnemonic_.size());
        mnemonic_.clear();
    }
    if (!passphrase_.empty()) {
        OPENSSL_cleanse(passphrase_.data(), passphrase_.size());
        passphrase_.clear();
    }
    if (!encryption_password_.empty()) {
        OPENSSL_cleanse(encryption_password_.data(), encryption_password_.size());
        encryption_password_.clear();
    }
}

std::string HDWalletManager::createWallet(int word_count, const std::string& passphrase) {
    // Generate entropy (128 bits for 12 words, 256 bits for 24 words)
    int entropy_bytes = (word_count == 24) ? 32 : 16;
    std::vector<uint8_t> entropy(entropy_bytes);
    
    if (!SecureRandom::GetBytes(entropy.data(), entropy_bytes)) {
        throw std::runtime_error("Failed to generate entropy");
    }
    
    // Generate mnemonic from entropy
    mnemonic_ = dinero::bip39::mnemonic_from_entropy(entropy.data(), entropy_bytes);
    passphrase_ = passphrase;
    
    // Derive seed from mnemonic
    uint8_t seed[64];
    dinero::bip39::mnemonic_to_seed(mnemonic_, passphrase_, seed);
    
    // Create master key from seed
    std::vector<uint8_t> seed_vec(seed, seed + 64);
    auto master = crypto::HDKeychain::fromSeed(seed_vec);
    
    // Derive BIP-84 account key: m/84'/coin'/account'
    account_key_ = std::make_unique<crypto::HDKeychain::ExtendedKey>(
        crypto::HDKeychain::getBIP86Account(master, coin_type_, account_)
    );
    
    // Create address generator
    address_gen_ = std::make_unique<crypto::BIP86AddressGenerator>(*account_key_, "din");
    
    // Save wallet
    save();
    
    printf("✅ Created new HD wallet with %d words\n", word_count);
    printf("⚠️  WRITE DOWN YOUR SEED PHRASE - This is shown ONCE!\n");
    
    return mnemonic_;
}

bool HDWalletManager::restoreWallet(const std::string& mnemonic, const std::string& passphrase) {
    // Validate mnemonic
    auto words = dinero::bip39::split(mnemonic);
    if (words.size() != 12 && words.size() != 24) {
        fprintf(stderr, "Invalid mnemonic: must be 12 or 24 words\n");
        return false;
    }
    
    mnemonic_ = mnemonic;
    passphrase_ = passphrase;
    
    // Derive seed
    uint8_t seed[64];
    dinero::bip39::mnemonic_to_seed(mnemonic_, passphrase_, seed);
    
    // Create master key
    std::vector<uint8_t> seed_vec(seed, seed + 64);
    auto master = crypto::HDKeychain::fromSeed(seed_vec);
    
    // Derive account key
    account_key_ = std::make_unique<crypto::HDKeychain::ExtendedKey>(
        crypto::HDKeychain::getBIP86Account(master, coin_type_, account_)
    );
    
    // Create address generator
    address_gen_ = std::make_unique<crypto::BIP86AddressGenerator>(*account_key_, "din");
    
    // Save
    save();
    
    printf("✅ Restored HD wallet from mnemonic\n");
    return true;
}

std::string HDWalletManager::generateAddress(const std::string& label) {
    if (!address_gen_) {
        throw std::runtime_error("Wallet not initialized - create or restore wallet first");
    }
    
    // Generate next BIP-84 address
    std::string address = address_gen_->getNextAddress();

    // Add to simple wallet for tracking
    // simple_wallet_->add_address(address, label.empty() ? "BIP-84 address" : label);  // DISABLED

    save();
    
    return address;
}

std::string HDWalletManager::generateChangeAddress() {
    if (!address_gen_) {
        throw std::runtime_error("Wallet not initialized");
    }
    
    std::string address = address_gen_->getNextChangeAddress();
    // simple_wallet_->add_address(address, "Change address");  // DISABLED
    save();
    
    return address;
}

std::vector<std::string> HDWalletManager::getAllAddresses() const {
    // return simple_wallet_->get_all_addresses();  // DISABLED
    return {};  // TODO: Implement without SimpleWallet
}

bool HDWalletManager::isMine(const std::string& address) const {
    // return simple_wallet_->is_mine(address);  // DISABLED
    return false;  // TODO: Implement without SimpleWallet
}

uint64_t HDWalletManager::getBalance() const {
    // return simple_wallet_->get_total_balance();  // DISABLED
    return 0;  // TODO: Implement without SimpleWallet
}

uint32_t HDWalletManager::getCurrentIndex() const {
    if (address_gen_) {
        return address_gen_->getCurrentIndex();
    }
    return 0;
}

bool HDWalletManager::save() {
    try {
        Json::Value root;
        root["schema"] = 1;  // Version the format for future upgrades
        
        // Metadata (authenticated via AAD when encrypted)
        Json::Value meta;
        meta["coin_type"] = coin_type_;
        meta["account"] = account_;
        meta["gap_limit"] = 20;  // Default BIP-44 gap limit
        
        if (address_gen_) {
            meta["next_index_recv"] = address_gen_->getCurrentIndex();
            meta["next_index_change"] = address_gen_->getCurrentChangeIndex();
        } else {
            meta["next_index_recv"] = 0;
            meta["next_index_change"] = 0;
        }
        
        root["meta"] = meta;
        root["has_hd"] = (account_key_ != nullptr);
        root["encrypted"] = encrypted_;
        
        // SECURITY: Never persist plaintext mnemonic. Even when runtime wallet
        // is "unencrypted", mnemonic is still encrypted-at-rest with empty password.
        if (!mnemonic_.empty()) {
            // Build AAD from metadata (prevents tampering)
            Json::StreamWriterBuilder aad_builder;
            aad_builder["indentation"] = "";
            std::string aad_json = Json::writeString(aad_builder, meta);
            
            // KDF params
            Json::Value kdf;
            kdf["type"] = "argon2id";
            kdf["m"] = 65536;  // 64 MB
            kdf["t"] = 3;      // iterations
            kdf["p"] = 1;      // parallelism
            root["kdf"] = kdf;

            std::string storage_password;
            if (encrypted_) {
                if (encryption_password_.empty()) {
                    fprintf(stderr, "Refusing to save encrypted wallet without password\n");
                    return false;
                }
                storage_password = encryption_password_;
            } else {
                storage_password = "";  // unencrypted runtime mode, encrypted-at-rest
            }
            
            // Encrypt mnemonic with AAD
            std::vector<uint8_t> encrypted_data;
            if (!WalletCrypto::encrypt(mnemonic_, storage_password, aad_json, encrypted_data)) {
                fprintf(stderr, "Failed to encrypt mnemonic\n");
                return false;
            }
            
            // Store crypto data
            Json::Value crypto;
            crypto["cipher"] = "aes-256-gcm";
            crypto["data"] = WalletCrypto::base64Encode(encrypted_data);
            root["crypto"] = crypto;
            
            if (!passphrase_.empty()) {
                root["has_passphrase"] = true; // Don't save actual passphrase
            }
        }
        
        // Atomic save: write to temp file, then rename
        std::string temp_file = wallet_file_ + ".hd.tmp";
        std::string final_file = wallet_file_ + ".hd";
        
        FILE* fp = fopen(temp_file.c_str(), "w");
        if (!fp) {
            fprintf(stderr, "Failed to open temp wallet file\n");
            return false;
        }
        
        // Set restrictive permissions (0600) on Unix
        #ifndef _WIN32
        chmod(temp_file.c_str(), S_IRUSR | S_IWUSR);  // rw-------
        #endif
        
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::unique_ptr<Json::StreamWriter> writer(builder.newStreamWriter());
        std::ostringstream os;
        writer->write(root, &os);
        std::string json_str = os.str();
        
        size_t written = fwrite(json_str.c_str(), 1, json_str.size(), fp);
        fflush(fp);  // Ensure data is written
#ifdef _WIN32
        _commit(_fileno(fp));  // Sync to disk (Windows)
#else
        fsync(fileno(fp));  // Sync to disk (POSIX)
#endif
        fclose(fp);
        
        if (written != json_str.size()) {
            std::remove(temp_file.c_str());
            fprintf(stderr, "Failed to write complete wallet file\n");
            return false;
        }
        
        // Atomic rename (crash-safe)
        if (std::rename(temp_file.c_str(), final_file.c_str()) != 0) {
            std::remove(temp_file.c_str());
            fprintf(stderr, "Failed to rename wallet file\n");
            return false;
        }

        // Also save simple wallet addresses
        // simple_wallet_->save();  // DISABLED

        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "Failed to save HD wallet: %s\n", e.what());
        return false;
    }
}

bool HDWalletManager::load() {
    try {
        FILE* fp = fopen((wallet_file_ + ".hd").c_str(), "r");
        if (!fp) {
            return false; // No HD wallet yet
        }
        
        // Read file into string
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        std::string json_str(fsize, '\0');
        fread(&json_str[0], 1, fsize, fp);
        fclose(fp);
        
        Json::Value root;
        Json::CharReaderBuilder builder;
        std::istringstream is(json_str);
        std::string errs;
        if (!Json::parseFromStream(builder, is, &root, &errs)) {
            fprintf(stderr, "JSON parse error: %s\n", errs.c_str());
            return false;
        }
        
        // Handle both old and new schema
        int schema = root.get("schema", 0).asInt();
        
        // Extract metadata
        Json::Value meta;
        if (schema >= 1 && root.isMember("meta")) {
            meta = root["meta"];
            coin_type_ = meta.get("coin_type", DINERO_COIN_TYPE).asUInt();
            account_ = meta.get("account", 0).asUInt();
        } else {
            // Old format
            coin_type_ = root.get("coin_type", DINERO_COIN_TYPE).asUInt();
            account_ = root.get("account", 0).asUInt();
            meta["coin_type"] = coin_type_;
            meta["account"] = account_;
            meta["gap_limit"] = 20;
        }
        
        const bool has_crypto = root.isMember("crypto");
        const bool has_legacy_encrypted = root.isMember("encrypted_mnemonic");
        const bool has_legacy_plaintext = root.isMember("mnemonic_plaintext");

        // New format writes explicit encrypted flag; old format inferred from payload.
        if (root.isMember("encrypted")) {
            encrypted_ = root["encrypted"].asBool();
        } else {
            encrypted_ = has_crypto || has_legacy_encrypted;
        }
        
        if (root.get("has_hd", false).asBool()) {
            // Load seed (decrypt if encrypted)
            if (has_crypto) {
                // New format with AAD
                Json::Value crypto = root["crypto"];
                std::string decrypt_password;
                
                if (encrypted_) {
                    if (encryption_password_.empty()) {
                        // No password provided yet, wallet stays locked
                        locked_ = true;
                        printf("🔒 Encrypted wallet loaded (locked)\n");
                        return true;
                    }
                    decrypt_password = encryption_password_;
                } else {
                    decrypt_password = "";  // unencrypted runtime mode
                    locked_ = false;
                }
                
                // Build AAD from metadata
                Json::StreamWriterBuilder aad_builder;
                aad_builder["indentation"] = "";
                std::string aad_json = Json::writeString(aad_builder, meta);
                
                // Decrypt with AAD verification
                std::string encrypted_b64 = crypto["data"].asString();
                std::vector<uint8_t> encrypted_data;
                
                if (!WalletCrypto::base64Decode(encrypted_b64, encrypted_data)) {
                    fprintf(stderr, "Failed to decode encrypted seed\n");
                    return false;
                }
                
                if (!WalletCrypto::decrypt(encrypted_data, decrypt_password, aad_json, mnemonic_)) {
                    fprintf(stderr, "Failed to decrypt seed - wrong password or tampered metadata\n");
                    return false;
                }
            } else if (has_legacy_encrypted) {
                // Old format without AAD (still supported)
                if (encryption_password_.empty()) {
                    locked_ = true;
                    printf("🔒 Encrypted wallet loaded (locked)\n");
                    return true;
                }
                
                std::string encrypted_b64 = root["encrypted_mnemonic"].asString();
                std::vector<uint8_t> encrypted_data;
                
                if (!WalletCrypto::base64Decode(encrypted_b64, encrypted_data)) {
                    fprintf(stderr, "Failed to decode encrypted mnemonic\n");
                    return false;
                }
                
                // Decrypt without AAD (old format)
                if (!WalletCrypto::decrypt(encrypted_data, encryption_password_, "", mnemonic_)) {
                    fprintf(stderr, "Failed to decrypt mnemonic - wrong password?\n");
                    return false;
                }
            } else if (has_legacy_plaintext) {
                fprintf(stderr,
                        "Refusing to load legacy plaintext mnemonic storage. "
                        "Migrate wallet to encrypted-at-rest format.\n");
                return false;
            } else {
                fprintf(stderr, "No seed found in wallet file\n");
                return false;
            }
            
            if (!mnemonic_.empty()) {
                // Restore from mnemonic
                if (!restoreFromMnemonic(mnemonic_)) {
                    return false;
                }
                
                if (encrypted_) {
                    printf("✅ Loaded encrypted HD wallet\n");
                } else {
                    printf("✅ Loaded HD wallet\n");
                }
                return true;
            }
        }
        
        return false;
    } catch (const std::exception& e) {
        fprintf(stderr, "Failed to load HD wallet: %s\n", e.what());
        return false;
    }
}

// Encryption methods

bool HDWalletManager::encryptWallet(const std::string& password) {
    if (encrypted_) {
        fprintf(stderr, "Wallet is already encrypted\n");
        return false;
    }
    
    if (password.empty()) {
        fprintf(stderr, "Password cannot be empty\n");
        return false;
    }
    
    if (mnemonic_.empty()) {
        fprintf(stderr, "No wallet to encrypt\n");
        return false;
    }
    
    encrypted_ = true;
    encryption_password_ = password;
    
    if (!save()) {
        encrypted_ = false;
        encryption_password_.clear();
        return false;
    }
    
    printf("✅ Wallet encrypted with Argon2id + AES-256-GCM\n");
    printf("⚠️  Wallet is now locked. Use unlock() to access.\n");
    
    // Lock immediately after encryption
    lock();
    
    return true;
}

bool HDWalletManager::lock() {
    if (!encrypted_) {
        fprintf(stderr, "Wallet is not encrypted, cannot lock\n");
        return false;
    }
    
    // Clear sensitive data from memory
    if (!mnemonic_.empty()) {
        std::fill(mnemonic_.begin(), mnemonic_.end(), '0');
        mnemonic_.clear();
    }
    if (!encryption_password_.empty()) {
        std::fill(encryption_password_.begin(), encryption_password_.end(), '0');
        encryption_password_.clear();
    }
    
    // Clear keys
    account_key_.reset();
    address_gen_.reset();
    
    locked_ = true;
    printf("🔒 Wallet locked\n");
    
    return true;
}

bool HDWalletManager::unlock(const std::string& password) {
    if (!encrypted_) {
        fprintf(stderr, "Wallet is not encrypted\n");
        return false;
    }
    
    if (!locked_) {
        printf("Wallet is already unlocked\n");
        return true;
    }
    
    if (password.empty()) {
        fprintf(stderr, "Password cannot be empty\n");
        return false;
    }
    
    // Temporarily set password to decrypt
    encryption_password_ = password;
    
    // Reload wallet (will decrypt mnemonic)
    bool success = load();
    
    if (success) {
        locked_ = false;
        printf("🔓 Wallet unlocked\n");
    } else {
        // Wrong password or corrupted wallet
        std::fill(encryption_password_.begin(), encryption_password_.end(), '0');
        encryption_password_.clear();
        fprintf(stderr, "Failed to unlock wallet - wrong password or corrupted data\n");
    }
    
    return success;
}

bool HDWalletManager::changePassword(const std::string& old_password, const std::string& new_password) {
    if (!encrypted_) {
        fprintf(stderr, "Wallet is not encrypted\n");
        return false;
    }
    
    if (old_password.empty() || new_password.empty()) {
        fprintf(stderr, "Passwords cannot be empty\n");
        return false;
    }
    
    // Unlock with old password
    if (locked_) {
        if (!unlock(old_password)) {
            return false;
        }
    } else {
        // Verify old password
        if (encryption_password_ != old_password) {
            fprintf(stderr, "Old password is incorrect\n");
            return false;
        }
    }
    
    // Change password and save
    encryption_password_ = new_password;
    
    if (!save()) {
        fprintf(stderr, "Failed to save wallet with new password\n");
        encryption_password_ = old_password;
        return false;
    }
    
    printf("✅ Password changed successfully\n");
    return true;
}

bool HDWalletManager::restoreFromMnemonic(const std::string& mnemonic) {
    // Derive seed
    uint8_t seed[64];
    dinero::bip39::mnemonic_to_seed(mnemonic, passphrase_, seed);
    
    // Create master key
    std::vector<uint8_t> seed_vec(seed, seed + 64);
    auto master = crypto::HDKeychain::fromSeed(seed_vec);
    
    // Derive account key
    account_key_ = std::make_unique<crypto::HDKeychain::ExtendedKey>(
        crypto::HDKeychain::getBIP86Account(master, coin_type_, account_)
    );
    
    // Create address generator
    address_gen_ = std::make_unique<crypto::BIP86AddressGenerator>(*account_key_, "din");
    
    return true;
}

} // namespace dinero
