#pragma once
#include <array>
#include <cstdint>
#include "dinero/core/crypto/ripemd160.h"
#include "dinero/core/crypto/dinero_crypto_minimal.h"  // For global sha256 function

namespace dinero {
// Use the global sha256 function
inline std::array<uint8_t,32> SHA256(const uint8_t* data, size_t len) {
    std::array<uint8_t,32> out{};
    ::sha256(data, len, out.data());  // Call global sha256 function
    return out;
}

inline std::array<uint8_t,20> Hash160(const uint8_t* data, size_t len) {
    auto s = SHA256(data, len);
    return RIPEMD160(s.data(), s.size());
}
} // namespace dinero
