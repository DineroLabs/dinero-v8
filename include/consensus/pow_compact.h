#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace dinero {

// nBits (compact) -> 32-byte BE target
inline std::array<uint8_t,32> TargetFromBitsBE(uint32_t bits) {
    std::array<uint8_t,32> t{};            // zero-init
    uint32_t exp  = bits >> 24;            // exponent (bytes)
    uint32_t mant = bits & 0x00ffffff;     // 24-bit mantissa (3 bytes)

    if (mant == 0 || exp == 0) return t;

    const uint8_t b0 = uint8_t((mant >> 16) & 0xFF);
    const uint8_t b1 = uint8_t((mant >>  8) & 0xFF);
    const uint8_t b2 = uint8_t( mant        & 0xFF);

    if (exp <= 3) {
        if (exp == 3)      { t[29] = b0; t[30] = b1; t[31] = b2; }
        else if (exp == 2) {            t[30] = b1; t[31] = b2; }
        else if (exp == 1) {                       t[31] = b2; }
    } else {
        // Bounds check: exp must be reasonable (4 <= exp <= 32)
        if (exp > 32) return t;  // Invalid exponent, return zero target
        const size_t idx = 32u - exp;     // safe: 4<=exp<=32 → 0<=idx<=28
        t[idx]   = b0;
        t[idx+1] = b1;
        t[idx+2] = b2;
    }
    return t;
}

// 32-byte BE target -> nBits (compact)
inline uint32_t BitsFromTargetBE(const std::array<uint8_t,32>& t) {
    size_t i = 0;
    while (i < 32 && t[i] == 0) ++i;
    if (i == 32) return 0;

    const int size = int(32 - i);     // significant bytes
    int exp = size;
    uint32_t mant = 0;

    if (size >= 3) {
        mant  = (uint32_t(t[i])   << 16)
              | (uint32_t(t[i+1]) <<  8)
              |  uint32_t(t[i+2]);
    } else if (size == 2) {
        mant  = (uint32_t(t[i])   << 16)
              | (uint32_t(t[i+1]) <<  8);
    } else { // size == 1
        mant  = (uint32_t(t[i])   << 16);
    }

    // Normalize if sign bit would be set
    if (mant & 0x00800000u) { mant >>= 8; ++exp; }

    return (uint32_t(exp) << 24) | (mant & 0x00ffffff);
}

// BE memcmp comparator
inline bool HashBelowTargetBE(const std::array<uint8_t,32>& hash,
                              const std::array<uint8_t,32>& target) {
    return std::memcmp(hash.data(), target.data(), 32) <= 0;
}

} // namespace dinero
