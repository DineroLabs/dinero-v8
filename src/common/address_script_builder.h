#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::addr {

// Strict vNext rules: lower-case Bech32, HRP must match active network,
// witver=0 (bech32, not bech32m), witness program = 20 bytes (P2WPKH).
bool DecodeBech32P2WPKH(const std::string& addr,
                        const std::string& expected_hrp,
                        std::array<uint8_t,20>& out_prog,
                        std::string& err);

// scriptPubKey = 0x00 0x14 <20-byte witness program>
inline std::vector<uint8_t> MakeP2WPKHScript(const std::array<uint8_t,20>& prog) {
    std::vector<uint8_t> spk;
    spk.reserve(22);
    spk.push_back(0x00);
    spk.push_back(0x14);
    spk.insert(spk.end(), prog.begin(), prog.end());
    return spk;
}

} // namespace dinero::addr