#include "daemon/bech32_decode.h"
#include "external/bech32/bech32.hpp"
#include "address/addr_codec.h"
#include "common/logger.h"
#include "consensus/chainparams.h" // For HrpForActiveNetworkRef
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>
#include <iostream> // For std::cout, std::endl

namespace dinero::mining {

// bech32 helper functions removed — using bech32::Decode from bech32.hpp

bool Bech32DecodeSegwit(
    const std::string& addr,
    const std::string& expected_hrp,
    int& witver,
    std::vector<uint8_t>& witprog
) {
    // Use bech32::Decode directly
    auto result = bech32::Decode(expected_hrp, addr);
    if (!result) return false;

    // **Checksum variant must match witness version** (BIP-350)
    if ((result->witver == 0 && result->encoding != bech32::Encoding::BECH32) ||
        (result->witver >  0 && result->encoding != bech32::Encoding::BECH32M)) return false;

    witver = result->witver;
    witprog = result->program;
    return true;
}

std::string GetBech32HRP() {
    // Use the active network HRP from chainparams
    return dinero::HrpForActiveNetworkRef();
}

} // namespace dinero::mining
