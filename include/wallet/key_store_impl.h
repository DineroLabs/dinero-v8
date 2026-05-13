#pragma once
#include "wallet/wallet_iface.h"
#include "consensus/coin_type.h"
#include <map>
#include <string>
#include <vector>

namespace din {

/**
 * @brief Key store implementation with BIP32 HD wallet support
 * 
 * Handles deterministic key derivation, storage, and signing operations
 * with proper BIP84 P2WPKH support and hardware wallet compatibility.
 */
class KeyStoreImpl : public IKeyStore {
public:
    explicit KeyStoreImpl(const std::string& wallet_name);
    ~KeyStoreImpl();
    
    // Initialization with seed or xpub
    bool initializeFromSeed(const std::vector<uint8_t>& seed, uint32_t coin_type = dinero::consensus::DINERO_COIN_TYPE);
    bool initializeFromXPub(const std::string& xpub, const std::string& fingerprint);
    
    // IKeyStore implementation
    std::optional<std::string> getXPub(const std::string& path) const override;
    std::optional<std::string> getXPriv(const std::string& path) const override;
    std::optional<std::vector<uint8_t>> sign(const std::vector<uint8_t>& hash, 
                                            const std::string& key_path) override;
    bool canSign(const std::string& key_path) const override;
    bool hasKey(const std::string& key_path) const override;
    std::vector<std::string> listKeyPaths() const override;
    
    // BIP84 specific methods (Native SegWit P2WPKH)
    std::string deriveBIP84Address(uint32_t account, bool is_change, uint32_t index) const;
    std::string getBIP84Descriptor(uint32_t account, bool is_change) const;

    // BIP86 specific methods (Taproot P2TR key-path only)
    std::string deriveBIP86Address(uint32_t account, bool is_change, uint32_t index) const;
    std::string getBIP86Descriptor(uint32_t account, bool is_change) const;

    // Security
    bool isWatchOnly() const { return watch_only_; }
    bool isEncrypted() const { return encrypted_; }

private:
    struct CachedPrivateKey {
        std::vector<uint8_t> bytes;
        bool locked = false;
    };

    std::string wallet_name_;
    bool watch_only_;
    bool encrypted_;
    bool master_seed_locked_;
    uint32_t coin_type_;
    
    // Master key data (encrypted in production)
    std::vector<uint8_t> master_seed_; // 64 bytes, encrypted when encrypted_ = true
    std::string master_fingerprint_;   // 8 hex chars
    
    // Derived key cache (for performance)
    mutable std::map<std::string, std::string> xpub_cache_;
    mutable std::map<std::string, CachedPrivateKey> private_key_cache_;
    
    // BIP32 helpers
    std::string deriveXPub(const std::string& path) const;
    std::vector<uint8_t> derivePrivateKey(const std::string& path) const;
    std::string pathToString(const std::vector<uint32_t>& path) const;
    std::vector<uint32_t> pathFromString(const std::string& path) const;
    
    // Crypto helpers
    std::vector<uint8_t> hmacSHA512(const std::vector<uint8_t>& key, 
                                   const std::vector<uint8_t>& data) const;
    std::string computeFingerprint(const std::vector<uint8_t>& pubkey) const;
    void clearMasterSeed();
    void clearPrivateKeyCache();
    static bool tryLockBytes(std::vector<uint8_t>& data);
    static void secureClearBytes(std::vector<uint8_t>& data, bool was_locked = false);
};

} // namespace din
