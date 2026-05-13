#pragma once
#include <cstdint>
#include "consensus/chainwork.h"

namespace dinero {

// Extract the LOW 64 bits of an arith_uint256 and return them as LE bytes.
// Works on all platforms - arith_uint256 has GetWord() method everywhere.
inline uint64_t Low64LE(const arith_uint256& a) {
    return a.GetWord(0);  // GetWord(0) returns low 64 bits in native byte order
}

} // namespace dinero
