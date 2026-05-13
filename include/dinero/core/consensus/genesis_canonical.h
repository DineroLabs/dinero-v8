#pragma once
#include <array>
#include <string>
#include "consensus/chainparams.h"

namespace dinero {

struct CanonicalGenesis {
    std::array<unsigned char,80> headerLE; // exact 80 bytes the daemon hashes
    std::string hashBE;                    // human hex (big-endian)
    std::string merkleBE;                  // human hex (big-endian)
};

// Builds the genesis header via the SAME logic the daemon uses (coinbase builder,
// merkle computation, header serialization). No manual hex parsing here.
CanonicalGenesis BuildCanonicalGenesis(const ChainParams& P);

} // namespace dinero
