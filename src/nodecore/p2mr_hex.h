#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dinero::nodecore {
inline std::vector<uint8_t> DecodeP2mrHex(const std::string& hex) {
    const auto nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("bad hex");
    };
    if (hex.size() % 2 != 0) throw std::runtime_error("bad hex");
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<uint8_t>((nibble(hex[2*i]) << 4) | nibble(hex[2*i+1]));
    return out;
}
} // namespace dinero::nodecore
