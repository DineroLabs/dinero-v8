#include "rpc/address_validation.h"
#include "../../external/bech32/bech32.hpp"

#include <string>
#include <vector>

namespace dinero {

// Validate P2WPKH bech32 address against expected HRP
bool ValidateBech32Address(const std::string& address, const std::string& expectedHrp) {
    auto dec = bech32::Decode(expectedHrp, address);
    if (!dec.has_value()) return false;
    const auto& r = dec.value();
    if (r.witver != 0) return false;
    if (r.program.size() != 20) return false; // P2WPKH
    return true;
}

// Build scriptPubKey for P2WPKH address: 0x00 0x14 <20-byte hash>
bool BuildP2WPKHScriptPubKey(const std::string& address, std::vector<uint8_t>& scriptPubKey, std::string& error) {
    // Try decode without HRP (extract hrp & encoding), then verify
    auto [hrp, data, enc] = bech32::DecodeWithEncoding(address);
    if (hrp.empty()) { error = "Invalid bech32 address"; return false; }
    if (enc != bech32::Encoding::BECH32) { error = "Invalid encoding for v0"; return false; }
    // data[0] is version when using some encoders; our helper returns program only, so use DecodeWithEncoding semantics:
    // DecodeWithEncoding returns hrp and 5-bit data; convertbits is internal; prefer dedicated Decode(hrp,addr)
    auto dec = bech32::Decode(hrp, address);
    if (!dec.has_value()) { error = "Decode failed"; return false; }
    const auto& r = dec.value();
    if (r.witver != 0 || r.program.size() != 20) { error = "Not P2WPKH"; return false; }

    scriptPubKey.clear();
    scriptPubKey.reserve(22);
    scriptPubKey.push_back(0x00); // OP_0
    scriptPubKey.push_back(0x14); // push 20 bytes
    scriptPubKey.insert(scriptPubKey.end(), r.program.begin(), r.program.end());
    return true;
}

// Implementation for ChainParamsImpl declared in header (kept minimal here)
std::string ChainParamsImpl::HRP() const {
    return "rdin";  // Default to regtest in this module; callers should pass explicit HRP where needed
}

ChainParamsImpl& GetChainParams() {
    static ChainParamsImpl params;
    return params;
}

bool ToWitnessScript(const std::string& address, std::vector<uint8_t>& witnessScript, 
                    const ChainParamsImpl& params, std::string& error) {
    // Validate with expected HRP from params
    if (!ValidateBech32Address(address, params.HRP())) {
        error = "Invalid address for network HRP";
        return false;
    }
    return BuildP2WPKHScriptPubKey(address, witnessScript, error);
}

std::string EncodeBech32P2WPKH(const std::vector<uint8_t>& pubkeyHash, const std::string& hrp) {
    if (pubkeyHash.size() != 20) return {};
    return bech32::Encode(hrp, /*witver=*/0, pubkeyHash, bech32::Encoding::BECH32);
}

} // namespace dinero