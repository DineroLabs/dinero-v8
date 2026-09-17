#pragma once

#include <cstdint>
#include <algorithm>
#include <type_traits>
#include "consensus/chain_identity.h"
#include "consensus/block_timing.h"
#include "primitives/amount.h"

namespace dinero {

/**
 * Dinero Monetary Policy — Fair Launch v3
 *
 * No premine. No hard cap. Legacy tail: 1 DIN/block; activated tail: 0.5 DIN/block.
 *
 * Genesis (height 0): 100 DIN burned via OP_RETURN (unspendable, symbolic)
 * PoW (height 1+): 100 DIN initial, halving every 1,314,000 blocks (~5 years)
 * Tail emission: max(halving_subsidy, 1 DIN) — kicks in at epoch 7 (~35 years)
 *
 * Decimals: 1 DIN = 100,000,000 una (Bitcoin standard)
 *
 * The 60-second upgrade retains the epoch heights and initial reward, and
 * changes the floor only for blocks at/above its network activation height.
 * Pure callers pass that height explicitly; omission preserves the legacy rule.
 *
 * Legacy supply curve (no hard cap — disinflationary). Figures below are computed
 * from the constants in this file at the 120-second target spacing
 * (262,980 blocks/year, so one 1,314,000-block epoch is 5.00 years):
 *   Epochs 0-6: 260.747M DIN from halvings (through height 9,198,000)
 *   Epoch 7+:   1 DIN/block = ~1.315M DIN per 5-year epoch
 *   Year 35:    ~260.75M DIN   (0.101%/yr inflation)
 *   Year 100:   ~277.85M DIN   (0.0947%/yr inflation → 0% asymptotically)
 *
 * The year-35 and year-100 rates are TAIL-ERA rates: forward-looking, at the
 * 1 DIN/block floor, i.e. 262,980 DIN/year over the supply at that point
 * (262,980 / 260,753,175 = 0.1009%; 262,980 / 277,846,875 = 0.0947%). Do not
 * confuse them with the rate during epoch 6, which is ~0.158%/yr — that epoch
 * still pays 1.5625 DIN/block, so a year-on-year delta measured ACROSS the
 * epoch-6/7 boundary reports the old, higher rate rather than the tail rate.
 *
 * Earlier revisions of this comment claimed ~346.55M DIN at year 100 and
 * inflation of 0.50%/0.38%/yr. Those did not follow from these constants —
 * at 1 DIN/block the tail adds only ~262,980 DIN/year, so year 100 is
 * ~277.85M, not ~346.55M. The executable logic was always correct; only this
 * prose was wrong. SubsidySchedule pins these figures so the comment cannot
 * drift from the code again.
 *
 * Network Magic: see src/consensus/chainparams_impl.cpp (Dinero mainnet — v7 restart)
 * Genesis Motto: "Dinero: Real Money For Free People - Post-Quantum Native. April 17 2026"
 */
struct ConsensusSubsidy {
    // Network identification.
    //
    // The P2P wire magic for each chain lives in
    // src/consensus/chainparams_impl.cpp and is read at runtime via
    // dinero::Params().magic. The constant that used to live here was
    // a hardcoded mainnet-only copy with zero callers — deleted to keep
    // exactly one truth path. If consensus code needs the magic, route
    // through Params(); do not re-introduce a local literal.
    static constexpr const char* NETWORK_NAME = "mainnet";
    static constexpr uint32_t PROTOCOL_VERSION = 20000;  // 2.0.0 (fair launch)

    // Genesis block identification (v7 restart)
    static constexpr const char* GENESIS_HASH = dinero::consensus::kMainnetGenesisHash.data();
    static constexpr const char* GENESIS_MOTTO = dinero::consensus::kGenesisMotto.data();
    static constexpr uint32_t GENESIS_TIME = 1776384000;  // 2026-04-17 00:00:00 UTC

    // Core monetary constants
    static constexpr uint64_t UNA_PER_DIN = 100000000ULL;               // 8 decimals
    static constexpr uint64_t INITIAL_SUBSIDY = 100ULL * UNA_PER_DIN;   // 100 DIN per block
    static constexpr uint32_t HALVING_INTERVAL = 1314000;                 // ~5 years @ 2 min blocks
    static constexpr uint64_t TAIL_EMISSION_UNA = 1ULL * UNA_PER_DIN;  // Legacy floor; activated floor is selected by height

    // Genesis unspendable (symbolic OP_RETURN burn)
    static constexpr uint64_t GENESIS_UNSPENDABLE_DIN  = 100ULL;
    static constexpr uint64_t GENESIS_UNSPENDABLE_UNA = GENESIS_UNSPENDABLE_DIN * UNA_PER_DIN;

    // Block heights
    static constexpr uint32_t GENESIS_HEIGHT = 0;

    /**
     * Get the block subsidy for a given height.
     *
     * SINGLE SOURCE OF TRUTH for all value creation.
     *
     * Height 0: Genesis (no spendable subsidy — OP_RETURN)
     * Height 1+: PoW emission with halvings and the height-selected tail floor
     */
    static constexpr uint64_t TailEmissionAtHeight(uint32_t height, uint32_t activation = UINT32_MAX) {
        return consensus::SixtySecondActive(height, activation) ? UNA_PER_DIN / 2 : TAIL_EMISSION_UNA;
    }

    static constexpr AmountUna GetBlockSubsidy(uint32_t height, uint32_t sixty_second_activation_height = UINT32_MAX) {
        // Genesis: unspendable OP_RETURN output
        if (height == 0) {
            return AmountUna::Zero();
        }

        // PoW emission starts at height 1
        uint32_t pow_blocks = height - 1;
        uint32_t halvings = pow_blocks / HALVING_INTERVAL;

        // Compute halving subsidy (shifts to 0 after 64 halvings)
        uint64_t subsidy = (halvings >= 64) ? 0 : (INITIAL_SUBSIDY >> halvings);

        // Height-selected floor: preserve the historical 1 DIN era; 0.5 DIN after activation.
        return AmountUna::Una(std::max(subsidy, TailEmissionAtHeight(height, sixty_second_activation_height)));
    }

    /**
     * Get total PoW coins issued from height 1 to height (inclusive).
     * Accounts for tail emission floor.
     */
    static uint64_t GetPoWIssuedAtHeight(uint32_t height, uint32_t sixty_second_activation_height = UINT32_MAX) {
        // Sum each era separately. A late activation must not recompute old
        // tail rewards at the new floor. uint64 arithmetic also covers height
        // UINT32_MAX without wrapping epoch boundaries or the genesis burn.
        const auto sum_to = [](uint32_t end, uint64_t floor) {
            uint64_t remaining = end;
            uint64_t total = 0;
            uint32_t epoch = 0;
            while (remaining != 0) {
                const uint64_t base = epoch >= 64 ? 0 : INITIAL_SUBSIDY >> epoch;
                if (base <= floor) return total + remaining * floor;
                const uint64_t blocks = std::min<uint64_t>(remaining, HALVING_INTERVAL);
                total += blocks * base;
                remaining -= blocks;
                ++epoch;
            }
            return total;
        };
        if (!consensus::SixtySecondActive(height, sixty_second_activation_height))
            return sum_to(height, TAIL_EMISSION_UNA);
        const uint32_t prefix = sixty_second_activation_height > 0
            ? sixty_second_activation_height - 1 : 0;
        return sum_to(prefix, TAIL_EMISSION_UNA) + sum_to(height, UNA_PER_DIN / 2) -
               sum_to(prefix, UNA_PER_DIN / 2);
    }

    /**
     * Get total coins issued at a given height.
     * Includes: genesis (unspendable) + PoW (with tail emission)
     */
    static uint64_t GetTotalIssuedAtHeight(uint32_t height, uint32_t sixty_second_activation_height = UINT32_MAX) {
        uint64_t genesis = (height >= GENESIS_HEIGHT) ? GENESIS_UNSPENDABLE_UNA : 0ULL;
        return genesis + GetPoWIssuedAtHeight(height, sixty_second_activation_height);
    }

    // ========================================================================
    // COMPILE-TIME ASSERTIONS
    // ========================================================================

    static_assert(UNA_PER_DIN == 100000000ULL, "Must use 8 decimals (Bitcoin standard)");
    static_assert(INITIAL_SUBSIDY == 10000000000ULL, "Initial subsidy must be 100 DIN");
    static_assert(HALVING_INTERVAL == 1314000, "Halving interval must be 1,314,000 blocks");
    static_assert(TAIL_EMISSION_UNA == 100000000ULL, "Tail emission must be 1 DIN");
    static_assert(GENESIS_HEIGHT == 0, "Genesis must be at height 0");
};

} // namespace dinero
