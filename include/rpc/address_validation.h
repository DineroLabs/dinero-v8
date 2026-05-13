#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

// Forward declarations
class ChainParamsImpl {
public:
    std::string HRP() const;
};

// Global chain params accessor
ChainParamsImpl& GetChainParams();

// Address validation functions
bool ValidateBech32Address(const std::string& address, const std::string& expectedHrp);
bool BuildP2WPKHScriptPubKey(const std::string& address, std::vector<uint8_t>& scriptPubKey, std::string& error);

// Additional required functions
bool ToWitnessScript(const std::string& address, std::vector<uint8_t>& witnessScript, 
                    const ChainParamsImpl& params, std::string& error);
std::string EncodeBech32P2WPKH(const std::vector<uint8_t>& pubkeyHash, const std::string& hrp);

} // namespace dinero
