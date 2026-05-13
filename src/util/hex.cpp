#include "util/hex.h"
#include <stdexcept>
#include <cctype>

namespace dinero {

static uint8_t fromHex(char c) {
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
    throw std::invalid_argument("invalid hex char");
}

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) throw std::invalid_argument("hex length must be even");
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i=0; i<hex.size(); i+=2) {
        uint8_t hi = fromHex(hex[i]);
        uint8_t lo = fromHex(hex[i+1]);
        out.push_back((hi << 4) | lo);
    }
    return out;
}

std::vector<uint8_t> HexToBytesLE(const std::string& hex) {
    auto be = HexToBytes(hex);
    return std::vector<uint8_t>(be.rbegin(), be.rend()); // reverse to LE
}

}
