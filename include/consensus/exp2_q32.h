#pragma once
#include <cstdint>
#include <limits>

namespace dinero {

/**
 * Q32 fixed-point: 32 fractional bits.
 * 1.0 == (1ULL << 32)
 */
static inline uint64_t Q32_ONE() { return (1ULL << 32); }

/**
 * Return 2^x where x is Q32 (signed).
 * No floating point at runtime. Uses a LUT + linear interpolation.
 * Domain expected for ASERT is small (|x| << 64), we saturate if shifting would overflow.
 */
uint64_t Exp2_Q32(int64_t x_q32);

} // namespace dinero
