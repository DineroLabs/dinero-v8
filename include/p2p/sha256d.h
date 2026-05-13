#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// Forward declaration to avoid OpenSSL dependency in header
namespace din::crypto {
    std::array<uint8_t,32> sha256_once(const uint8_t* p, size_t n);
    std::array<uint8_t,32> sha256d(const uint8_t* p, size_t n);
}

// Convenience wrapper for existing Dinero crypto
inline std::array<uint8_t,32> sha256d(const uint8_t* p, size_t n) {
    return din::crypto::sha256d(p, n);
}
