#pragma once
#include "crypto/ripemd160.h"
#include "crypto/hash160.h"
#include "crypto/dinero_crypto_minimal.h"

namespace dinero {

// Canonical signatures expected by tests (C-style array outputs)
inline void ripemd160(const uint8_t* data, size_t len, uint8_t out[20]) {
    // Use global ripemd160 function
    ::ripemd160(data, len, out);
}

inline void hash160(const uint8_t* data, size_t len, uint8_t out[20]) {
    // Use global HASH160 function
    ::HASH160(data, len, out);
}

} // namespace dinero
