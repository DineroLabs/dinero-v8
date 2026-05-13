// amount.h - Dinero Currency Units
// Standardized currency system: DIN with 8 decimal places
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <cmath>

namespace dinero {

using Una = int64_t;                         // smallest integer unit
static constexpr Una UNA_PER_DIN = 100'000'000; // 8 decimals (like Bitcoin)

/**
 * Format una as DIN string with 8 decimal places
 * Examples:
 *   100000000 una -> "1.00000000"
 *   1 una -> "0.00000001"
 *   -50000000 una -> "-0.50000000"
 */
inline std::string FormatDIN(Una una) {
    bool neg = una < 0;
    uint64_t abs_una = neg
        ? static_cast<uint64_t>(-(una + 1)) + 1ULL
        : static_cast<uint64_t>(una);

    uint64_t whole = abs_una / static_cast<uint64_t>(UNA_PER_DIN);
    uint64_t frac  = abs_una % static_cast<uint64_t>(UNA_PER_DIN);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s%llu.%08llu",
                  neg ? "-" : "", (unsigned long long)whole, (unsigned long long)frac);
    return std::string(buf);
}

/**
 * Format una as unit string for logs/debug
 * Examples:
 *   1 -> "1 una"
 *   1000 -> "1000 una"
 *   -5 -> "-5 una"
 */
inline std::string FormatUna(Una una) {
    bool neg = una < 0; 
    uint64_t abs_una = neg
        ? static_cast<uint64_t>(-(una + 1)) + 1ULL
        : static_cast<uint64_t>(una);
    
    return std::string(neg ? "-" : "") + std::to_string(abs_una) +
           (una == 1 ? " una" : " una");
}

/**
 * Convert DIN (double) to una (int64_t)
 * Examples:
 *   0.1 -> 100000 una
 *   100.0 -> 100000000 una
 */
inline Una DINToUna(double din) {
    return static_cast<Una>(std::llround(din * UNA_PER_DIN));
}

/**
 * Convert una to DIN (double) - use sparingly, prefer FormatDIN for display
 */
inline double UnaToDIN(Una una) {
    return static_cast<double>(una) / UNA_PER_DIN;
}

// Common amounts as constants
namespace Amount {
    static constexpr Una ZERO = 0;
    static constexpr Una ONE_UNA = 1;
    static constexpr Una ONE_DIN = UNA_PER_DIN;
    static constexpr Una DUST_THRESHOLD = 546;  // 546 una dust limit
    static constexpr Una MIN_RELAY_FEE = 1000;  // 1000 una minimum relay fee
}

} // namespace dinero
