#include "../common/address_script_builder.h"
#include <string>
#include <vector>

// Forward declare functions from our clean helper
using dinero::addr::DecodeBech32P2WPKH;
using dinero::addr::MakeP2WPKHScript;

// Legacy compatibility functions using our clean helper
namespace dinero {

// Convert scriptPubKey vector to uppercase hex string
std::string ScriptPubKeyToHex(const std::vector<uint8_t>& scriptPubKey) {
    std::string hex;
    hex.reserve(scriptPubKey.size() * 2);
    
    for (uint8_t byte : scriptPubKey) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", byte);
        hex += buf;
    }
    
    return hex;
}

// Build scriptPubKey from address using our clean helper
bool BuildScriptPubKeyFromAddress(const std::string& address, 
                                 std::vector<uint8_t>& out_scriptPubKey, 
                                 std::string& error) {
    
    // Determine HRP based on address prefix (simple heuristic)
    std::string hrp;
    if (address.substr(0, 4) == "rdin") {
        hrp = "rdin";
    } else if (address.substr(0, 4) == "din1") {
        hrp = "din1";
    } else {
        error = "unsupported address prefix";
        return false;
    }
    
    // Decode using our clean helper
    std::array<uint8_t, 20> prog{};
    if (!DecodeBech32P2WPKH(address, hrp, prog, error)) {
        return false;
    }
    
    // Build scriptPubKey
    auto spk = MakeP2WPKHScript(prog);
    out_scriptPubKey = std::move(spk);
    
    return true;
}

} // namespace dinero
