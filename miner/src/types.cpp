#include "solo_miner/types.h"
#include "solo_miner/miner.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace dinero {
namespace solo {

std::string minerBackendToString(MinerBackend backend) {
    switch (backend) {
        case MinerBackend::Auto:   return "auto";
        case MinerBackend::Cpu:    return "cpu";
        case MinerBackend::Metal:  return "metal";
        case MinerBackend::Cuda:   return "cuda";
        case MinerBackend::OpenCl: return "opencl";
    }
    return "unknown";
}

MinerBackend minerBackendFromString(const std::string& backend) {
    std::string normalized = backend;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (normalized == "auto" || normalized.empty()) return MinerBackend::Auto;
    if (normalized == "cpu") return MinerBackend::Cpu;
    if (normalized == "metal") return MinerBackend::Metal;
    if (normalized == "cuda") return MinerBackend::Cuda;
    if (normalized == "opencl" || normalized == "open-cl") return MinerBackend::OpenCl;
    throw std::invalid_argument("Unknown miner backend: " + backend);
}

Hash256 compactToTarget(uint32_t compact) {
    // Extract exponent and mantissa from compact target
    // Format: 0xEEMMMMM where EE is exponent, MMMMMM is mantissa
    uint32_t exponent = (compact >> 24) & 0xFF;
    uint32_t mantissa = compact & 0x007FFFFF;

    // Handle negative bit (bit 23 of compact)
    bool negative = (mantissa != 0) && ((compact & 0x00800000) != 0);
    if (negative) {
        mantissa &= 0x007FFFFF;
    }

    Hash256 target{};

    if (exponent <= 3) {
        // Target fits in first 3 bytes
        mantissa >>= 8 * (3 - exponent);
        target[31] = mantissa & 0xFF;
        target[30] = (mantissa >> 8) & 0xFF;
        target[29] = (mantissa >> 16) & 0xFF;
    } else {
        // Target spans more bytes
        size_t shift = exponent - 3;
        if (shift < 32) {
            size_t offset = 32 - shift - 1;
            if (offset < 32) target[offset] = mantissa & 0xFF;
            if (offset > 0) target[offset - 1] = (mantissa >> 8) & 0xFF;
            if (offset > 1) target[offset - 2] = (mantissa >> 16) & 0xFF;
        }
    }

    return target;
}

bool hashMeetsTarget(const Hash256& hash, const Hash256& target) {
    // Compare hash to target (little-endian, hash <= target)
    // Start from most significant byte
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true; // Equal is valid
}

std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);

    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t byte = 0;
        for (int j = 0; j < 2; j++) {
            char c = hex[i + j];
            uint8_t nibble;
            if (c >= '0' && c <= '9') nibble = c - '0';
            else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
            else throw std::invalid_argument("Invalid hex character");
            byte = (byte << 4) | nibble;
        }
        bytes.push_back(byte);
    }
    return bytes;
}

std::string bytesToHex(const uint8_t* data, size_t len) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result.push_back(hex_chars[(data[i] >> 4) & 0x0F]);
        result.push_back(hex_chars[data[i] & 0x0F]);
    }
    return result;
}

std::string bytesToHex(const std::vector<uint8_t>& data) {
    return bytesToHex(data.data(), data.size());
}

Hash256 reverseHash(const Hash256& hash) {
    Hash256 reversed;
    for (int i = 0; i < 32; i++) {
        reversed[i] = hash[31 - i];
    }
    return reversed;
}

} // namespace solo
} // namespace dinero
