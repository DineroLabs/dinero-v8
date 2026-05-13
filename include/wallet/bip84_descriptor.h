#pragma once
#include "wallet/wallet_iface.h"
#include "consensus/coin_type.h"
#include <string>
#include <vector>

namespace din {

/**
 * @brief BIP84 descriptor factory for P2WPKH wallets
 * 
 * Creates standard BIP84 descriptors:
 * - Receive: wpkh([fingerprint/84h/1448h/0h]xpub/0/\*)
 * - Change:  wpkh([fingerprint/84h/1448h/0h]xpub/1/\*)
 */
class BIP84DescriptorFactory {
public:
    struct BIP84Config {
        std::string master_fingerprint;  // 8 hex chars
        std::string account_xpub;        // Account-level xpub (m/84'/coin_type'/0')
        uint32_t coin_type = dinero::consensus::DINERO_COIN_TYPE;
        uint32_t account = 0;            // Account index
        uint32_t gap_limit = 20;         // Address gap limit
    };
    
    /**
     * @brief Create BIP84 receive descriptor
     * 
     * Format: wpkh([fingerprint/84h/coin_type'/account']xpub/0/\*)
     */
    static std::string createReceiveDescriptor(const BIP84Config& config);
    
    /**
     * @brief Create BIP84 change descriptor
     * 
     * Format: wpkh([fingerprint/84h/coin_type'/account']xpub/1/\*)
     */
    static std::string createChangeDescriptor(const BIP84Config& config);
    
    /**
     * @brief Parse BIP84 descriptor and extract components
     */
    struct ParsedBIP84 {
        bool valid = false;
        std::string fingerprint;
        std::string xpub;
        std::vector<uint32_t> derivation_path;
        bool is_change = false;
        std::string error;
    };
    
    static ParsedBIP84 parseDescriptor(const std::string& descriptor);
    
    /**
     * @brief Validate BIP84 descriptor format
     */
    static bool validateBIP84Descriptor(const std::string& descriptor, std::string& error);
    
    /**
     * @brief Create default BIP84 wallet configuration
     * 
     * Generates standard receive and change descriptors for a new wallet.
     */
    static std::pair<std::string, std::string> createDefaultDescriptors(
        const std::string& master_fingerprint, 
        const std::string& account_xpub,
        uint32_t coin_type = dinero::consensus::DINERO_COIN_TYPE);

private:
    // Helper methods for descriptor parsing
    static bool parseKeyOrigin(const std::string& origin_str, 
                              std::string& fingerprint, 
                              std::vector<uint32_t>& path);
    static std::string formatKeyOrigin(const std::string& fingerprint, 
                                      const std::vector<uint32_t>& path);
    static std::string hardened(uint32_t index) { return std::to_string(index) + "h"; }
};

} // namespace din
