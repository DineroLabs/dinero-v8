#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <memory>

// Forward declarations to avoid secp256k1 header in public interface
typedef struct secp256k1_context_struct secp256k1_context;

namespace dinero {
namespace crypto {

/**
 * BIP32 HD Key derivation implementation
 * Supports BIP84 (P2WPKH) and BIP86 (P2TR) derivation paths
 * Default: BIP86 Taproot at m/86'/coin'/account'/change/index
 */
class HDKeychain {
public:
    static constexpr size_t CHAINCODE_SIZE = 32;
    static constexpr size_t PRIVKEY_SIZE = 32;
    static constexpr size_t PUBKEY_SIZE = 33; // Compressed
    static constexpr size_t XONLY_SIZE = 32;  // x-only pubkey for Taproot
    static constexpr size_t HASH160_SIZE = 20;

    // BIP purpose constants
    static constexpr uint32_t BIP84_PURPOSE = 84;
    static constexpr uint32_t BIP86_PURPOSE = 86;
    static constexpr uint32_t HARDENED_KEY_START = 0x80000000;
    
    struct ExtendedKey {
        std::array<uint8_t, CHAINCODE_SIZE> chain_code;
        std::array<uint8_t, PRIVKEY_SIZE> private_key;
        std::array<uint8_t, PUBKEY_SIZE> public_key;
        uint32_t fingerprint;
        uint8_t depth;
        uint32_t child_number;
        bool is_private;
        
        ExtendedKey();
        ~ExtendedKey();
        
        // Derive child key at given index
        ExtendedKey derive(uint32_t index) const;
        
        // Get compressed public key
        std::array<uint8_t, PUBKEY_SIZE> getPublicKey() const;
        
        // Get address from public key (P2WPKH) — legacy BIP84
        std::string getAddress(const std::string& hrp) const;

        // Get Taproot address from public key (P2TR) — BIP86 default
        // Computes x-only pubkey, applies BIP341 key tweak, bech32m encodes
        std::string getTaprootAddress(const std::string& hrp) const;

        // Get x-only public key (32 bytes, drops sign byte from compressed)
        std::array<uint8_t, XONLY_SIZE> getXOnlyPubkey() const;

        // Get HASH160 of public key
        std::array<uint8_t, HASH160_SIZE> getHash160() const;

        // Serialize to xpub/xprv format
        std::string serialize(bool mainnet = true) const;
    };
    
    // Create master key from seed
    static ExtendedKey fromSeed(const std::vector<uint8_t>& seed);
    
    // Create master key from mnemonic
    static ExtendedKey fromMnemonic(const std::string& mnemonic, const std::string& passphrase = "");
    
    // BIP84 derivation: m/84'/coin'/account'/change/index (legacy P2WPKH)
    static ExtendedKey deriveBIP84(const ExtendedKey& master, uint32_t coin_type,
                                   uint32_t account, uint32_t change, uint32_t index);

    // Get BIP84 account extended key: m/84'/coin'/account' (legacy P2WPKH)
    static ExtendedKey getBIP84Account(const ExtendedKey& master, uint32_t coin_type, uint32_t account);

    // BIP86 derivation: m/86'/coin'/account'/change/index (default P2TR Taproot)
    static ExtendedKey deriveBIP86(const ExtendedKey& master, uint32_t coin_type,
                                   uint32_t account, uint32_t change, uint32_t index);

    // Get BIP86 account extended key: m/86'/coin'/account' (default P2TR Taproot)
    static ExtendedKey getBIP86Account(const ExtendedKey& master, uint32_t coin_type, uint32_t account);
    
private:
    static secp256k1_context* getContext();
    static void hmacSha512(const uint8_t* key, size_t key_len, 
                          const uint8_t* data, size_t data_len, 
                          uint8_t* out);
};

/**
 * BIP84 Address Generator
 * Implements gap limit scanning and address discovery
 */
class BIP84AddressGenerator {
public:
    static constexpr uint32_t DEFAULT_GAP_LIMIT = 20;
    static constexpr uint32_t EXTERNAL_CHAIN = 0;
    static constexpr uint32_t INTERNAL_CHAIN = 1; // Change addresses
    
    BIP84AddressGenerator(const HDKeychain::ExtendedKey& account_key, 
                         const std::string& hrp, 
                         uint32_t gap_limit = DEFAULT_GAP_LIMIT);
    
    // Generate next external (receiving) address
    std::string getNextAddress();
    
    // Generate next internal (change) address  
    std::string getNextChangeAddress();
    
    // Generate address at specific index
    std::string getAddress(uint32_t chain, uint32_t index);
    
    // Get current external index
    uint32_t getCurrentIndex() const { return external_index_; }
    
    // Get current change index
    uint32_t getCurrentChangeIndex() const { return internal_index_; }
    
    // Check if address belongs to this wallet (within gap limit)
    bool isOwnAddress(const std::string& address) const;
    
    // Scan for used addresses up to gap limit
    std::vector<std::string> scanAddresses(uint32_t chain, uint32_t start_index = 0);
    
private:
    HDKeychain::ExtendedKey account_key_;
    std::string hrp_;
    uint32_t gap_limit_;
    uint32_t external_index_;
    uint32_t internal_index_;
};

/**
 * BIP86 Taproot Address Generator (DEFAULT)
 * Produces P2TR bech32m addresses at m/86'/coin'/account'/chain/index
 */
class BIP86AddressGenerator {
public:
    static constexpr uint32_t DEFAULT_GAP_LIMIT = 20;
    static constexpr uint32_t EXTERNAL_CHAIN = 0;
    static constexpr uint32_t INTERNAL_CHAIN = 1;

    BIP86AddressGenerator(const HDKeychain::ExtendedKey& account_key,
                         const std::string& hrp,
                         uint32_t gap_limit = DEFAULT_GAP_LIMIT);

    std::string getNextAddress();
    std::string getNextChangeAddress();
    std::string getAddress(uint32_t chain, uint32_t index);
    uint32_t getCurrentIndex() const { return external_index_; }
    uint32_t getCurrentChangeIndex() const { return internal_index_; }
    bool isOwnAddress(const std::string& address) const;
    std::vector<std::string> scanAddresses(uint32_t chain, uint32_t start_index = 0);

private:
    HDKeychain::ExtendedKey account_key_;
    std::string hrp_;
    uint32_t gap_limit_;
    uint32_t external_index_;
    uint32_t internal_index_;
};

} // namespace crypto
} // namespace dinero
