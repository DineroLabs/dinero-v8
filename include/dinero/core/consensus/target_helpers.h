#pragma once
#include "consensus/pow_compact.h"
#include <cstdint>
#include <string>
#include <iomanip>
#include <sstream>

namespace dinero {

// Convert compact difficulty bits to target array
inline std::array<uint8_t,32> TargetFromBits(uint32_t nBits) {
    return TargetFromBitsBE(nBits);
}

// Convert target array to compact difficulty bits (for testing)
inline uint32_t BitsFromTarget(const std::array<uint8_t,32>& target) {
    return BitsFromTargetBE(target);
}

// Convert target to hex string (64 chars, zero-padded)
inline std::string TargetToHex(const std::array<uint8_t,32>& target) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto& byte : target) {
        ss << std::setw(2) << static_cast<unsigned>(byte);
    }
    return ss.str(); // Always exactly 64 chars (32 bytes * 2 hex chars each)
}

} // namespace dinero
