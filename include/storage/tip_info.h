#pragma once
#include <string>
#include <cstdint>
#include "consensus/chainwork.h"
#include "primitives/uint256.h"

namespace dinero {

struct TipInfo {
    uint256 hash;
    int height = 0;
    arith_uint256 work = arith_uint256(0);  // Initialize with proper constructor
    uint32_t timestamp = 0;
};

} // namespace dinero
