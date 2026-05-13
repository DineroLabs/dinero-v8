#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include "consensus/coin_type.h"

namespace dinero {

// BIP-39 Mnemonic generation and validation
class BIP39 {
public:
    // Generate mnemonic from entropy
    static std::string generate_mnemonic(int word_count = 12); // 12 or 24 words
    
    // Validate mnemonic
    static bool validate_mnemonic(const std::string& mnemonic);
    
    // Mnemonic to seed (with optional passphrase)
    static std::array<uint8_t, 64> mnemonic_to_seed(
        const std::string& mnemonic, 
        const std::string& passphrase = ""
    );
    
    // Get word list (English)
    static const std::vector<std::string>& get_wordlist();
    
private:
    static std::vector<uint8_t> generate_entropy(int bits); // 128 or 256 bits
    static std::string entropy_to_mnemonic(const std::vector<uint8_t>& entropy);
    static std::vector<uint8_t> mnemonic_to_entropy(const std::string& mnemonic);
    static uint8_t calculate_checksum(const std::vector<uint8_t>& entropy);
};

// BIP-32 HD Key Derivation
class BIP32 {
public:
    struct ExtendedKey {
        std::array<uint8_t, 32> key;        // Private or public key
        std::array<uint8_t, 32> chain_code;
        uint32_t depth;
        uint32_t fingerprint;
        uint32_t child_number;
        bool is_private;
        
        std::string serialize() const; // xprv/xpub format
    };
    
    // Create master key from seed
    static ExtendedKey master_from_seed(const std::array<uint8_t, 64>& seed);
    
    // Derive child key
    static ExtendedKey derive_child(const ExtendedKey& parent, uint32_t index, bool hardened = false);
    
    // Derive path (e.g., "m/84'/1448'/0'/0/0")
    static ExtendedKey derive_path(const ExtendedKey& master, const std::string& path);
    
private:
    static constexpr uint32_t HARDENED_BIT = 0x80000000;
};

// BIP-84 P2WPKH Derivation (Native SegWit)
class BIP84 {
public:
    // Dinero coin type (SLIP-44)
    static constexpr uint32_t COIN_TYPE = dinero::consensus::DINERO_COIN_TYPE;
    
    // Derive receive address at index
    static std::string derive_address(
        const BIP32::ExtendedKey& account_key,
        uint32_t index,
        bool is_change = false
    );
    
    // Create account key (m/84'/coin_type'/0')
    static BIP32::ExtendedKey create_account_key(const std::array<uint8_t, 64>& seed);
    
    // Batch derive addresses
    static std::vector<std::string> derive_addresses(
        const BIP32::ExtendedKey& account_key,
        uint32_t start_index,
        uint32_t count,
        bool is_change = false
    );
};

} // namespace dinero
