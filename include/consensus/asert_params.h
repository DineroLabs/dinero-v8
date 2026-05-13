#pragma once
#include <cstdint>

namespace dinero {

/**
 * ASERT Difficulty Adjustment — Default Parameters
 *
 * Dinero Difficulty Schedule (Fair Launch v3):
 * - Block 0: Genesis (fixed 0x1d31ffce)
 * - Block 1+: ASERT per-block adjustment (anchored at genesis)
 *
 * No bootstrap phase. ASERT from the very first mined block.
 */
struct ASERTConsensus {
    // Genesis difficulty (50x easier than Bitcoin genesis)
    static constexpr uint32_t GENESIS_BITS = 0x1d31ffce;
    static constexpr uint32_t DIFFICULTY_1_BITS = 0x1d31ffce;

    // ASERT half-life: 12 hours (43,200 seconds)
    static constexpr int64_t ASERT_HALF_LIFE_SEC = 43200;

    // Target block spacing: 2 minutes (120 seconds)
    static constexpr uint32_t TARGET_SPACING_SEC = 120;

    // ASERT anchored at genesis (block 0)
    static constexpr uint32_t ASERT_ANCHOR_HEIGHT = 0;
    static constexpr uint32_t ASERT_START_HEIGHT = 1;
    static constexpr uint32_t ASERT_ANCHOR_BITS = 0x1d31ffce;

    // PoW limit floor
    static constexpr uint32_t POW_LIMIT_BITS = 0x1d31ffce;

    // Emergency floor (256x easier, only if chain stalls)
    static constexpr uint32_t MIN_DIFFICULTY_BITS = 0x1f00ffff;

    // Assertions
    static_assert(ASERT_START_HEIGHT == ASERT_ANCHOR_HEIGHT + 1,
        "ASERT must start at height 1 (one after genesis anchor)");
    static_assert(ASERT_ANCHOR_BITS == GENESIS_BITS,
        "ASERT anchor bits must match genesis bits");
    static_assert(POW_LIMIT_BITS == GENESIS_BITS,
        "POW limit must match genesis difficulty");
    static_assert(TARGET_SPACING_SEC == 120,
        "Target spacing must be 120 seconds (2 minutes)");
    static_assert(ASERT_HALF_LIFE_SEC == 43200,
        "ASERT half-life must be 43,200 seconds (12 hours)");
};

} // namespace dinero
