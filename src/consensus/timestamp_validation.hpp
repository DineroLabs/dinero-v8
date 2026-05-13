#pragma once
#include <cstdint>
#include <string>

/**
 * Timestamp Validation (Anti-Runaway Protection)
 *
 * Prevents blockchain bloat and timestamp manipulation attacks by enforcing:
 * 1. Block timestamps must advance past Median Time Past (MTP)
 * 2. Block timestamps cannot be more than 2 hours in the future
 * 3. Blocks must be at least 1 second apart
 *
 * These rules prevent:
 * - Time-warp attacks (rewinding difficulty)
 * - Spam attacks (mining thousands of blocks instantly)
 * - Blockchain bloat (even with high hashrate, blocks are rate-limited)
 *
 * References:
 * - Bitcoin BIP-113: https://github.com/bitcoin/bips/blob/master/bip-0113.mediawiki
 * - Median Time Past: Median of last 11 block timestamps
 */

namespace dinero {

/**
 * Maximum allowed future timestamp (2 hours)
 * Prevents nodes from accepting blocks with absurdly future timestamps
 */
constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;  // 2 hours in seconds

/**
 * Minimum time spacing between consecutive blocks (1 second)
 * Prevents instant block spam even with high hashrate
 */
constexpr int64_t MIN_BLOCK_SPACING = 1;  // 1 second

/**
 * Number of blocks to use for Median Time Past calculation
 * Bitcoin uses 11 blocks (current + 10 previous)
 */
constexpr int MTP_WINDOW_SIZE = 11;

/**
 * Calculate Median Time Past from block timestamps
 *
 * @param timestamps Array of recent block timestamps (sorted oldest to newest)
 * @param count Number of timestamps (up to MTP_WINDOW_SIZE)
 * @return Median timestamp
 */
inline int64_t CalculateMedianTimePast(const int64_t* timestamps, int count) {
    if (count == 0) return 0;
    if (count == 1) return timestamps[0];

    // Sort timestamps (copy to avoid modifying original)
    int64_t sorted[MTP_WINDOW_SIZE];
    for (int i = 0; i < count; ++i) {
        sorted[i] = timestamps[i];
    }

    // Bubble sort (small array, simple and deterministic)
    for (int i = 0; i < count - 1; ++i) {
        for (int j = 0; j < count - i - 1; ++j) {
            if (sorted[j] > sorted[j + 1]) {
                int64_t temp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = temp;
            }
        }
    }

    // Return median
    return sorted[count / 2];
}

/**
 * Validate block timestamp (CONSENSUS-CRITICAL)
 *
 * Enforces three rules:
 * 1. nTime > MTP(prev) - Must advance past median of previous blocks
 * 2. nTime <= now() + 2h - Cannot be more than 2 hours in future
 * 3. nTime >= prev->nTime + 1 - Must be at least 1 second after previous block
 *
 * @param blockTime Block's nTime field
 * @param prevBlockTime Previous block's nTime field
 * @param medianTimePast Median of last 11 block timestamps
 * @param networkAdjustedTime Current time from network consensus
 * @param errorOut Output parameter for error message (if validation fails)
 * @return true if timestamp is valid, false otherwise
 */
inline bool ValidateBlockTimestamp(
    int64_t blockTime,
    int64_t prevBlockTime,
    int64_t medianTimePast,
    int64_t networkAdjustedTime,
    std::string* errorOut = nullptr)
{
    // Rule 1: Block time must advance past MTP
    // This prevents rewinding the clock and manipulating difficulty
    if (blockTime <= medianTimePast) {
        if (errorOut) {
            *errorOut = "Block timestamp (" + std::to_string(blockTime) +
                       ") must be greater than median time past (" +
                       std::to_string(medianTimePast) + ")";
        }
        return false;
    }

    // Rule 2: Block time cannot be more than 2 hours in the future
    // Prevents nodes from accepting blocks with absurd future timestamps
    const int64_t maxAllowedTime = networkAdjustedTime + MAX_FUTURE_BLOCK_TIME;
    if (blockTime > maxAllowedTime) {
        if (errorOut) {
            *errorOut = "Block timestamp (" + std::to_string(blockTime) +
                       ") is too far in the future (max: " +
                       std::to_string(maxAllowedTime) + ")";
        }
        return false;
    }

    // Rule 3: Block time must be at least 1 second after previous block
    // Prevents spam attacks with identical or decreasing timestamps
    if (blockTime < prevBlockTime + MIN_BLOCK_SPACING) {
        if (errorOut) {
            *errorOut = "Block timestamp (" + std::to_string(blockTime) +
                       ") too close to previous block (" +
                       std::to_string(prevBlockTime) + "), minimum spacing: " +
                       std::to_string(MIN_BLOCK_SPACING) + " seconds";
        }
        return false;
    }

    // All checks passed
    return true;
}

/**
 * Check if timestamp would allow emergency difficulty (anti-stall check)
 *
 * This is NOT part of timestamp validation, but a helper for difficulty calculation.
 * Returns true if the time delta exceeds the stall threshold.
 *
 * @param currentMTP Current block's Median Time Past
 * @param prevMTP Previous block's Median Time Past
 * @param stallThresholdSeconds Threshold in seconds (typically 100 minutes = 6000 sec)
 * @return true if stall threshold exceeded (emergency difficulty should activate)
 */
inline bool IsChainStalled(
    int64_t currentMTP,
    int64_t prevMTP,
    int64_t stallThresholdSeconds)
{
    return (currentMTP - prevMTP) >= stallThresholdSeconds;
}

} // namespace dinero
