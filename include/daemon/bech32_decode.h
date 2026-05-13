#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero::mining {

/**
 * Decode a Bech32 SegWit address and extract witness version and program
 * 
 * @param addr The Bech32 address (e.g., "din1qxy2kgdygjrsqtzq2n0yrf2493p83kkfjhx0wlh")
 * @param expected_hrp Expected human-readable part ("din", "tdin", "rdin")
 * @param witver Output: witness version (0-16)
 * @param witprog Output: witness program (2-40 bytes)
 * @return true on success, false on validation failure
 */
bool Bech32DecodeSegwit(
    const std::string& addr,
    const std::string& expected_hrp,
    int& witver,
    std::vector<uint8_t>& witprog
);

/**
 * Get the appropriate HRP for the current network
 * @return "din" for mainnet, "tdin" for testnet, "rdin" for regtest
 */
std::string GetBech32HRP();

} // namespace dinero::mining
