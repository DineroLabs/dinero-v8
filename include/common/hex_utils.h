#pragma once
#include <cstdint>
#include <string>
#include <array>

namespace dinero {

// Lowercase, 0-padded hex for a 32-bit value (no "0x" prefix)
inline std::string hex32(uint32_t v) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::array<char, 8> buf{};
    for (int i = 7; i >= 0; --i) {
        buf[7 - i] = kHex[(v >> (i * 4)) & 0xF];
    }
    return std::string(buf.data(), buf.size());
}

// Variant with 0x prefix (useful in logs)
inline std::string hex32_0x(uint32_t v) {
    return std::string("0x") + hex32(v);
}

} // namespace dinero
