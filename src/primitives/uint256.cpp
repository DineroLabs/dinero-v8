#include "primitives/uint256.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cctype>

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
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 31; i >= 0; --i) {  // Output in big-endian order
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return oss.str();
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
    std::string clean_hex = hex;

    // Remove "0x" prefix if present
    if (clean_hex.size() >= 2 && clean_hex[0] == '0' && (clean_hex[1] == 'x' || clean_hex[1] == 'X')) {
        clean_hex = clean_hex.substr(2);
    }

    // Pad with leading zeros if needed
    if (clean_hex.size() < 64) {
        clean_hex = std::string(64 - clean_hex.size(), '0') + clean_hex;
    }

    // Check length
    if (clean_hex.size() != 64) {
        out.SetNull();
        return false;
    }

    // Validate and parse hex string (big-endian input)
    for (size_t i = 0; i < 32; ++i) {
        size_t idx = (31 - i) * 2;
        char high = clean_hex[idx];
        char low = clean_hex[idx + 1];

        // Validate hex characters
        if (!std::isxdigit(static_cast<unsigned char>(high)) ||
            !std::isxdigit(static_cast<unsigned char>(low))) {
            out.SetNull();
            return false;
        }

        // Parse byte
        std::string byte_str = clean_hex.substr(idx, 2);
        unsigned int byte_val;
        std::istringstream iss(byte_str);
        if (!(iss >> std::hex >> byte_val)) {
            out.SetNull();
            return false;
        }
        out.data[i] = static_cast<uint8_t>(byte_val);
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
