#include "common/address_script_builder.h"
#include "daemon/bech32_encoder.h"
#include <sstream>
#include <iomanip>

namespace dinero {

bool BuildScriptPubKeyFromAddress(const std::string& addr, std::vector<uint8_t>& spk, std::string& error) {
    // Decode the bech32 address
    Bech32Encoder encoder;
    auto result = encoder.decode_segwit_address(addr);

    if (!result.valid) {
        error = "Failed to decode address: " + result.error;
        return false;
    }

    // Phase W.1.1: Accept both mainnet and regtest HRPs
    // din = mainnet, rdin = regtest, tdin = testnet
    if (result.hrp != "din" && result.hrp != "rdin" && result.hrp != "tdin") {
        error = "Invalid HRP '" + result.hrp + "', expected 'din' (mainnet), 'rdin' (regtest), or 'tdin' (testnet)";
        return false;
    }

    // Build scriptPubKey: OP_<version> <program_length> <program>
    spk.clear();

    // Convert witness version to opcode:
    // - v0 = OP_0 (0x00)
    // - v1 = OP_1 (0x51)
    // - v2 = OP_2 (0x52), etc.
    uint8_t witness_opcode = (result.witness_version == 0) ? 0x00 : (0x50 + result.witness_version);
    spk.push_back(witness_opcode);
    spk.push_back(static_cast<uint8_t>(result.witness_program.size()));  // Program length
    spk.insert(spk.end(), result.witness_program.begin(), result.witness_program.end());

    return true;
}

std::string ScriptPubKeyToHex(const std::vector<uint8_t>& spk) {
    std::ostringstream oss;
    for (auto byte : spk) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

} // namespace dinero