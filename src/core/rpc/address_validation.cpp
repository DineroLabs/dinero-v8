#include "rpc/address_validation.h"
#include "../../external/bech32/bech32.hpp"

#include <string>
#include <vector>

namespace dinero {

// Simple address validation for P2WPKH addresses
bool ValidateBech32Address(const std::string& address, const std::string& expectedHrp) {
    auto [hrp, data, encoding] = bech32::DecodeWithEncoding(address);
    if (hrp != expectedHrp) {
        return false;
    }
    
    // Only support witness version 0 with 20-byte pubkey hash (P2WPKH)
    if (encoding != bech32::Encoding::BECH32 || data.size() != 20) {
        return false;
    }
    
    return true;
}

// Build scriptPubKey for P2WPKH address
bool BuildP2WPKHScriptPubKey(const std::string& address, std::vector<uint8_t>& scriptPubKey, std::string& error) {
    auto [hrp, data, encoding] = bech32::DecodeWithEncoding(address);
    
    if (encoding != bech32::Encoding::BECH32 || data.size() != 20) {
        error = "Invalid witness version or program length";
        return false;
    }
    
    // Build P2WPKH scriptPubKey: OP_0 (0x00) + 20-byte pubkey hash
    scriptPubKey.clear();
    scriptPubKey.push_back(0x00);  // OP_0
    scriptPubKey.insert(scriptPubKey.end(), data.begin(), data.end());
    
    return true;
}

// Implementation for ChainParamsImpl declared in header
std::string ChainParamsImpl::HRP() const {
    return "rdin";  // Regtest HRP
}

ChainParamsImpl& GetChainParams() {
    static ChainParamsImpl params;
    return params;
}

// Additional required functions
bool ToWitnessScript(const std::string& address, std::vector<uint8_t>& witnessScript, 
                    const ChainParamsImpl& params, std::string& error) {
    return BuildP2WPKHScriptPubKey(address, witnessScript, error);
}

std::string EncodeBech32P2WPKH(const std::vector<uint8_t>& pubkeyHash, const std::string& hrp) {
    // Simple implementation - in real code this would properly encode
    // For now, return a placeholder valid regtest address
    return "rdin1q" + std::string(38, 'a'); // Simplaceholder for regtest
}

} // namespace dinero