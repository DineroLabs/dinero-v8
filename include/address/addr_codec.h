#ifndef DINERO_ADDRESS_ADDR_CODEC_H
#define DINERO_ADDRESS_ADDR_CODEC_H

#include <string>
#include <vector>
#include "address/addr_types.h"

namespace dinero {

// Global HRP configuration - will be set by SelectParams()
extern std::string g_active_bech32_hrp;

// HRP for active network (returns "din", "tdin", or "rdin")
const std::string& HrpForActiveNetworkRef();

// Address decoding functions
Destination DecodeBase58Address(const std::string& s);
Destination DecodeBech32Address(const std::string& s, const std::string& hrp);
Destination DecodeTaprootAddress(const std::string& s, const std::string& hrp);

// Address encoding functions
std::string EncodeBase58Address(const Destination& d);
std::string EncodeBech32Address(const Destination& d, const std::string& hrp);

// Taproot-specific functions
std::vector<uint8_t> DecodeTaprootWitnessProgram(const std::string& address);
std::vector<uint8_t> CreateP2TRScriptPubKey(const std::vector<uint8_t>& witness_program);

// Auto-decode function
ParsedAddress DecodeAddressAuto(const std::string& s);

// Destination validation
bool IsValidDestination(const Destination& d);

} // namespace dinero

#endif // DINERO_ADDRESS_ADDR_CODEC_H
