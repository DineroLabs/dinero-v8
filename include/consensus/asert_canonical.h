#pragma once
#include <cstdint>

namespace dinero {

/**
 * ============================================================================
 * CANONICAL BITCOIN CASH NODE ASERT IMPLEMENTATION
 * ============================================================================
 *
 * This is a consensus-safe ASERT implementation using the exact algorithm
 * from Bitcoin Cash Node (BCH/eCash). It uses integer arithmetic with a
 * cubic polynomial approximation for 2^x calculation.
 *
 * Reference: https://github.com/bitcoin-cash-node/bitcoin-cash-node
 * Specification: doc/asert.md
 * Implementation: src/pow/aserti3-2d.cpp
 *
 * CRITICAL DIFFERENCES FROM CURRENT PRODUCTION CODE:
 * - NO floating-point arithmetic (std::exp2 is non-deterministic!)
 * - Uses 16-bit fixed-point arithmetic (radix 65,536)
 * - Cubic polynomial approximation with <0.013% error
 * - Guaranteed consensus safety across all platforms
 *
 * Mathematical Formula:
 * =====================
 * target_next = target_anchor × 2^exponent
 *
 * Where:
 *   exponent = (time_diff - height_diff × target_spacing) / half_life
 *   time_diff = current_MTP - anchor_MTP
 *   height_diff = current_height - anchor_height
 *
 * Integer Approximation for 2^x where x ∈ [0,1):
 * ===============================================
 * Let x = i + frac where i is integer part, frac is fractional part [0,1)
 *
 * 2^x = 2^i × 2^frac
 *
 * For 2^frac, use cubic polynomial (accurate to <0.013%):
 *
 *   2^frac ≈ (c0 + c1×frac + c2×frac² + c3×frac³) / c0
 *
 * With 16-bit fixed-point (radix R = 65,536):
 *   frac is in range [0, 65535]
 *   c0 = 65,536 (implicit, cancels in division)
 *   c1 = 195,766,423,245,049
 *   c2 = 971,821,376
 *   c3 = 5,127
 *
 * Result = 65,536 + ((c1×f + c2×f² + c3×f³ + round) >> 48)
 *
 * These coefficients are CONSENSUS-CRITICAL and must match BCH Node exactly.
 *
 * @param prev_height Previous block height
 * @param prev_median_time_past Previous block MedianTimePast
 * @param prev_bits Previous block difficulty (compact format)
 * @param candidate_time Current block timestamp
 * @param anchor_time Anchor block timestamp
 * @param anchor_bits Anchor block difficulty
 * @param anchor_height Anchor block height
 * @param pow_limit_bits Maximum allowed difficulty (easiest)
 * @param target_spacing Target seconds per block
 * @param half_life_seconds ASERT half-life parameter
 * @return Next block difficulty (compact format)
 */
uint32_t CalculateNextWork_ASERT_Canonical(
    int32_t prev_height,
    int64_t prev_median_time_past,
    uint32_t prev_bits,
    int64_t candidate_time,
    int64_t anchor_time,
    uint32_t anchor_bits,
    int32_t anchor_height,
    uint32_t pow_limit_bits,
    int64_t target_spacing,
    int64_t half_life_seconds);

/**
 * Helper: Convert compact difficulty format to 256-bit target
 * Returns target as arith_uint256 for precise calculations
 */
void CompactToTarget(uint32_t nBits, uint8_t target[32]);

/**
 * Helper: Convert 256-bit target to compact difficulty format
 */
uint32_t TargetToCompact(const uint8_t target[32]);

/**
 * Helper: Multiply 256-bit number by a scalar (for 2^exponent calculation)
 * result = value × multiplier / divisor
 */
void MultiplyTarget(const uint8_t value[32], uint64_t multiplier, uint64_t divisor, uint8_t result[32]);

/**
 * Helper: Right shift 256-bit number by n bits
 */
void ShiftRight(uint8_t value[32], int n);

/**
 * Helper: Left shift 256-bit number by n bits
 */
void ShiftLeft(uint8_t value[32], int n);

/**
 * Helper: Compare two 256-bit numbers
 * Returns: -1 if a < b, 0 if a == b, 1 if a > b
 */
int Compare(const uint8_t a[32], const uint8_t b[32]);

} // namespace dinero
