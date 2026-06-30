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

// Witness address validation result (covers SegWit v0 P2WPKH/P2WSH and v1 Taproot).
struct WitnessAddressInfo {
    bool is_valid = false;
    bool is_witness = false;
    int witness_version = -1;
    std::vector<uint8_t> witness_program;   // raw program bytes (20/32 for v0, 32 for taproot)
    std::vector<uint8_t> script_pubkey;     // canonical scriptPubKey (e.g. 0x5120<program> for taproot)
};

// Decode a bech32/bech32m witness address for the given HRP and, if valid,
// produce its witness version, program, and scriptPubKey. Enforces BIP350:
// v0 must be bech32 (20- or 32-byte program); v1..v16 must be bech32m.
// Returns is_valid=false (rather than throwing) for any malformed/foreign input.
WitnessAddressInfo DecodeWitnessAddress(const std::string& s, const std::string& hrp);

// Auto-decode function
ParsedAddress DecodeAddressAuto(const std::string& s);

// Destination validation
bool IsValidDestination(const Destination& d);

} // namespace dinero

#endif // DINERO_ADDRESS_ADDR_CODEC_H
