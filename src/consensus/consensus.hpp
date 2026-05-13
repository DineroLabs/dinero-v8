#pragma once
#include <cstdint>

// ============================================================================
// CONSENSUS PARAMETERS — Fair Launch v3
// ============================================================================
// SINGLE SOURCE OF TRUTH for all difficulty parameters.
//
// Difficulty schedule (no bootstrap):
// - Genesis (block 0): 0x1d31ffce (50x easier than Bitcoin, fixed)
// - Block 1+: ASERT per-block adjustment anchored at genesis
// - Floor: 0x1d31ffce (can never go easier than genesis)
// - Half-life: 12 hours (converges to 2-min blocks within ~1 day)
// ============================================================================

static constexpr int64_t COIN = 100'000'000;  // 1 DIN = 100,000,000 units (8 decimals)

// DAA type selection
enum class DAAType : uint8_t {
    BITCOIN_DAA,  // Original Bitcoin 2016-block adjustment
    ASERT,        // Anchor-based Smooth Elastic Retargeting (per-block)
    LWMA          // Linearly Weighted Moving Average (per-block)
};

struct Consensus {
    // Genesis difficulty (50x easier than Bitcoin genesis)
    uint32_t genesisBits         = 0x1d31ffce;

    // Target block spacing (2 minutes)
    uint32_t targetSpacingSec    = 120;

    // PoW limit floor (can never go easier than genesis)
    uint32_t powLimitBits        = 0x1d31ffce;

    // ASERT from block 1 — per-block smooth exponential adjustment
    // Anchored at genesis, converges to 2-min blocks within ~1 day
    DAAType daaType              = DAAType::ASERT;
    uint32_t asertAnchorHeight   = 0;            // Anchor at genesis
    uint32_t asertAnchorBits     = 0x1d31ffce;   // Genesis difficulty
    int64_t  asertHalfLifeSec    = 43'200;       // 12 hours = 360 blocks @ 2 min

    // Emergency min-difficulty (rescue floor if chain stalls catastrophically)
    uint32_t minDifficultyBits   = 0x1f00ffff;   // 256x easier than genesis

    // GPU mining governance (0 = disabled)
    uint32_t gpuMiningActivationHeight = 0;
    bool     allowGPUMining            = false;
};
