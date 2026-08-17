#pragma once
#include "consensus.hpp"
#include "consensus/pow_compact.h"
#include <array>
#include <cstdint>
#include <algorithm>

/**
 * ASERT (Anchor-based Smooth Elastic Retargeting)
 *
 * Per-block difficulty adjustment algorithm that provides smooth,
 * responsive difficulty changes without oscillation.
 *
 * Formula:
 *   new_target = anchor_target * 2^(excess_time / half_life)
 *
 * Where:
 *   excess_time = (time_delta - ideal_time)
 *   time_delta = current_time - anchor_time
 *   ideal_time = (current_height - anchor_height) * target_spacing
 *
 * Positive excess_time → blocks too slow → difficulty decreases (target increases)
 * Negative excess_time → blocks too fast → difficulty increases (target decreases)
 *
 * Half-life: Time for difficulty to double (or halve). Typical: 12 hours (144 blocks)
 *
 * References:
 * - Bitcoin Cash ASERT: https://gitlab.com/bitcoin-cash-node/bchn-sw/bitcoincash-upgrade-specifications/-/blob/master/spec/2020-11-15-asert.md
 * - Zcash NU5: https://zips.z.cash/zip-0208
 */

namespace dinero {

/**
 * Calculate ASERT target using integer fixed-point arithmetic
 *
 * @param currentHeight Current block height
 * @param currentTime Current block timestamp (MTP or nTime)
 * @param anchorHeight Anchor block height (usually last Phase 1 block)
 * @param anchorTime Anchor block timestamp
 * @param anchorBits Anchor block difficulty (compact bits format)
 * @param c Consensus parameters
 * @return New difficulty target (compact bits format)
 */
inline uint32_t CalculateASERT_Target(
    uint32_t currentHeight,
    int64_t currentTime,
    uint32_t anchorHeight,
    int64_t anchorTime,
    uint32_t anchorBits,
    const Consensus& c)
{
    // Calculate time and height deltas since anchor
    const int64_t heightDelta = static_cast<int64_t>(currentHeight) - static_cast<int64_t>(anchorHeight);
    const int64_t timeDelta = currentTime - anchorTime;

    // Ideal time if blocks came exactly every targetSpacingSec
    const int64_t idealTime = heightDelta * static_cast<int64_t>(c.targetSpacingSec);

    // Excess time: positive = slow (decrease difficulty), negative = fast (increase difficulty)
    const int64_t excessTime = timeDelta - idealTime;

    // Get anchor target from compact bits
    auto anchorTarget = TargetFromBitsBE(anchorBits);

    // If no excess time, return anchor difficulty unchanged
    if (excessTime == 0) {
        return anchorBits;
    }

    // ========== FIXED-POINT EXPONENT CALCULATION ==========
    // We need to compute: 2^(excessTime / halfLife)
    // Using 16-bit fixed-point fractional arithmetic

    const int64_t halfLife = c.asertHalfLifeSec;

    // Compute integer and fractional parts
    // exponent_fp = (excessTime * 2^16) / halfLife  (16.16 fixed-point)
    //
    // NB: this is a signed MULTIPLY, not `excessTime << 16`. excessTime is
    // negative on the fast-block branch, and left-shifting a negative signed
    // value is UNDEFINED BEHAVIOR before C++20 — and the iOS/NodeCore toolchain
    // still builds this consensus code with -std=c++17. The multiply is
    // well-defined in every standard and, on two's-complement platforms, yields
    // the exact value the arithmetic shift produced, so the ASERT result is
    // unchanged. (excessTime is bounded by the 32-bit header timestamp, so the
    // 2^16 scaling cannot overflow int64 for any real block — the same bound the
    // shift already relied on.)
    int64_t exponent_fp = (excessTime * 65536) / halfLife;

    // Extract integer part (k) and fractional part (r)
    int64_t k = exponent_fp >> 16;  // Integer part
    int64_t r = exponent_fp & 0xFFFF;  // Fractional part (0..65535)

    // ========== APPLY INTEGER SHIFT (2^k) ==========
    std::array<uint8_t, 32> newTarget = anchorTarget;

    // Clamp k to prevent absurd shifts
    // Maximum shift: ±32 (prevents overflow/underflow)
    if (k > 32) k = 32;
    if (k < -32) k = -32;

    // Apply integer power of 2 via bit shifts
    if (k > 0) {
        // Shift left (easier difficulty)
        for (int64_t i = 0; i < k; ++i) {
            // Left shift entire 256-bit target
            uint8_t carry = 0;
            for (int j = 31; j >= 0; --j) {
                uint16_t temp = (static_cast<uint16_t>(newTarget[j]) << 1) | carry;
                newTarget[j] = static_cast<uint8_t>(temp & 0xFF);
                carry = static_cast<uint8_t>((temp >> 8) & 0x01);
            }
        }
    } else if (k < 0) {
        // Shift right (harder difficulty)
        for (int64_t i = 0; i < -k; ++i) {
            // Right shift entire 256-bit target
            uint8_t carry = 0;
            for (int j = 0; j < 32; ++j) {
                uint16_t temp = static_cast<uint16_t>(newTarget[j]) | (static_cast<uint16_t>(carry) << 8);
                carry = static_cast<uint8_t>(temp & 0x01);
                newTarget[j] = static_cast<uint8_t>(temp >> 1);
            }
        }
    }

    // ========== APPLY FRACTIONAL ADJUSTMENT (2^r) ==========
    // For fractional part, use linear approximation (good enough for small r)
    // 2^(r/65536) ≈ 1 + r/65536 for small r
    // More precise: use Taylor series or lookup table

    // Simple linear approximation for now:
    // newTarget *= (1 + r/65536)
    // = newTarget + newTarget * r / 65536

    if (r != 0) {
        // Calculate fractional adjustment: newTarget * r / 65536
        // Use 64-bit arithmetic to avoid overflow
        std::array<uint8_t, 32> fractionalPart = {};

        // Multiply newTarget by r (treating as 32-byte big-endian integer)
        uint64_t carry = 0;
        for (int i = 31; i >= 0; --i) {
            uint64_t product = static_cast<uint64_t>(newTarget[i]) * static_cast<uint64_t>(r) + carry;
            fractionalPart[i] = static_cast<uint8_t>(product & 0xFF);
            carry = product >> 8;
        }

        // Divide by 65536 (right shift by 16 bits)
        for (int shift = 0; shift < 16; ++shift) {
            uint8_t carry_bit = 0;
            for (int i = 0; i < 32; ++i) {
                uint16_t temp = static_cast<uint16_t>(fractionalPart[i]) | (static_cast<uint16_t>(carry_bit) << 8);
                carry_bit = static_cast<uint8_t>(temp & 0x01);
                fractionalPart[i] = static_cast<uint8_t>(temp >> 1);
            }
        }

        // Add fractional adjustment to newTarget
        if (excessTime > 0) {
            // Blocks too slow → increase target (easier)
            uint8_t carry_add = 0;
            for (int i = 31; i >= 0; --i) {
                uint16_t sum = static_cast<uint16_t>(newTarget[i]) +
                             static_cast<uint16_t>(fractionalPart[i]) +
                             carry_add;
                newTarget[i] = static_cast<uint8_t>(sum & 0xFF);
                carry_add = static_cast<uint8_t>((sum >> 8) & 0x01);
            }
        } else {
            // Blocks too fast → decrease target (harder)
            // Subtract fractional part
            int8_t borrow = 0;
            for (int i = 31; i >= 0; --i) {
                int16_t diff = static_cast<int16_t>(newTarget[i]) -
                              static_cast<int16_t>(fractionalPart[i]) -
                              borrow;
                if (diff < 0) {
                    newTarget[i] = static_cast<uint8_t>(diff + 256);
                    borrow = 1;
                } else {
                    newTarget[i] = static_cast<uint8_t>(diff);
                    borrow = 0;
                }
            }
        }
    }

    // ========== CLAMP TO POW LIMIT ==========
    const auto powLimit = TargetFromBitsBE(c.powLimitBits);

    bool exceedsLimit = false;
    for (int i = 0; i < 32; ++i) {
        if (newTarget[i] > powLimit[i]) {
            exceedsLimit = true;
            break;
        } else if (newTarget[i] < powLimit[i]) {
            break;
        }
    }

    if (exceedsLimit) {
        newTarget = powLimit;
    }

    // Prevent zero target
    bool isZero = true;
    for (int i = 0; i < 32; ++i) {
        if (newTarget[i] != 0) {
            isZero = false;
            break;
        }
    }
    if (isZero) {
        newTarget[31] = 1;  // Minimum non-zero target
    }

    return BitsFromTargetBE(newTarget);
}

} // namespace dinero
