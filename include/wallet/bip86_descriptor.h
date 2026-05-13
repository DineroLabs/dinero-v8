#pragma once
#include "wallet/wallet_iface.h"
#include "consensus/coin_type.h"
#include <string>
#include <vector>

namespace din {

/**
 * @brief BIP86 descriptor factory for Taproot P2TR key-path wallets
 *
 * Creates standard BIP86 Taproot descriptors (key-path spending only):
 * - Receive: tr([fingerprint/86h/1448h/0h]xpub/0/\*)
 * - Change:  tr([fingerprint/86h/1448h/0h]xpub/1/\*)
 *
 * IMPORTANT: These descriptors are for key-path-only Taproot.
 * Script-path spending is NOT enabled by default for security.
 */
class BIP86DescriptorFactory {
public:
    struct BIP86Config {
        std::string master_fingerprint;  // 8 hex chars
        std::string account_xpub;        // Account-level xpub (m/86'/coin_type'/0')
        uint32_t coin_type = dinero::consensus::DINERO_COIN_TYPE;
        uint32_t account = 0;            // Account index
        uint32_t gap_limit = 20;         // Address gap limit
    };

    /**
     * @brief Create BIP86 Taproot receive descriptor
     *
     * Format: tr([fingerprint/86h/coin_type'/account']xpub/0/\*)
     *
     * This creates a key-path-only Taproot descriptor.
     * Addresses derived from this descriptor can only be spent
     * via key-path (single signature), not script-path.
     */
    static std::string createReceiveDescriptor(const BIP86Config& config);

    /**
     * @brief Create BIP86 Taproot change descriptor
     *
     * Format: tr([fingerprint/86h/coin_type'/account']xpub/1/\*)
     */
    static std::string createChangeDescriptor(const BIP86Config& config);

    /**
     * @brief Parse BIP86 descriptor and extract components
     */
    struct ParsedBIP86 {
        bool valid = false;
        std::string fingerprint;
        std::string xpub;
        std::vector<uint32_t> derivation_path;
        bool is_change = false;
        std::string error;
    };

    static ParsedBIP86 parseDescriptor(const std::string& descriptor);

    /**
     * @brief Validate BIP86 descriptor format
     */
    static bool validateBIP86Descriptor(const std::string& descriptor, std::string& error);

    /**
     * @brief Create default BIP86 wallet configuration
     *
     * Generates standard Taproot receive and change descriptors for a new wallet.
     * These are key-path-only descriptors for maximum simplicity and privacy.
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
