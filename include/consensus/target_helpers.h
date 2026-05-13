#pragma once
#include "consensus/pow_compact.h"
#include <cstdint>
#include <string>
#include <iomanip>
#include <sstream>
#include <limits>
#include <cmath>

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

// Convert 32-byte big-endian target to double (for difficulty calculation)
// This converts the target to a floating point approximation
inline double TargetToDouble(const std::array<uint8_t,32>& target) {
    // Find first non-zero byte
    size_t i = 0;
    while (i < 32 && target[i] == 0) ++i;

    if (i == 32) return std::numeric_limits<double>::infinity(); // All zeros = infinite difficulty

    // Extract leading significant bytes for precision
    // Use the first 8 non-zero bytes to create a double
    double value = 0.0;
    size_t count = 0;
    for (; i < 32 && count < 8; ++i, ++count) {
        value = value * 256.0 + static_cast<double>(target[i]);
    }

    // Account for remaining bytes as powers of 256
    size_t remaining_bytes = (32 - i);
    if (remaining_bytes > 0) {
        value *= std::pow(256.0, static_cast<double>(remaining_bytes));
    }

    return value;
}

// Calculate difficulty from nBits and powLimitBits
// Formula: difficulty = powLimit_target / current_target
// Where difficulty = 1.0 at powLimit (easiest allowed difficulty)
inline double DifficultyFromBits(uint32_t nBits, uint32_t powLimitBits) {
    // Get targets from compact representation
    auto current_target = TargetFromBitsBE(nBits);
    auto pow_limit_target = TargetFromBitsBE(powLimitBits);

    // Convert to doubles
    double current = TargetToDouble(current_target);
    double limit = TargetToDouble(pow_limit_target);

    // Avoid division by zero
    if (current <= 0.0) return std::numeric_limits<double>::infinity();

    // difficulty = powLimit / target
    // Higher target = easier mining = lower difficulty
    // Lower target = harder mining = higher difficulty
    return limit / current;
}

} // namespace dinero
