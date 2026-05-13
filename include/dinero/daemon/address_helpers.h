#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {

// Forward declaration for chain parameters
class ChainParamsImpl {
public:
    std::string HRP() const;
};

/**
 * Convert Bech32 address to witness script
 * @param addr Bech32 address string
 * @param out_script Output witness script bytes
 * @param params Chain parameters for HRP validation
 * @param why Output error reason if validation fails
 * @return true if valid address and script generated
 */
bool ToWitnessScript(const std::string& addr,
                     std::vector<uint8_t>& out_script,
                     const ChainParamsImpl& params,
                     std::string& why);

/**
 * Encode pubkey hash to Bech32 P2WPKH address
 * @param pubkey_hash 20-byte pubkey hash
 * @param hrp Human readable part (network prefix)
 * @return Bech32 address string
 */
std::string EncodeBech32P2WPKH(const std::vector<uint8_t>& pubkey_hash,
                               const std::string& hrp);

// Get current chain parameters
const ChainParamsImpl& GetChainParams();

} // namespace dinero
