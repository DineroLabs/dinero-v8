#include "primitives/uint256.h"
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {
// The original released parser is the differential oracle. Keep its stream
// conversion here so these checks do not mirror the optimized implementation.
bool ReleasedParser(std::string hex, dinero::uint256& out) {
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex = hex.substr(2);
    if (hex.size() < 64) hex = std::string(64 - hex.size(), '0') + hex;
    if (hex.size() != 64) { out.SetNull(); return false; }
    for (size_t i = 0; i < 32; ++i) {
        const size_t at = (31 - i) * 2;
        if (!std::isxdigit(static_cast<unsigned char>(hex[at])) ||
            !std::isxdigit(static_cast<unsigned char>(hex[at + 1]))) { out.SetNull(); return false; }
        unsigned value;
        std::istringstream input(hex.substr(at, 2));
        if (!(input >> std::hex >> value)) { out.SetNull(); return false; }
        out.data[i] = static_cast<uint8_t>(value);
    }
    return true;
}
std::string ReleasedFormat(const dinero::uint256& value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (int i = 31; i >= 0; --i) output << std::setw(2) << unsigned(value.data[i]);
    return output.str();
}
void Require(bool value) { if (!value) throw std::runtime_error("uint256 hex compatibility failed"); }
uint64_t checked = 0;
void Compare(const std::string& input) {
    dinero::uint256 old_value, new_value;
    std::fill(old_value.begin(), old_value.end(), 0xff);
    std::fill(new_value.begin(), new_value.end(), 0xff);
    const bool old_ok = ReleasedParser(input, old_value);
    const bool new_ok = dinero::uint256::FromHex(input, new_value);
    Require(old_ok == new_ok && old_value == new_value);
    Require(old_ok || new_value.IsNull());
    Require(new_value.GetHex() == ReleasedFormat(old_value));
    ++checked;
}
}
int main() {
    for (const auto* value : {"", "0x", "0X", "0", "f", "abc", "0XABC", " 1", "-1", "+1", "0x0x1"}) Compare(value);
    // Every possible pair of input bytes, including embedded NUL/high bytes.
    for (unsigned a = 0; a < 256; ++a) for (unsigned b = 0; b < 256; ++b)
        Compare(std::string{char(a), char(b)});
    // Every character at every nibble position: full and partial failures
    // must clear a previously nonzero output, regardless of parse direction.
    for (size_t at = 0; at < 64; ++at) for (unsigned c = 0; c < 256; ++c) {
        std::string text(64, 'a'); text[at] = char(c); Compare(text);
    }
    std::mt19937_64 random(0x150074);
    for (size_t length = 0; length <= 66; ++length) for (unsigned n = 0; n < 16; ++n) {
        std::string text(length, '0');
        for (auto& c : text) c = "0123456789aBcDeF"[random() % 16];
        Compare(text); Compare("0x" + text); Compare("0X" + text);
    }
    for (unsigned n = 0; n < 1024; ++n) {
        dinero::uint256 value;
        for (auto& byte : value) byte = uint8_t(random());
        const auto hex = value.GetHex();
        Require(hex.size() == 64 && hex == ReleasedFormat(value));
        Require(dinero::uint256::FromHexUnsafe(hex) == value);
    }
    const auto vector = dinero::uint256::FromHexUnsafe("00000000c7b0a0a1f94547bf4cb6a6c8446a7ac3e50682c01477685b46939fd2");
    Require(vector.data[0] == 0xd2 && vector.data[31] == 0);
    Require(vector.GetHex() == "00000000c7b0a0a1f94547bf4cb6a6c8446a7ac3e50682c01477685b46939fd2");
    std::cout << "uint256 hexadecimal compatibility: " << checked << " parser comparisons and 1024 byte roundtrips passed\n";
}
