#pragma once

#include "crypto/hd_keychain.h"
#include "consensus/coin_type.h"
// #include "simple_wallet.h"  // DISABLED: SimpleWallet removed
#include <memory>
#include <string>
#include <json/json.h>

namespace dinero {

/**
 * HD Wallet Manager - Integrates BIP-39/32/86 with HD key derivation
 * Default: BIP86 Taproot (P2TR) addresses
 */
class HDWalletManager {
public:
    HDWalletManager(const std::string& wallet_file);
    ~HDWalletManager();
    
    // Wallet creation/restoration
    std::string createWallet(int word_count = 12, const std::string& passphrase = "");
    bool restoreWallet(const std::string& mnemonic, const std::string& passphrase = "");
    bool hasWallet() const { return account_key_ != nullptr; }
    
    // Address generation (BIP-86 Taproot by default)
    std::string generateAddress(const std::string& label = "");
    std::string generateChangeAddress();
    std::vector<std::string> getAllAddresses() const;
    
    // Wallet info
    bool isMine(const std::string& address) const;
    uint64_t getBalance() const;
    std::string getMnemonic() const { return mnemonic_; } // Dangerous - only for backup
    uint32_t getCurrentIndex() const;
    
    // Persistence
    bool save();
    bool load();
    
    // Encryption (Argon2id + AES-256-GCM)
    bool encryptWallet(const std::string& password);
    bool lock();
    bool unlock(const std::string& password);
    bool changePassword(const std::string& old_password, const std::string& new_password);
    bool isLocked() const { return locked_; }
    bool isEncrypted() const { return encrypted_; }
    
private:
    std::string wallet_file_;
    // std::unique_ptr<SimpleWallet> simple_wallet_;  // DISABLED: SimpleWallet removed
    std::unique_ptr<crypto::HDKeychain::ExtendedKey> account_key_; // m/86'/1448'/0'
    std::unique_ptr<crypto::BIP86AddressGenerator> address_gen_;
    
    std::string mnemonic_; // Cleared when locked
    std::string passphrase_;
    uint32_t coin_type_;
    uint32_t account_;
    bool locked_;
    bool encrypted_;
    std::string encryption_password_; // Temporary, cleared after use
    
    // Helper methods
    bool restoreFromMnemonic(const std::string& mnemonic);
    
    static constexpr uint32_t DINERO_COIN_TYPE = dinero::consensus::DINERO_COIN_TYPE;
};

} // namespace dinero
