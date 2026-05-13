#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cctype>

namespace dinero {

// hex char -> nibble
inline int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    throw std::invalid_argument("Hex: non-hex char");
}

// generic hex -> bytes
inline std::vector<uint8_t> HexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) throw std::invalid_argument("Hex: odd length");
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1])));
    }
    return out;
}

// strict 32-byte hash (64 hex chars) in display BE
inline std::vector<uint8_t> HexToHash32BE(const std::string& hex64) {
    if (hex64.size() != 64) throw std::invalid_argument("HexToHash32BE: need 64 hex chars");
    return HexToBytes(hex64);
}

} // namespace dinero
