#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace din {

/**
 * @brief Taproot address utilities (BIP-341)
 * 
 * Implements P2TR (Pay-to-Taproot) address generation and validation
 * using Bech32m encoding with 32-byte pubkey hashes.
 */
class TaprootAddress {
public:
    /**
     * @brief Generate P2TR address from public key
     * 
     * @param pubkey 32-byte public key (x-only)
     * @param network Network type (mainnet/testnet/regtest)
     * @return Bech32m encoded P2TR address
     */
    static std::string fromPubkey(const std::vector<uint8_t>& pubkey, 
                                  const std::string& network = "mainnet");
    
    /**
     * @brief Validate P2TR address
     * 
     * @param address Bech32m encoded address
     * @param network Expected network
     * @return true if valid P2TR address
     */
    static bool isValid(const std::string& address, 
                       const std::string& network = "mainnet");
    
    /**
     * @brief Extract public key from P2TR address
     * 
     * @param address Bech32m encoded address
     * @return 32-byte public key, or nullopt if invalid
     */
    static std::optional<std::vector<uint8_t>> extractPubkey(const std::string& address);
    
    /**
     * @brief Create Taproot output script
     * 
     * @param pubkey 32-byte public key
     * @return P2TR scriptPubKey (OP_1 + 32-byte pubkey)
     */
    static std::vector<uint8_t> createScriptPubkey(const std::vector<uint8_t>& pubkey);
    
    /**
     * @brief Validate public key for Taproot
     * 
     * @param pubkey Public key to validate
     * @return true if valid for Taproot (32 bytes, on curve)
     */
    static bool isValidPubkey(const std::vector<uint8_t>& pubkey);

private:
    /**
     * @brief Get HRP for network
     */
    static std::string getHrp(const std::string& network);
    
    /**
     * @brief Bech32m encode (8-bit data)
     */
    static std::string bech32mEncode(const std::string& hrp,
                                    const std::vector<uint8_t>& data);

    /**
     * @brief Bech32m encode for SegWit (5-bit data)
     */
    static std::string bech32mEncodeSegWit(const std::string& hrp,
                                          const std::vector<uint8_t>& data);

    /**
     * @brief Bech32m decode
     */
    static std::optional<std::pair<std::string, std::vector<uint8_t>>>
    bech32mDecode(const std::string& address);
    
    /**
     * @brief Convert 8-bit data to 5-bit for Bech32m
     */
    static std::vector<uint8_t> convertBits(const std::vector<uint8_t>& data, 
                                           int frombits, int tobits, bool pad);
};

/**
 * @brief Taproot key utilities
 * 
 * Helper functions for Taproot key operations including
 * internal key generation and tweaking.
 */
class TaprootKeys {
public:
    /**
     * @brief Generate internal key for Taproot
     * 
     * @param seed 32-byte seed
     * @return 32-byte internal public key
     */
    static std::vector<uint8_t> generateInternalKey(const std::vector<uint8_t>& seed);
    
    /**
     * @brief Compute Taproot output key
     * 
     * @param internal_pubkey 32-byte internal public key
     * @param script_root 32-byte Merkle root of script tree (optional)
     * @return 32-byte Taproot output public key
     */
    static std::vector<uint8_t> computeOutputKey(
        const std::vector<uint8_t>& internal_pubkey,
        const std::optional<std::vector<uint8_t>>& script_root = std::nullopt);
    
    /**
     * @brief Validate Taproot key pair
     * 
     * @param pubkey 32-byte public key
     * @param privkey 32-byte private key
     * @return true if valid key pair
     */
    static bool isValidKeyPair(const std::vector<uint8_t>& pubkey,
                              const std::vector<uint8_t>& privkey);
};

} // namespace din
