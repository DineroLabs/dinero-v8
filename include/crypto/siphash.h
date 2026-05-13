// Copyright (c) 2025-2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// SipHash-2-4 — keyed hash function optimized for short inputs.
// Reference: Jean-Philippe Aumasson & Daniel J. Bernstein, 2012.
//
// Used by:
// - Compact block relay (short txid computation)
// - BIP158 Golomb-Coded Set block filters

#include <cstdint>
#include <cstring>
#include <vector>

namespace dinero {
namespace crypto {

namespace detail {

inline uint64_t rotl64(uint64_t x, int b) {
    return (x << b) | (x >> (64 - b));
}

inline void sipround(uint64_t& v0, uint64_t& v1, uint64_t& v2, uint64_t& v3) {
    v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0;
    v0 = rotl64(v0, 32);
    v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
    v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
    v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2;
    v2 = rotl64(v2, 32);
}

} // namespace detail

/// SipHash-2-4 for arbitrary-length data.
/// k0, k1 are the 128-bit key split into two 64-bit halves.
inline uint64_t SipHash24(uint64_t k0, uint64_t k1,
                          const uint8_t* data, size_t len) {
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;

    // Process full 8-byte words
    const size_t full_words = len / 8;
    for (size_t i = 0; i < full_words; ++i) {
        uint64_t m;
        std::memcpy(&m, data + i * 8, 8);
        v3 ^= m;
        detail::sipround(v0, v1, v2, v3);
        detail::sipround(v0, v1, v2, v3);
        v0 ^= m;
    }

    // Last word: remaining bytes + length in high byte
    uint64_t last = static_cast<uint64_t>(len & 0xFF) << 56;
    const uint8_t* tail = data + full_words * 8;
    size_t remaining = len & 7;
    switch (remaining) {
        case 7: last |= static_cast<uint64_t>(tail[6]) << 48; [[fallthrough]];
        case 6: last |= static_cast<uint64_t>(tail[5]) << 40; [[fallthrough]];
        case 5: last |= static_cast<uint64_t>(tail[4]) << 32; [[fallthrough]];
        case 4: last |= static_cast<uint64_t>(tail[3]) << 24; [[fallthrough]];
        case 3: last |= static_cast<uint64_t>(tail[2]) << 16; [[fallthrough]];
        case 2: last |= static_cast<uint64_t>(tail[1]) << 8;  [[fallthrough]];
        case 1: last |= static_cast<uint64_t>(tail[0]);        [[fallthrough]];
        case 0: break;
    }

    v3 ^= last;
    detail::sipround(v0, v1, v2, v3);
    detail::sipround(v0, v1, v2, v3);
    v0 ^= last;

    // Finalization: 4 rounds
    v2 ^= 0xFF;
    detail::sipround(v0, v1, v2, v3);
    detail::sipround(v0, v1, v2, v3);
    detail::sipround(v0, v1, v2, v3);
    detail::sipround(v0, v1, v2, v3);

    return v0 ^ v1 ^ v2 ^ v3;
}

/// Convenience: SipHash-2-4 for a std::vector<uint8_t>.
inline uint64_t SipHash24(uint64_t k0, uint64_t k1,
                          const std::vector<uint8_t>& data) {
    return SipHash24(k0, k1, data.data(), data.size());
}

} // namespace crypto
} // namespace dinero
