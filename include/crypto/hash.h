#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace din::crypto {

// 32-byte and 20-byte digests
using Sha256Hash = std::array<uint8_t, 32>;
using Ripemd160  = std::array<uint8_t, 20>;

// One-shot helpers (C++17 compatible)
Sha256Hash SHA256(const uint8_t* data, std::size_t size);
Sha256Hash SHA256D(const uint8_t* data, std::size_t size); // double SHA-256
Ripemd160  RIPEMD160(const uint8_t* data, std::size_t size);
Ripemd160  HASH160(const uint8_t* data, std::size_t size); // RIPEMD160(SHA256(data))

// Convenience overloads for std::vector
inline Sha256Hash SHA256(const std::vector<uint8_t>& data) {
    return SHA256(data.data(), data.size());
}
inline Sha256Hash SHA256D(const std::vector<uint8_t>& data) {
    return SHA256D(data.data(), data.size());
}
inline Ripemd160 RIPEMD160(const std::vector<uint8_t>& data) {
    return RIPEMD160(data.data(), data.size());
}
inline Ripemd160 HASH160(const std::vector<uint8_t>& data) {
    return HASH160(data.data(), data.size());
}

// Call once on startup; returns true if legacy provider (RIPEMD160) is available.
bool OpenSSL_EnsureProviders();

// Self-test with known vectors to verify correctness
bool OpenSSL_SelfTest();

} // namespace din::crypto
