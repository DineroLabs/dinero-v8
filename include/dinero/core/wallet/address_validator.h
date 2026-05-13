#pragma once
#include <string>
#include <optional>
#include <cstdint>
#include <vector>
#include "wallet/taproot_address.h"

namespace din {

/**
 * @brief Address validation and parsing for Dinero
 * 
 * Provides comprehensive address validation with network checks,
 * script type detection, and proper error reporting.
 * 
 * Supported formats:
 * - Bech32 (P2WPKH): din1q... (witness v0, 20-byte program)  
 * - Bech32m (P2TR): din1p... (witness v1, 32-byte program)
 * - Legacy P2PKH: D... (Base58Check, 21-byte payload)
 * - Legacy P2SH: 3... (Base58Check, 21-byte payload)
 */
class AddressValidator {
public:
    /**
     * @brief Address type classification
     */
    enum class AddressType {
        P2WPKH,     // Pay-to-Witness-PubkeyHash (Bech32)
        P2WSH,      // Pay-to-Witness-Script-Hash (Bech32)  
        P2TR,       // Pay-to-Taproot (Bech32m)
        P2PKH,      // Pay-to-PubkeyHash (Legacy Base58)
        P2SH,       // Pay-to-Script-Hash (Legacy Base58)
        Unknown     // Invalid or unsupported format
    };
    
    /**
     * @brief Network type for address validation
     */
    enum class Network {
        Mainnet,    // din1..., D..., 3...
        Testnet,    // tdin1..., t..., 2...
        Regtest     // rdin1..., r..., 2...
    };
    
    /**
     * @brief Address validation result
     */
    struct ValidationResult {
        bool valid = false;
        AddressType type = AddressType::Unknown;
        Network network = Network::Mainnet;
        std::string error_message;
        std::vector<uint8_t> program;  // Decoded address program/hash
        uint8_t witness_version = 0;   // For segwit addresses
        
        // Helper methods
        bool isSegwit() const { 
            return type == AddressType::P2WPKH || type == AddressType::P2WSH || type == AddressType::P2TR; 
        }
        bool isLegacy() const { 
            return type == AddressType::P2PKH || type == AddressType::P2SH; 
        }
        size_t programSize() const { return program.size(); }
    };

public:
    /**
     * @brief Validate address string and return detailed result
     * 
     * @param address Address string to validate
     * @param expected_network Expected network (default: mainnet)
     * @return Detailed validation result with type, network, and program
     */
    static ValidationResult validate(
        const std::string& address, 
        Network expected_network = Network::Mainnet
    );
    
    /**
     * @brief Quick address validation (boolean result)
     * 
     * @param address Address string to validate
     * @param expected_network Expected network (default: mainnet)
     * @return True if address is valid for the expected network
     */
    static bool isValid(
        const std::string& address,
        Network expected_network = Network::Mainnet
    );
    
    /**
     * @brief Get address type without full validation
     * 
     * @param address Address string to classify
     * @return Address type (may be Unknown for invalid addresses)
     */
    static AddressType getType(const std::string& address);
    
    /**
     * @brief Convert address type to string representation
     * 
     * @param type Address type to convert
     * @return Human-readable type name
     */
    static std::string typeToString(AddressType type);
    
    /**
     * @brief Convert network to string representation
     * 
     * @param network Network to convert
     * @return Human-readable network name
     */
    static std::string networkToString(Network network);
    
    /**
     * @brief Get expected HRP (Human Readable Part) for network
     * 
     * @param network Target network
     * @return HRP string for Bech32/Bech32m addresses
     */
    static std::string getHRP(Network network);

private:
    // Bech32/Bech32m validation helpers
    static ValidationResult validateBech32(const std::string& address, Network expected_network);
    static ValidationResult validateBech32m(const std::string& address, Network expected_network);
    
    // Legacy Base58Check validation helpers  
    static ValidationResult validateBase58(const std::string& address, Network expected_network);
    
    // Low-level parsing helpers
    static std::optional<std::pair<std::string, std::vector<uint8_t>>> decodeBech32(const std::string& address);
    static std::optional<std::pair<std::string, std::vector<uint8_t>>> decodeBech32m(const std::string& address);
    static std::optional<std::vector<uint8_t>> decodeBase58Check(const std::string& address);
    
    // Network prefix validation
    static bool isValidNetworkPrefix(const std::string& hrp, Network network);
    static bool isValidBase58Prefix(uint8_t version_byte, Network network, AddressType& detected_type);
    
    // Program size validation
    static bool isValidProgramSize(AddressType type, size_t program_size);
};

} // namespace din
