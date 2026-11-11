#include "primitives/uint256.h"
#include <string_view>

namespace dinero {

bool uint256::operator==(const uint256& other) const {
    return std::memcmp(data, other.data, 32) == 0;
}

bool uint256::operator<(const uint256& other) const {
    // Compare in reverse order (big-endian comparison)
    for (int i = 31; i >= 0; --i) {
        if (data[i] < other.data[i]) return true;
        if (data[i] > other.data[i]) return false;
    }
    return false;
}

std::string uint256::ToString() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        const auto byte = data[31 - i];
        result[2 * i] = digits[byte >> 4];
        result[2 * i + 1] = digits[byte & 15];
    }
    return result;
}

bool uint256::IsNull() const {
    for (int i = 0; i < 32; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

void uint256::SetNull() {
    std::memset(data, 0, 32);
}

bool uint256::FromHex(const std::string& hex, uint256& out) {
    std::string_view input(hex);
    if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X')) {
        input.remove_prefix(2);
    }
    out.SetNull();
    if (input.size() > 64) return false;

    // Preserve short/odd/empty input, optional prefix, display endianness and
    // null-on-error semantics without constructing 32 locale-aware streams
    // for every persisted header hash read during cold startup.
    const auto nibble = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < input.size(); ++i) {
        const int value = nibble(static_cast<unsigned char>(input[input.size() - 1 - i]));
        if (value < 0) {
            out.SetNull();
            return false;
        }
        out.data[i / 2] |= static_cast<uint8_t>(value << ((i % 2) * 4));
    }
    return true;
}

uint256 uint256::FromHexUnsafe(const std::string& hex) {
    uint256 result;
    FromHex(hex, result);  // Ignores return value - returns null hash on error
    return result;
}

uint256 uint256S(const char* hex) {
    return uint256::FromHexUnsafe(hex);
}

} // namespace dinero
