#pragma once
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <map>
#include <chrono>
#include "compat/jsoncpp_compat.h"
#include "dinero/core/wallet/wallet_balance_service.h"
#include "consensus/coin_type.h"
#include "wallet/utxo_index.h"
// Crypto operations now handled by dinero_crypto_minimal

namespace dinero {

// Address types
enum class AddressType {
    P2PKH,     // Pay to Public Key Hash (legacy)
    P2SH,      // Pay to Script Hash
    BECH32,    // Native SegWit
    BECH32M,   // Native SegWit v1+ (Taproot)
    DINERO_P2PKH,  // Dinero P2PKH (starts with 'D')
    DINERO_P2SH    // Dinero P2SH
};

// Address version bytes
namespace AddressVersion {
    // Bitcoin-compatible versions (for compatibility)
    const uint8_t MAINNET_P2PKH = 0x00;  // '1' addresses
    const uint8_t TESTNET_P2PKH = 0x6F;  // 'm' or 'n' addresses
    const uint8_t MAINNET_P2SH = 0x05;   // '3' addresses
    const uint8_t TESTNET_P2SH = 0xC4;   // '2' addresses
    const uint8_t BECH32_MAINNET = 0x00; // 'bc1' addresses
    const uint8_t BECH32_TESTNET = 0x00; // 'tb1' addresses
    
    // Dinero-specific versions (for distinct branding)
    const uint8_t DINERO_P2PKH = 0x1E;  // 'D' addresses (starts with D)
    const uint8_t DINERO_P2SH = 0x3F;   // Dinero P2SH addresses  
    const uint8_t DINERO_WIF = 0x9E;    // Dinero WIF private keys
    const uint8_t DINERO_TESTNET_P2PKH = 0x6F;  // 'm' or 'n' addresses (testnet)
    const uint8_t DINERO_TESTNET_P2SH = 0xC4;   // '2' addresses (testnet)
}

// Address metadata structure
struct AddressMetadata {
    std::string address;
    AddressType type;
    std::string network;
    bool is_dinero;
    std::string version_byte;
    std::string hex_hash160;
    std::string prefix;
    bool is_valid;
    std::string error_message;
    
    AddressMetadata() : is_dinero(false), is_valid(false) {}
};

// Comprehensive address record for persistence
struct AddressRecord {
    std::string address;                    // The actual address string
    std::string scriptPubKey;              // Hex-encoded script public key
    std::string path;                      // BIP84 derivation path (m/84'/1448'/0'/0/index or m/84'/1448'/1'/0/index)
    uint32_t index;                        // Address index in the sequence
    AddressType kind;                      // P2WPKH/P2TR/etc.
    bool is_change;                        // Whether this is a change address (branch 1)
    std::string label;                     // User-defined label
    std::chrono::system_clock::time_point created_at;  // Creation timestamp
    bool used;                             // Whether this address has been used in transactions
    std::string account;                   // Account name (default, etc.)
    uint32_t branch;                      // Branch (0 = receiving, 1 = change)
    
    AddressRecord() : index(0), kind(AddressType::BECH32M), is_change(false), used(false), branch(0) {}
    
    // JSON serialization
    Json::Value toJson() const;
    static AddressRecord fromJson(const Json::Value& json);
};

// Gap-limit counters per (account, branch)
struct GapLimitCounter {
    std::string account;
    uint32_t branch;                       // 0 = receiving, 1 = change
    uint32_t next_index;                   // Next index to generate
    uint32_t unused_count;                 // Count of consecutive unused addresses
    
    GapLimitCounter() : branch(0), next_index(0), unused_count(0) {}
    
    // JSON serialization
    Json::Value toJson() const;
    static GapLimitCounter fromJson(const Json::Value& json);
};

// Wallet encryption parameters
struct WalletEncryption {
    std::string algorithm;                 // "argon2id" or "scrypt"
    uint32_t iterations;                   // KDF iterations
    uint32_t memory_cost;                  // Memory cost (for Argon2)
    uint32_t parallelism;                  // Parallelism (for Argon2)
    uint32_t salt_length;                  // Salt length in bytes
    std::vector<uint8_t> salt;            // Random salt
    std::vector<uint8_t> nonce;           // Encryption nonce/IV
    std::vector<uint8_t> tag;             // Authentication tag
    
    WalletEncryption() : algorithm("argon2id"), iterations(100000), memory_cost(65536), 
                        parallelism(4), salt_length(32) {}
    
    // JSON serialization
    Json::Value toJson() const;
    static WalletEncryption fromJson(const Json::Value& json);
};

// Wallet metadata
struct WalletMetadata {
    std::string name;                      // Wallet name
    std::string description;               // User description
    std::string network;                   // mainnet/testnet/regtest
    std::string coin_type;                 // BIP44 coin type (1448 for Dinero)
    bool is_watch_only;                    // Whether this is a watch-only wallet
    bool is_hardware_wallet;               // Whether this is a hardware wallet
    std::string xpub;                      // Extended public key (for watch-only/hardware)
    std::string descriptor;                // Output descriptor
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_used;
    
    WalletMetadata()
        : coin_type(std::to_string(dinero::consensus::DINERO_COIN_TYPE)),
          is_watch_only(false),
          is_hardware_wallet(false) {}
    
    // JSON serialization
    Json::Value toJson() const;
    static WalletMetadata fromJson(const Json::Value& json);
};

class Address {
public:
    Address();
    ~Address();
    
    // Key generation
    static std::array<uint8_t, 32> generatePrivateKey();
    static std::vector<uint8_t> derivePublicKey(const std::array<uint8_t, 32>& privateKey, bool compressed = true);
    
    // Address creation
    static std::string createAddress(const std::vector<uint8_t>& publicKey, AddressType type = AddressType::P2PKH);
    static std::string createAddressFromPrivateKey(const std::array<uint8_t, 32>& privateKey, AddressType type = AddressType::P2PKH);
    
    // Address validation
    static bool validateAddress(const std::string& address);
    static bool validateAddress(const std::string& address, AddressType expectedType);
    
    // Comprehensive address decoding and validation
    struct DecodedAddress {
        std::string address;
        std::string network;        // "mainnet", "testnet", "regtest"
        std::string hrp;           // "din", "tdin", "rdin"
        std::string addressType;   // "legacy", "p2wpkh", "p2sh"
        std::string scriptPubKey;  // Hex-encoded script
        std::string pubKeyHash;    // Hash160 of public key
        bool isValid;
        std::string error;
    };
    
    static DecodedAddress decodeAddress(const std::string& address);
    
    // Network-specific validation
    static bool isValidAddressForNetwork(const std::string& address, const std::string& network);
    static std::string detectNetwork(const std::string& address);
    static std::string getNetworkHRP(const std::string& network);
    
    // Base58Check validation with detailed error reporting
    static bool validateBase58Check(const std::string& address, std::string& error);
    static std::string base58ToHex(const std::string& base58);
    
    // Bech32 validation with network HRP checking
    static bool validateBech32(const std::string& address, const std::string& expectedHrp, std::string& error);
    static std::string bech32ToHex(const std::string& bech32);
    
    // Bulletproof Base58Check functions (mirrors Bitcoin Core)
    static std::string encodeBase58Check(const std::vector<uint8_t>& payload);
    static bool decodeBase58Check(const std::string& encoded, std::vector<uint8_t>& payload);
    static std::vector<uint8_t> hash160(const std::vector<uint8_t>& data);
    static std::string publicKeyToAddress(const std::vector<uint8_t>& publicKey, AddressType type);
    // Bech32 P2WPKH (BIP84)
    static std::string createBech32P2WPKH(const std::vector<uint8_t>& publicKey, const std::string& hrp);
    
    // P2WPKH address and script creation (aliases for compatibility)
    static std::string createP2WPKHAddress(const std::vector<uint8_t>& publicKey, const std::string& hrp);
    static std::vector<uint8_t> createP2WPKHScript(const std::string& address);
    
    // Advanced features
    static AddressMetadata getAddressMetadata(const std::string& address);
    static std::string generateQRCode(const std::string& address, int size = 200);
    static std::string generateVanityAddress(const std::string& prefix, AddressType type = AddressType::DINERO_P2PKH, int maxAttempts = 10000);
    static std::vector<std::string> generateBatchAddresses(int count, AddressType type = AddressType::DINERO_P2PKH);
    
    // Transaction signing and verification
    static std::vector<uint8_t> signMessage(const std::vector<uint8_t>& message, const std::array<uint8_t, 32>& privateKey);
    static bool verifySignature(const std::vector<uint8_t>& message, const std::vector<uint8_t>& signature, const std::vector<uint8_t>& publicKey);
    static std::vector<uint8_t> signTransaction(const std::vector<uint8_t>& transactionHash, const std::array<uint8_t, 32>& privateKey);
    static bool verifyTransactionSignature(const std::vector<uint8_t>& transactionHash, const std::vector<uint8_t>& signature, const std::vector<uint8_t>& publicKey);
    
    // Utility functions
    static std::vector<uint8_t> publicKeyToHash(const std::vector<uint8_t>& publicKey);
    static std::string base58Encode(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> base58Decode(const std::string& encoded);
    static std::string bytesToHex(const std::vector<uint8_t>& bytes);
    static std::vector<uint8_t> computeChecksum(const std::vector<uint8_t>& data);
    static bool verifyChecksum(const std::vector<uint8_t>& data);
    
    // Getters
    std::string getAddress() const { return m_address; }
    std::array<uint8_t, 32> getPrivateKey() const { return m_privateKey; }
    std::vector<uint8_t> getPublicKey() const { return m_publicKey; }
    AddressType getType() const { return m_type; }
    
private:
    std::string m_address;
    std::array<uint8_t, 32> m_privateKey;
    std::vector<uint8_t> m_publicKey;
    AddressType m_type;
    
    // Internal helper methods
    static std::vector<uint8_t> ripemd160(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> doubleSha256(const std::vector<uint8_t>& data);
    static std::string bech32Encode(const std::vector<uint8_t>& data, const std::string& hrp);
    static std::vector<uint8_t> bech32Decode(const std::string& address, std::string& hrp);
};

// Wallet class for managing multiple addresses
class Wallet {
public:
    Wallet();
    ~Wallet();
    
    // Initialization
    bool initialize(const std::string& walletPath = "./wallet");
    void shutdown();
    
    // Wallet creation and management
    bool createWallet(const std::string& name, const std::string& description = "", 
                     const std::string& network = "mainnet");
    bool loadWallet(const std::string& name);
    bool unloadWallet();
    bool deleteWallet(const std::string& name);
    std::vector<std::string> listWallets() const;
    
    // BIP39 seed and passphrase management
    bool createFromSeed(const std::vector<uint8_t>& seed, const std::string& passphrase = "");
    bool createFromMnemonic(const std::string& mnemonic, const std::string& passphrase = "");
    bool changePassphrase(const std::string& oldPassphrase, const std::string& newPassphrase);
    bool isEncrypted() const;
    bool unlock(const std::string& password, uint32_t timeout_seconds = 300);
    bool lock();
    bool isUnlocked() const;
    
    // Address management with gap-limit enforcement
    std::string generateNewAddress(const std::string& account = "default",
                                  AddressType type = AddressType::BECH32M);
    // Overload to preserve older call sites that pass only type
    std::string generateNewAddress(AddressType type);
    std::string generateNewChangeAddress(const std::string& account = "default",
                                       AddressType type = AddressType::BECH32M);
    // Convenience overload with default account
    std::vector<std::string> getAddresses() const;
    std::vector<std::string> getAddresses(const std::string& account = "default") const;
    std::vector<AddressRecord> getAddressRecords(const std::string& account = "default") const;
    bool importAddress(const std::string& address, const std::string& label = "");
    bool removeAddress(const std::string& address);
    bool labelAddress(const std::string& address, const std::string& label);
    
    // Watch-only and hardware wallet support
    bool createWatchOnlyWallet(const std::string& xpub, const std::string& descriptor = "");
    bool importDescriptor(const std::string& descriptor);
    bool isWatchOnly() const;
    bool isHardwareWallet() const;
    
    // Key management
    bool importPrivateKey(const std::array<uint8_t, 32>& privateKey, const std::string& label = "");
    std::array<uint8_t, 32> getPrivateKey(const std::string& address) const;
    bool exportPrivateKey(const std::string& address, std::array<uint8_t, 32>& privateKey);
    
    // Balance and UTXO management
    uint64_t getBalance(const std::string& address) const;
    uint64_t getTotalBalance() const;
    uint64_t getAccountBalance(const std::string& account) const;
    std::vector<std::string> getUTXOs(const std::string& address) const;
    std::vector<WalletUTXO> getUTXOsForAddress(const std::string& address) const;
    
    // Transaction signing
    bool signTransaction(const std::string& txHex, const std::string& address, std::string& signedTx);
    
    // Gap-limit management
    uint32_t getGapLimit() const { return m_gap_limit; }
    void setGapLimit(uint32_t limit) { m_gap_limit = limit; }
    bool checkGapLimit(const std::string& account, uint32_t branch);
    void updateGapLimitCounters(const std::string& account, uint32_t branch, uint32_t index);
    
    // Persistence
    bool saveWallet();
    bool loadWallet();
    bool backupWallet(const std::string& backupPath);
    bool restoreWallet(const std::string& backupPath);
    
    // Getters
    std::string getWalletPath() const { return m_walletPath; }
    std::string getWalletName() const { return m_metadata.name; }
    std::string getNetwork() const { return m_metadata.network; }
    bool isInitialized() const { return m_initialized; }

private:
    std::string m_walletPath;
    std::string m_walletName;
    WalletMetadata m_metadata;
    WalletEncryption m_encryption;
    std::vector<AddressRecord> m_addresses;
    std::map<std::string, std::map<uint32_t, GapLimitCounter>> m_gap_counters; // account -> branch -> counter
    std::vector<uint8_t> m_encrypted_seed;
    std::vector<uint8_t> m_decrypted_seed;
    bool m_initialized;
    bool m_encrypted;
    bool m_unlocked;
    uint32_t m_gap_limit;
    std::chrono::system_clock::time_point m_unlock_time;
    
    // Internal methods
    bool generateAddressesUpToGapLimit(const std::string& account, uint32_t branch, uint32_t target_index);
    bool persistAddressRecord(const AddressRecord& record);
    bool loadAddressRecords();
    bool saveAddressRecords();
    bool persistGapLimitCounters();
    bool loadGapLimitCounters();
    bool encryptSeed(const std::vector<uint8_t>& seed, const std::string& password);
    bool decryptSeed(const std::string& password);
    bool validatePassphrase(const std::string& passphrase);
    std::string deriveBIP84Path(uint32_t account, uint32_t branch, uint32_t index) const;
    void secureZero(std::vector<uint8_t>& data);
    
    // BIP39 and encryption helper methods
    bool applyBIP39Passphrase(const std::string& passphrase);
    std::vector<uint8_t> deriveSeedFromPassphrase(const std::string& passphrase);
    bool initializeFromSeed();
    bool initializeWatchOnly();
    bool generateInitialAddresses();
    bool importAddressesFromXpub();
    bool parseAndImportDescriptor(const std::string& descriptor);
    std::array<uint8_t, 32> derivePrivateKeyFromSeed(const std::string& path) const;
    std::vector<uint8_t> mnemonicToSeed(const std::string& mnemonic, const std::string& passphrase);
    
    // Legacy methods (to be replaced)
    bool storeAddress(const Address& address);
    bool loadAddress(const std::string& addressStr);
};

} // namespace dinero 
