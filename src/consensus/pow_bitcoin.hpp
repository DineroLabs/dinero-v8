#pragma once
#include "consensus.hpp"
#include "consensus/pow_compact.h"
#include <array>
#include <cstdint>
#include <algorithm>
#include <functional>

/**
 * Bitcoin-Style Difficulty Adjustment (with Emergency Adjustment)
 *
 * This implements Bitcoin's proven difficulty algorithm with an added
 * emergency adjustment mechanism to prevent chain stalling.
 *
 * == NORMAL RETARGETING (every 2016 blocks) ==
 * Formula:
 *   new_target = old_target * (actual_time / expected_time)
 *   where:
 *     actual_time = time span of last 2016 blocks
 *     expected_time = 2016 * 5 minutes = 7 days
 *
 * Clamp: new difficulty cannot change by more than ×2 or ÷2
 * (Bitcoin uses ×4/÷4, we use ×2/÷2 for more conservative adjustments)
 *
 * == EMERGENCY ADJUSTMENT ==
 * Trigger: If average of last 10 blocks > 30 minutes
 * Action: Halve difficulty (double target) immediately
 * Purpose: Prevent chain from getting stuck if hash rate crashes
 *
 * This emergency mechanism is insurance you hope to never use, but will
 * be grateful for if needed. It protects against the #1 killer of small
 * PoW coins: stuck difficulty after miner exodus.
 *
 * References:
 * - Bitcoin Core: src/pow.cpp GetNextWorkRequired()
 * - BIP 34: Block v2, Height in Coinbase
 */

namespace dinero {

/**
 * Calculate Bitcoin-style difficulty target with emergency adjustment
 *
 * @param currentHeight Current block height
 * @param pindexPrev Previous block index (for timestamp/bits access)
 * @param pindexFirst First block of retarget interval (height = currentHeight - 2016)
 * @param c Consensus parameters
 * @param getBlockTime Lambda to get block timestamp by height
 * @param getBlockBits Lambda to get block nBits by height
 * @return New difficulty target (compact bits format)
 */
inline uint32_t CalculateBitcoinTarget(
    uint32_t currentHeight,
    int64_t prevBlockTime,
    uint32_t prevBlockBits,
    int64_t firstBlockTime,
    const Consensus& c,
    std::function<int64_t(uint32_t)> getBlockTime = nullptr)
{
    // ========== EMERGENCY DIFFICULTY ADJUSTMENT ==========
    // Check if last 10 blocks took too long (average > 30 min)
    // This prevents chain from getting stuck if hash rate crashes

    if (getBlockTime && currentHeight >= 11) {
        const int CHECK_WINDOW = 10;                  // Last 10 blocks
        const int64_t EMERGENCY_THRESHOLD = 30 * 60;  // 30 minutes

        int64_t total_spacing = 0;
        int valid_samples = 0;

        // Collect timestamps for last 10 blocks
        for (int i = 0; i < CHECK_WINDOW; ++i) {
            int64_t t1 = getBlockTime(currentHeight - i - 1);
            int64_t t2 = getBlockTime(currentHeight - i - 2);

            if (t1 > 0 && t2 > 0 && t1 > t2) {
                total_spacing += (t1 - t2);
                valid_samples++;
            }
        }

        // Require at least 3 valid samples to avoid false positives
        if (valid_samples >= 3) {
            int64_t avg_spacing = total_spacing / valid_samples;

            if (avg_spacing > EMERGENCY_THRESHOLD) {
                // EMERGENCY: Double the target (halve difficulty)
                auto target = TargetFromBitsBE(prevBlockBits);

                // Left shift by 1 (multiply by 2)
                uint8_t carry = 0;
                for (int j = 31; j >= 0; --j) {
                    uint16_t temp = (static_cast<uint16_t>(target[j]) << 1) | carry;
                    target[j] = static_cast<uint8_t>(temp & 0xFF);
                    carry = static_cast<uint8_t>((temp >> 8) & 0x01);
                }

                // Clamp to PoW limit
                const auto powLimit = TargetFromBitsBE(c.powLimitBits);
                bool exceedsLimit = false;
                for (int i = 0; i < 32; ++i) {
                    if (target[i] > powLimit[i]) {
                        exceedsLimit = true;
                        break;
                    } else if (target[i] < powLimit[i]) {
                        break;
                    }
                }

                if (exceedsLimit) {
                    target = powLimit;
                }

                std::printf("[EDA] ⚠️ Emergency Difficulty Triggered! avg=%.1fs (%d samples) - Halving difficulty\n",
                           static_cast<double>(avg_spacing), valid_samples);

                return BitsFromTargetBE(target);
            }
        }
    }

    // ========== NORMAL BITCOIN-STYLE RETARGETING ==========
    // Only retarget every 2016 blocks

    if (currentHeight % c.retargetIntervalBlk != 0) {
        // Not a retarget block, return previous difficulty
        return prevBlockBits;
    }

    // Calculate actual time span of last 2016 blocks
    int64_t actualTimespan = prevBlockTime - firstBlockTime;

    // Expected timespan: 2016 blocks * 5 minutes = 604,800 seconds (7 days)
    const int64_t expectedTimespan = c.retargetIntervalBlk * c.targetSpacingSec;

    // Clamp actual timespan to ×2/÷2 range (more conservative than Bitcoin's ×4/÷4)
    // This prevents difficulty from changing too dramatically
    const int64_t minTimespan = expectedTimespan / 2;  // 3.5 days
    const int64_t maxTimespan = expectedTimespan * 2;  // 14 days

    if (actualTimespan < minTimespan) {
        actualTimespan = minTimespan;
    }
    if (actualTimespan > maxTimespan) {
        actualTimespan = maxTimespan;
    }

    // Get previous target from compact bits
    auto oldTarget = TargetFromBitsBE(prevBlockBits);

    // Calculate new target: old_target * (actual_time / expected_time)
    // We do this using 256-bit arithmetic to avoid overflow

    // Multiply target by actualTimespan
    std::array<uint8_t, 32> newTarget = {};
    uint64_t carry = 0;

    for (int i = 31; i >= 0; --i) {
        uint64_t product = static_cast<uint64_t>(oldTarget[i]) * static_cast<uint64_t>(actualTimespan) + carry;
        newTarget[i] = static_cast<uint8_t>(product & 0xFF);
        carry = product >> 8;
    }

    // Divide by expectedTimespan (long division on 256-bit number)
    uint64_t remainder = 0;
    for (int i = 0; i < 32; ++i) {
        uint64_t dividend = (remainder << 8) | newTarget[i];
        newTarget[i] = static_cast<uint8_t>(dividend / expectedTimespan);
        remainder = dividend % expectedTimespan;
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
